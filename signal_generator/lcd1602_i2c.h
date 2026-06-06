// HD44780 1602 LCD driver over a PCF8574 I2C backpack (4-bit mode).
#ifndef LCD1602_I2C_H
#define LCD1602_I2C_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/i2c.h"

// Initialise the LCD. `i2c` must already be configured with its SDA/SCL pins.
// `addr` is the PCF8574 7-bit address (0x27 typical, 0x3F for PCF8574A).
void lcd_init(i2c_inst_t *i2c, uint8_t addr);

// Clear the display and home the cursor.
void lcd_clear(void);

// Move the cursor. `column` 0-15, `row` 0-1.
void lcd_set_cursor(uint8_t column, uint8_t row);

// Print a null-terminated string at the current cursor position.
void lcd_print(const char *text);

// Turn the backlight on/off (applied on the next byte written).
void lcd_backlight(bool on);

#endif // LCD1602_I2C_H
