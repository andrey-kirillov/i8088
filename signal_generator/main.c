// i8088 slow-clock signal generator (RP2040 / Pico 1).
//
// Generates a 1-100 Hz clock for single-stepping an Intel 80C88 (CMOS) CPU.
// The CLK high pulse is a FIXED width; the potentiometer varies only the low time,
// i.e. the frequency. The 80C88 is a static part with no input duty-cycle rule and
// no maximum period -- it only needs each phase above its minimum (CLK high time
// TCHCL >= 69 ns, low time TCLCH >= 118 ns, Harris 80C88 datasheet), which a fixed
// pulse + long low easily meets. MODE toggles AUTO/MANUAL; in MANUAL each STEP press
// emits one fixed pulse. A 1602 LCD shows frequency, period and the fixed pulse.
//
// The 80C88 CLK input needs ~3.9-4.2 V logic-high, so GP15 drives the CPU through an
// inverting MOSFET level shifter (3.3 V -> 5 V). The original NMOS 8088 CANNOT run
// this slow (2 MHz floor) - use the static CMOS 80C88. See CLAUDE.md.
//
// Core 1 is a dedicated clock engine; its long low phase polls the shared timing in
// small chunks so pot/mode changes apply within a few ms. Core 0 runs the UI.
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/adc.h"
#include "hardware/i2c.h"
#include "lcd1602_i2c.h"

// ----- Pin assignments -------------------------------------------------------
#define CLK_GPIO        15      // clock output -> Q1 level shifter -> 80C88 CLK
// GP15 drives the 80C88 CLK through an INVERTING transistor stage (single
// N-MOSFET, 3.3V -> 5V). With CLK_INVERTED=1 the firmware pre-inverts the GPIO so
// the CPU's CLK is high during the pulse. Set to 0 for a non-inverting buffer
// (e.g. 74HCT125) wired straight to CLK.
#define CLK_INVERTED    1
#define MODE_BTN_GPIO   14      // toggles AUTO/MANUAL, active-low to GND
#define STEP_BTN_GPIO   13      // single-step in MANUAL, active-low to GND
#define POT_ADC_GPIO    26      // potentiometer wiper -> ADC0
#define POT_ADC_CHANNEL 0
#define STATUS_LED_GPIO PICO_DEFAULT_LED_PIN // built-in LED, solid on = running

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
#define US_PER_SECOND      1000000.0f

// Fixed CLK high-pulse width. The static CMOS 80C88 only needs each phase above its
// minimum (high TCHCL >= 69 ns, low TCLCH >= 118 ns) with no duty-cycle rule, so the
// high pulse is held CONSTANT and only the low time (frequency) varies. 100 us sits
// well above the 69 ns floor and the MOSFET's RC edge, yet stays a small slice of
// the period across 1-100 Hz. Keep it < 1000 us for the LCD's 3-digit field.
#define PULSE_WIDTH_US     100u
#define MIN_LOW_US         2u    // floor on the low phase if period ever nears pulse
#define LOW_POLL_US        5000u // re-check pot/mode at least this often during low

// ----- Timing / smoothing ----------------------------------------------------
#define ADC_SMOOTH_ALPHA   0.20f  // EMA weight for the new sample
#define ADC_OVERSAMPLE     16     // ADC reads averaged per sample (noise rejection)
#define ADC_END_DEADZONE   40.0f  // counts trimmed at each travel end (reach 1 & 100 Hz)
#define FREQ_HYSTERESIS_HZ 0.2f   // ignore smaller changes so the readout stays steady
#define DEBOUNCE_US        25000  // button debounce window
#define LCD_REFRESH_US     150000 // LCD redraw interval
#define MANUAL_IDLE_US     200    // core-1 idle poll while waiting for a step

// ----- Shared state (core 0 writes, core 1 reads) ---------------------------
static critical_section_t s_clk_lock;
static volatile uint32_t  s_high_us = PULSE_WIDTH_US; // fixed high-pulse width
static volatile uint32_t  s_low_us  = 9900;   // low phase width (varies with frequency)
static volatile bool      s_manual  = false;  // AUTO (false) / MANUAL (true)
static volatile bool      s_step_request = false;
static volatile uint32_t  s_step_count   = 0;

// Drive the GPIO so the 80C88 CLK pin reaches `cpu_high`, compensating for the
// inverting transistor stage between GP15 and the CPU.
static inline void clk_drive(bool cpu_high) {
    gpio_put(CLK_GPIO, CLK_INVERTED ? !cpu_high : cpu_high);
}

// Snapshot the shared timing/mode under the lock.
static void read_clock_state(uint32_t *high_us, uint32_t *low_us, bool *manual) {
    critical_section_enter_blocking(&s_clk_lock);
    *high_us = s_high_us;
    *low_us  = s_low_us;
    *manual  = s_manual;
    critical_section_exit(&s_clk_lock);
}

// Emit one fixed-width high pulse, then return CLK low.
static void emit_high_pulse(uint32_t high_us) {
    clk_drive(true);
    busy_wait_us(high_us);
    clk_drive(false);
}

// Core 1: clock engine. The high pulse is fixed; the long low phase polls the shared
// state in chunks so a pot turn or mode change is picked up within a few ms (instead
// of after a whole period, which at 1 Hz would be up to a second).
static void core1_clock_engine(void) {
    while (true) {
        uint32_t high_us;
        uint32_t low_us;
        bool manual;
        read_clock_state(&high_us, &low_us, &manual);

        if (manual) {
            if (s_step_request) {
                s_step_request = false;
                emit_high_pulse(high_us);
                s_step_count++;
            } else {
                clk_drive(false);
                busy_wait_us(MANUAL_IDLE_US);
            }
            continue;
        }

        // AUTO: fixed high pulse, then a responsive low phase.
        emit_high_pulse(high_us);
        uint64_t low_start = time_us_64();
        while (true) {
            read_clock_state(&high_us, &low_us, &manual);
            if (manual) {
                break; // switched to MANUAL — start the next iteration immediately
            }
            uint64_t elapsed = time_us_64() - low_start;
            if (elapsed >= low_us) {
                break; // low phase complete
            }
            uint32_t remaining = low_us - (uint32_t)elapsed;
            busy_wait_us(remaining < LOW_POLL_US ? remaining : LOW_POLL_US);
        }
    }
}

// Read the pot (oversampled + smoothed) and map to a frequency in Hz (1..100).
// A small dead-zone at each end of pot travel makes the extremes reach exactly
// 1 Hz and 100 Hz despite ADC offset (which otherwise floors the low end near 1.2 Hz).
static float read_frequency_hz(float *adc_ema) {
    uint32_t accumulator = 0;
    for (uint32_t sample = 0; sample < ADC_OVERSAMPLE; sample++) {
        accumulator += adc_read();
    }
    float raw = (float)accumulator / ADC_OVERSAMPLE;
    *adc_ema += (raw - *adc_ema) * ADC_SMOOTH_ALPHA;

    float span = ADC_FULL_SCALE - 2.0f * ADC_END_DEADZONE;
    float fraction = (*adc_ema - ADC_END_DEADZONE) / span;
    if (fraction < 0.0f) {
        fraction = 0.0f;
    } else if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    return FREQ_MIN_HZ + fraction * (FREQ_MAX_HZ - FREQ_MIN_HZ);
}

// Publish the shared high/low widths for a frequency: the high pulse is fixed, so
// only the low time changes (low = period - pulse).
static void publish_timing(float freq_hz) {
    float period_us = US_PER_SECOND / freq_hz;
    uint32_t low_us = MIN_LOW_US;
    if (period_us > (float)(PULSE_WIDTH_US + MIN_LOW_US)) {
        low_us = (uint32_t)period_us - PULSE_WIDTH_US;
    }
    critical_section_enter_blocking(&s_clk_lock);
    s_high_us = PULSE_WIDTH_US;
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
    float period_s = 1.0f / freq_hz;                 // 0.010 s (100 Hz) .. 1.000 s (1 Hz)
    char line[LCD_COLUMNS + 1];

    lcd_set_cursor(0, 0);
    if (s_manual) {
        snprintf(line, sizeof(line), "MAN  step%6lu", (unsigned long)s_step_count);
    } else {
        snprintf(line, sizeof(line), "AUTO f=%6.2fHz", (double)freq_hz);
    }
    lcd_print(line);

    // Period (T) in seconds varies; the high pulse (P) is fixed, shown in us.
    lcd_set_cursor(0, 1);
    snprintf(line, sizeof(line), "T=%6.3f P=%3luus",
             (double)period_s, (unsigned long)PULSE_WIDTH_US);
    lcd_print(line);
}

int main(void) {
    stdio_init_all();

    // Clock output (idle CPU CLK low through the inverting stage).
    gpio_init(CLK_GPIO);
    gpio_set_dir(CLK_GPIO, GPIO_OUT);
    clk_drive(false);

    // Built-in LED solid on to show the firmware is running.
    gpio_init(STATUS_LED_GPIO);
    gpio_set_dir(STATUS_LED_GPIO, GPIO_OUT);
    gpio_put(STATUS_LED_GPIO, 1);

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
    float stable_freq_hz = read_frequency_hz(&adc_ema);
    publish_timing(stable_freq_hz);
    critical_section_init(&s_clk_lock);
    multicore_launch_core1(core1_clock_engine);

    uint64_t next_lcd_update = 0;
    while (true) {
        if (button_pressed(&mode_button)) {
            s_manual = !s_manual;
        }
        if (button_pressed(&step_button) && s_manual) {
            s_step_request = true;
        }

        // Hysteresis keeps the clock and the T/H readout steady while the knob is
        // still; the rail check still lets the travel ends snap to exactly 1/100 Hz.
        float candidate_hz = read_frequency_hz(&adc_ema);
        bool at_rail = candidate_hz <= FREQ_MIN_HZ || candidate_hz >= FREQ_MAX_HZ;
        if (fabsf(candidate_hz - stable_freq_hz) >= FREQ_HYSTERESIS_HZ ||
            (at_rail && candidate_hz != stable_freq_hz)) {
            stable_freq_hz = candidate_hz;
            publish_timing(stable_freq_hz);
        }

        uint64_t now = time_us_64();
        if (now >= next_lcd_update) {
            update_display(stable_freq_hz);
            next_lcd_update = now + LCD_REFRESH_US;
        }
    }
}
