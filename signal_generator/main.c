// i8088 slow-clock signal generator (RP2040 / Pico 1).
//
// Generates a 1-100 Hz, 33%-duty clock for single-stepping an Intel 80C88 (CMOS)
// CPU. The potentiometer sets the frequency in AUTO mode; the MODE button toggles
// to MANUAL single-step, where each STEP press emits exactly one clock cycle.
// A 1602 LCD (PCF8574 I2C backpack) shows frequency, period and pulse (high) time.
//
// The 80C88 CLK input needs ~3.9-4.2 V logic-high and a 33% duty cycle, so GP15
// must drive the CPU through a 74HCT buffer (3.3 V -> 5 V). The original NMOS 8088
// CANNOT run this slow (2 MHz floor) - use the static CMOS 80C88. See CLAUDE.md.
//
// Core 1 is a dedicated clock engine (jitter-free); core 0 runs the UI.
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "lcd1602_i2c.h"

// ----- Pin assignments -------------------------------------------------------
#define CLK_GPIO        15      // clock output -> 74HCT buffer -> 80C88 CLK
#define MODE_BTN_GPIO   14      // toggles AUTO/MANUAL, active-low to GND
#define STEP_BTN_GPIO   13      // single-step in MANUAL, active-low to GND
#define POT_ADC_GPIO    26      // potentiometer wiper -> ADC0
#define POT_ADC_CHANNEL 0
#define STATUS_LED_GPIO PICO_DEFAULT_LED_PIN // on = MANUAL mode

// ----- LCD (PCF8574 I2C backpack) -------------------------------------------
#define LCD_I2C        i2c0
#define LCD_SDA_GPIO   4
#define LCD_SCL_GPIO   5
#define LCD_I2C_HZ     100000
#define LCD_ADDR       0x27     // 0x3F for PCF8574A backpacks
#define LCD_COLUMNS    16

// ----- Frequency / clock shaping --------------------------------------------
#define FREQ_MIN_HZ        1.0f
#define FREQ_MAX_HZ        100.0f
#define ADC_FULL_SCALE     4095.0f
#define DUTY_HIGH_FRACTION (1.0f / 3.0f) // 33% duty: high time = period / 3
#define US_PER_SECOND      1000000.0f
#define US_PER_MS          1000.0f

// ----- Timing / smoothing ----------------------------------------------------
#define ADC_SMOOTH_ALPHA  0.20f  // EMA weight for the new sample
#define DEBOUNCE_US       25000  // button debounce window
#define LCD_REFRESH_US    150000 // LCD redraw interval
#define MANUAL_IDLE_US    200    // core-1 idle poll while waiting for a step

// ----- Shared state (core 0 writes, core 1 reads) ---------------------------
static critical_section_t s_clk_lock;
static volatile uint32_t  s_high_us = 3333;   // high phase width
static volatile uint32_t  s_low_us  = 6667;   // low phase width
static volatile bool      s_manual  = false;  // AUTO (false) / MANUAL (true)
static volatile bool      s_step_request = false;
static volatile uint32_t  s_step_count   = 0;

// Drive one full clock cycle: high for high_us, then low for low_us.
static void emit_clock_cycle(uint32_t high_us, uint32_t low_us) {
    gpio_put(CLK_GPIO, 1);
    busy_wait_us(high_us);
    gpio_put(CLK_GPIO, 0);
    busy_wait_us(low_us);
}

// Core 1: nothing but clock generation, so the waveform stays jitter-free.
static void core1_clock_engine(void) {
    while (true) {
        uint32_t high_us;
        uint32_t low_us;
        critical_section_enter_blocking(&s_clk_lock);
        high_us = s_high_us;
        low_us  = s_low_us;
        bool manual = s_manual;
        critical_section_exit(&s_clk_lock);

        if (!manual) {
            emit_clock_cycle(high_us, low_us);
        } else if (s_step_request) {
            s_step_request = false;
            emit_clock_cycle(high_us, low_us);
            s_step_count++;
        } else {
            gpio_put(CLK_GPIO, 0);
            busy_wait_us(MANUAL_IDLE_US);
        }
    }
}

// Read the pot (smoothed) and return the mapped frequency in Hz (1..100).
static float read_frequency_hz(float *adc_ema) {
    uint16_t raw = adc_read();
    *adc_ema += (raw - *adc_ema) * ADC_SMOOTH_ALPHA;
    float fraction = *adc_ema / ADC_FULL_SCALE;
    return FREQ_MIN_HZ + fraction * (FREQ_MAX_HZ - FREQ_MIN_HZ);
}

// Convert a frequency into shared high/low widths under the lock.
static void publish_timing(float freq_hz) {
    float period_us = US_PER_SECOND / freq_hz;
    uint32_t high_us = (uint32_t)(period_us * DUTY_HIGH_FRACTION);
    uint32_t low_us  = (uint32_t)(period_us) - high_us;
    critical_section_enter_blocking(&s_clk_lock);
    s_high_us = high_us;
    s_low_us  = low_us;
    critical_section_exit(&s_clk_lock);
}

// Active-low button with debounce. Returns true once per press (falling edge).
typedef struct {
    uint     gpio;
    bool     was_down;
    uint64_t last_change_us;
} debounced_button_t;

static void button_init(debounced_button_t *button, uint gpio) {
    button->gpio = gpio;
    button->was_down = false;
    button->last_change_us = 0;
    gpio_init(gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio);
}

static bool button_pressed(debounced_button_t *button) {
    bool is_down = !gpio_get(button->gpio); // active-low
    uint64_t now = time_us_64();
    if (is_down != button->was_down &&
        (now - button->last_change_us) > DEBOUNCE_US) {
        button->was_down = is_down;
        button->last_change_us = now;
        return is_down; // fire on the press, not the release
    }
    return false;
}

// Render the two LCD lines from the current frequency and mode.
static void update_display(float freq_hz) {
    float period_ms = US_PER_SECOND / freq_hz / US_PER_MS;
    float high_ms   = period_ms * DUTY_HIGH_FRACTION;
    char line[LCD_COLUMNS + 1];

    lcd_set_cursor(0, 0);
    if (s_manual) {
        snprintf(line, sizeof(line), "MAN  step%6lu", (unsigned long)s_step_count);
    } else {
        snprintf(line, sizeof(line), "AUTO f=%6.1fHz", (double)freq_hz);
    }
    lcd_print(line);

    lcd_set_cursor(0, 1);
    snprintf(line, sizeof(line), "T=%6.1f h=%5.1f", (double)period_ms, (double)high_ms);
    lcd_print(line);
}

int main(void) {
    stdio_init_all();

    // Clock output.
    gpio_init(CLK_GPIO);
    gpio_set_dir(CLK_GPIO, GPIO_OUT);
    gpio_put(CLK_GPIO, 0);

    // Status LED.
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_put(STATUS_LED_GPIO, 0);

    // Potentiometer ADC.
    adc_init();
    adc_gpio_init(POT_ADC_GPIO);
    adc_select_input(POT_ADC_CHANNEL);

    // LCD over I2C.
    i2c_init(LCD_I2C, LCD_I2C_HZ);
    gpio_set_function(LCD_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(LCD_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(LCD_SDA_GPIO);
    gpio_pull_up(LCD_SCL_GPIO);
    lcd_init(LCD_I2C, LCD_ADDR);

    // Buttons.
    debounced_button_t mode_button;
    debounced_button_t step_button;
    button_init(&mode_button, MODE_BTN_GPIO);
    button_init(&step_button, STEP_BTN_GPIO);

    // Seed timing before launching the clock engine on core 1.
    float adc_ema = adc_read();
    publish_timing(read_frequency_hz(&adc_ema));
    critical_section_init(&s_clk_lock);
    multicore_launch_core1(core1_clock_engine);

    uint64_t next_lcd_update = 0;
    while (true) {
        if (button_pressed(&mode_button)) {
            s_manual = !s_manual;
            gpio_put(STATUS_LED_GPIO, s_manual);
        }
        if (button_pressed(&step_button) && s_manual) {
            s_step_request = true;
        }

        float freq_hz = read_frequency_hz(&adc_ema);
        publish_timing(freq_hz);

        uint64_t now = time_us_64();
        if (now >= next_lcd_update) {
            update_display(freq_hz);
            next_lcd_update = now + LCD_REFRESH_US;
        }
    }
}
