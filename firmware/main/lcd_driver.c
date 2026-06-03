#include "lcd_driver.h"
#include "board.h"

#include <esp_log.h>
#include <esp_check.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include <esp_lvgl_port_disp.h>
#include <lvgl.h>

static const char *TAG = "lcd";
static lv_display_t *display_ = NULL;

/* --- Panel config (matches official example) --- */
#define LCD_RGB_BUFFER_NUMS             (2)
#define LCD_RGB_BOUNCE_BUFFER_HEIGHT    (10)

/* RGB timing from official example for 800x480 */
#define LCD_PANEL_RGB_TIMING()                              \
    {                                                       \
        .pclk_hz = 18 * 1000 * 1000,                        \
        .h_res = LCD_H_RES,                                 \
        .v_res = LCD_V_RES,                                 \
        .hsync_pulse_width = 40,                            \
        .hsync_back_porch = 40,                             \
        .hsync_front_porch = 48,                            \
        .vsync_pulse_width = 23,                            \
        .vsync_back_porch = 32,                             \
        .vsync_front_porch = 13,                            \
        .flags.pclk_active_neg = true,                      \
    }

esp_err_t lcd_init(void)
{
    ESP_LOGI(TAG, "Initialize RGB LCD 800x480");

    /* Step 1: RGB panel */
    esp_lcd_rgb_panel_config_t panel_conf = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .dma_burst_size = 64,
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .de_gpio_num = LCD_PIN_DE,
        .pclk_gpio_num = LCD_PIN_PCLK,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .disp_gpio_num = LCD_PIN_DISP,
        .data_gpio_nums = {
            LCD_PIN_DATA0,  LCD_PIN_DATA1,  LCD_PIN_DATA2,  LCD_PIN_DATA3,
            LCD_PIN_DATA4,  LCD_PIN_DATA5,  LCD_PIN_DATA6,  LCD_PIN_DATA7,
            LCD_PIN_DATA8,  LCD_PIN_DATA9,  LCD_PIN_DATA10, LCD_PIN_DATA11,
            LCD_PIN_DATA12, LCD_PIN_DATA13, LCD_PIN_DATA14, LCD_PIN_DATA15,
        },
        .timings = LCD_PANEL_RGB_TIMING(),
        .flags.fb_in_psram = 0,          /* No full frame buffers - use bounce buffer mode */
        .num_fbs = 0,                      /* 0 = pure bounce buffer mode */
        .bounce_buffer_size_px = LCD_H_RES * 20,  /* 16KB bounce buffer in internal SRAM */
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_conf, &panel_handle),
                        TAG, "RGB panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle),
                        TAG, "LCD init failed");
    ESP_LOGI(TAG, "RGB panel ready");

    /* Step 2: LVGL port */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    /* Step 3: Add RGB display with bounce buffer mode
     * NOTE: avoid_tearing needs target-specific code (currently only ESP32S3/P4).
     * On ESP32S31 we use bounce buffer mode with small LVGL draw buffers
     * instead of direct_mode (which would need 768KB allocation from internal RAM). */
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * 100,  /* 160KB - fits in internal RAM */
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,     /* LVGL draw buffer in PSRAM (16MB available) */
            .swap_bytes = false,
            .direct_mode = false,
            .full_refresh = false,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = false,
        },
    };
    display_ = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (display_ == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL RGB display");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL display ready");
    return ESP_OK;
}

lv_display_t* lcd_get_display(void)
{
    return display_;
}
