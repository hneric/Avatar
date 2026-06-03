#pragma once

#include "esp_err.h"
#include <driver/i2c_master.h>
#include <lvgl.h>

/**
 * Initialize I2C master bus (shared by touch, audio, camera)
 */
esp_err_t i2c_init(void);

/**
 * Get I2C master bus handle for peripheral initialization
 */
i2c_master_bus_handle_t i2c_get_bus(void);

/**
 * Initialize GT1151 touch panel on LVGL display
 */
esp_err_t touch_init(lv_display_t *display);
