// HD44780 1602 LCD driver over a PCF8574 I2C backpack (4-bit mode).
//
// Common backpack bit mapping (PCF8574 P0..P7):
//   P0 = RS, P1 = RW, P2 = EN, P3 = backlight, P4..P7 = D4..D7
#include "lcd1602_i2c.h"
#include "pico/stdlib.h"

#define LCD_RS_BIT        0x01
#define LCD_EN_BIT        0x04
#define LCD_BACKLIGHT_BIT 0x08

// HD44780 commands
#define LCD_CMD_CLEAR        0x01
#define LCD_CMD_ENTRY_MODE   0x06 // increment cursor, no shift
#define LCD_CMD_DISPLAY_ON   0x0C // display on, cursor off, blink off
#define LCD_CMD_DISPLAY_OFF  0x08
#define LCD_CMD_FUNCTION_SET 0x28 // 4-bit, 2 lines, 5x8 font
#define LCD_CMD_SET_DDRAM    0x80 // OR with address

#define LCD_ROW0_BASE 0x00
#define LCD_ROW1_BASE 0x40

#define LCD_ENABLE_PULSE_US 1
#define LCD_SETTLE_US       50
#define LCD_CLEAR_US        2000

static i2c_inst_t *s_i2c;
static uint8_t      s_addr;
static uint8_t      s_backlight = LCD_BACKLIGHT_BIT;

// Push one byte to the PCF8574 (the raw port state).
static void pcf8574_write(uint8_t value) {
    i2c_write_blocking(s_i2c, s_addr, &value, 1, false);
}

// Latch one nibble (already positioned in the high 4 bits) plus control bits.
static void lcd_pulse_enable(uint8_t nibble_and_ctrl) {
    pcf8574_write(nibble_and_ctrl | LCD_EN_BIT);
    busy_wait_us(LCD_ENABLE_PULSE_US);
    pcf8574_write(nibble_and_ctrl & ~LCD_EN_BIT);
    busy_wait_us(LCD_SETTLE_US);
}

// Send one nibble (value in high 4 bits) with the given control bits (RS).
static void lcd_write_nibble(uint8_t high_nibble, uint8_t control) {
    uint8_t port = (high_nibble & 0xF0) | control | s_backlight;
    lcd_pulse_enable(port);
}

// Send a full byte as two nibbles. `is_data` selects the RS line.
static void lcd_send(uint8_t value, bool is_data) {
    uint8_t control = is_data ? LCD_RS_BIT : 0x00;
    lcd_write_nibble(value & 0xF0, control);
    lcd_write_nibble((value << 4) & 0xF0, control);
}

void lcd_backlight(bool on) {
    s_backlight = on ? LCD_BACKLIGHT_BIT : 0x00;
    pcf8574_write(s_backlight);
}

void lcd_init(i2c_inst_t *i2c, uint8_t addr) {
    s_i2c = i2c;
    s_addr = addr;
    s_backlight = LCD_BACKLIGHT_BIT;

    // Power-on settle.
    busy_wait_us(50000);

    // Wake-up sequence: three 0x30 nibble writes, then switch to 4-bit (0x20).
    lcd_write_nibble(0x30, 0x00);
    busy_wait_us(4500);
    lcd_write_nibble(0x30, 0x00);
    busy_wait_us(4500);
    lcd_write_nibble(0x30, 0x00);
    busy_wait_us(150);
    lcd_write_nibble(0x20, 0x00); // 4-bit mode

    lcd_send(LCD_CMD_FUNCTION_SET, false);
    lcd_send(LCD_CMD_DISPLAY_OFF, false);
    lcd_send(LCD_CMD_CLEAR, false);
    busy_wait_us(LCD_CLEAR_US);
    lcd_send(LCD_CMD_ENTRY_MODE, false);
    lcd_send(LCD_CMD_DISPLAY_ON, false);
}

void lcd_clear(void) {
    lcd_send(LCD_CMD_CLEAR, false);
    busy_wait_us(LCD_CLEAR_US);
}

void lcd_set_cursor(uint8_t column, uint8_t row) {
    uint8_t base = row == 0 ? LCD_ROW0_BASE : LCD_ROW1_BASE;
    lcd_send(LCD_CMD_SET_DDRAM | (base + column), false);
}

void lcd_print(const char *text) {
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        lcd_send((uint8_t)*cursor, true);
    }
}
