#pragma once

#include <stdint.h>
#include "esp_err.h"
#include <lvgl.h>

/**
 * Initialize RGB LCD panel (800×480) and LVGL
 */
esp_err_t lcd_init(void);

/**
 * Get the LVGL display object
 */
lv_display_t* lcd_get_display(void);
