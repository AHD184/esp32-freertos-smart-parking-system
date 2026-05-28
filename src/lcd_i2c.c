#include "lcd_i2c.h"
#include "esp_rom_sys.h"
#include <string.h>

#define LCD_REG_SELECT   0x01  // P0 -> RS
#define LCD_READ_WRITE   0x02  // P1 -> RW
#define LCD_ENABLE       0x04  // P2 -> E
#define LCD_BACKLIGHT    0x08  // P3 -> Backlight
#define LCD_D4_SHIFT     4     // P4..P7 -> D4..D7

#define LCD_CMD_CLEAR            0x01
#define LCD_CMD_HOME             0x02
#define LCD_CMD_ENTRY_MODE       0x06
#define LCD_CMD_DISPLAY_ON       0x0C
#define LCD_CMD_FUNCTION_SET     0x28  // 4-bit, 2-line, 5x8 font
#define LCD_CMD_SET_DDRAM_ADDR   0x80

static esp_err_t lcd_i2c_write_byte(const lcd_i2c_t *lcd, uint8_t data) {
    return i2c_master_write_to_device(
        lcd->cfg.i2c_port,
        lcd->cfg.address,
        &data,
        1,
        pdMS_TO_TICKS(LCD_I2C_DEFAULT_TIMEOUT_MS)
    );
}

static esp_err_t lcd_i2c_write4(const lcd_i2c_t *lcd, uint8_t nibble, uint8_t rs) {
    uint8_t out = ((nibble & 0x0F) << LCD_D4_SHIFT);

    if (rs) {
        out |= LCD_REG_SELECT;
    }
    if (lcd->cfg.backlight) {
        out |= LCD_BACKLIGHT;
    }

    esp_err_t err;
    err = lcd_i2c_write_byte(lcd, out);
    if (err != ESP_OK) return err;

    err = lcd_i2c_write_byte(lcd, out | LCD_ENABLE);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(1);

    err = lcd_i2c_write_byte(lcd, out);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(50);

    return ESP_OK;
}

static esp_err_t lcd_i2c_send(const lcd_i2c_t *lcd, uint8_t value, uint8_t rs) {
    esp_err_t err;
    err = lcd_i2c_write4(lcd, value >> 4, rs);
    if (err != ESP_OK) return err;

    err = lcd_i2c_write4(lcd, value & 0x0F, rs);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

static esp_err_t lcd_i2c_command(const lcd_i2c_t *lcd, uint8_t cmd) {
    esp_err_t err = lcd_i2c_send(lcd, cmd, 0);
    if (err != ESP_OK) return err;

    if (cmd == LCD_CMD_CLEAR || cmd == LCD_CMD_HOME) {
        esp_rom_delay_us(2000);
    }
    return ESP_OK;
}

esp_err_t lcd_i2c_backlight(lcd_i2c_t *lcd, bool on) {
    if (lcd == NULL) return ESP_ERR_INVALID_ARG;
    lcd->cfg.backlight = on;
    return lcd_i2c_write_byte(lcd, on ? LCD_BACKLIGHT : 0x00);
}

esp_err_t lcd_i2c_init(lcd_i2c_t *lcd, const lcd_i2c_config_t *cfg) {
    if (lcd == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(lcd, 0, sizeof(*lcd));
    lcd->cfg = *cfg;

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = cfg->clk_speed_hz,
    };

    esp_err_t err = i2c_param_config(cfg->i2c_port, &i2c_cfg);
    if (err != ESP_OK) return err;

    err = i2c_driver_install(cfg->i2c_port, i2c_cfg.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_rom_delay_us(50000);

    // HD44780 init sequence for 4-bit mode via PCF8574 backpack
    err = lcd_i2c_write4(lcd, 0x03, 0);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(4500);

    err = lcd_i2c_write4(lcd, 0x03, 0);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(4500);

    err = lcd_i2c_write4(lcd, 0x03, 0);
    if (err != ESP_OK) return err;
    esp_rom_delay_us(150);

    err = lcd_i2c_write4(lcd, 0x02, 0); // 4-bit mode
    if (err != ESP_OK) return err;

    err = lcd_i2c_command(lcd, LCD_CMD_FUNCTION_SET);
    if (err != ESP_OK) return err;

    err = lcd_i2c_command(lcd, LCD_CMD_DISPLAY_ON);
    if (err != ESP_OK) return err;

    err = lcd_i2c_command(lcd, LCD_CMD_CLEAR);
    if (err != ESP_OK) return err;

    err = lcd_i2c_command(lcd, LCD_CMD_ENTRY_MODE);
    if (err != ESP_OK) return err;

    return ESP_OK;
}

esp_err_t lcd_i2c_clear(lcd_i2c_t *lcd) {
    if (lcd == NULL) return ESP_ERR_INVALID_ARG;
    return lcd_i2c_command(lcd, LCD_CMD_CLEAR);
}

esp_err_t lcd_i2c_home(lcd_i2c_t *lcd) {
    if (lcd == NULL) return ESP_ERR_INVALID_ARG;
    return lcd_i2c_command(lcd, LCD_CMD_HOME);
}

esp_err_t lcd_i2c_set_cursor(lcd_i2c_t *lcd, uint8_t col, uint8_t row) {
    if (lcd == NULL) return ESP_ERR_INVALID_ARG;

    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};

    if (row >= lcd->cfg.rows) row = lcd->cfg.rows - 1;
    if (col >= lcd->cfg.cols) col = lcd->cfg.cols - 1;

    return lcd_i2c_command(lcd, LCD_CMD_SET_DDRAM_ADDR | (col + row_offsets[row]));
}

esp_err_t lcd_i2c_write_char(lcd_i2c_t *lcd, char c) {
    if (lcd == NULL) return ESP_ERR_INVALID_ARG;
    return lcd_i2c_send(lcd, (uint8_t)c, 1);
}

esp_err_t lcd_i2c_write_str(lcd_i2c_t *lcd, const char *str) {
    if (lcd == NULL || str == NULL) return ESP_ERR_INVALID_ARG;

    while (*str) {
        esp_err_t err = lcd_i2c_write_char(lcd, *str++);
        if (err != ESP_OK) return err;
    }

    return ESP_OK;
}