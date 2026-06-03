#include "touch_driver.h"
#include "board.h"

#include <esp_log.h>
#include <esp_check.h>
#include <driver/i2c_master.h>
#include <esp_lcd_touch_gt1151.h>
#include <esp_lvgl_port_touch.h>

static const char *TAG = "touch";

static i2c_master_bus_handle_t i2c_bus_ = NULL;

esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &i2c_bus_),
                        TAG, "I2C bus init fail");
    ESP_LOGI(TAG, "I2C bus initialized (SDA=GPIO0, SCL=GPIO1)");
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_get_bus(void)
{
    return i2c_bus_;
}

esp_err_t touch_init(lv_display_t *display)
{
    if (i2c_bus_ == NULL) {
        i2c_init();
    }

    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
    };

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT1151_ADDRESS,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .flags = { .disable_control_phase = 1 },
        .scl_speed_hz = 400 * 1000,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_cfg, &tp_io),
                        TAG, "Touch IO init fail");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt1151(tp_io, &tp_cfg, &tp),
                        TAG, "GT1151 init fail");

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = display,
        .handle = tp,
    };
    if (lvgl_port_add_touch(&touch_cfg) == NULL) {
        ESP_LOGE(TAG, "LVGL touch add fail");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GT1151 touch initialized");
    return ESP_OK;
}
