#ifndef LCD_I2C_H
#define LCD_I2C_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_I2C_DEFAULT_ADDR        0x27
#define LCD_I2C_DEFAULT_COLS        16
#define LCD_I2C_DEFAULT_ROWS        2
#define LCD_I2C_DEFAULT_TIMEOUT_MS  100

typedef struct {
    i2c_port_t i2c_port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint8_t address;
    uint8_t cols;
    uint8_t rows;
    uint32_t clk_speed_hz;
    bool backlight;
} lcd_i2c_config_t;

typedef struct {
    lcd_i2c_config_t cfg;
} lcd_i2c_t;

esp_err_t lcd_i2c_init(lcd_i2c_t *lcd, const lcd_i2c_config_t *cfg);
esp_err_t lcd_i2c_clear(lcd_i2c_t *lcd);
esp_err_t lcd_i2c_home(lcd_i2c_t *lcd);
esp_err_t lcd_i2c_set_cursor(lcd_i2c_t *lcd, uint8_t col, uint8_t row);
esp_err_t lcd_i2c_write_char(lcd_i2c_t *lcd, char c);
esp_err_t lcd_i2c_write_str(lcd_i2c_t *lcd, const char *str);
esp_err_t lcd_i2c_backlight(lcd_i2c_t *lcd, bool on);

#ifdef __cplusplus
}
#endif

#endif