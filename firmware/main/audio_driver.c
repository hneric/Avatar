#include "audio_driver.h"
#include "board.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"

static const char *TAG = "audio";
static esp_codec_dev_handle_t codec_handle = NULL;
static i2s_chan_handle_t g_tx_handle = NULL;

esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus)
{
    /* I2S channels */
    i2s_chan_handle_t tx_h = NULL, rx_h = NULL;
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .intr_priority = 0,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_h, &rx_h), TAG, "I2S new channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_384,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_DIN_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_h, &std_cfg), TAG, "I2S TX init");
    g_tx_handle = tx_h;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_h, &std_cfg), TAG, "I2S RX init");

    /* Codec control interface (I2C) */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_PORT,
        .addr = ES8389_I2C_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if);

    /* Codec data interface (I2S) */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_h,
        .tx_handle = tx_h,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if);

    /* GPIO interface (PA control managed by codec) */
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    /* ES8389 config - pa_pin tells codec to manage PA automatically */
    es8389_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = ES8389_PA_PIN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&es_cfg);
    assert(codec_if);

    /* Top codec device handle */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    codec_handle = esp_codec_dev_new(&dev_cfg);
    assert(codec_handle);
    esp_codec_set_disable_when_closed(codec_handle, false);

    /* Open codec once (matching official example pattern) */
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = 16000,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(codec_handle, &sample_cfg), TAG, "Codec open failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(codec_handle, 80), TAG, "Volume set failed");

    /* Force PA on for debug */
    gpio_reset_pin(ES8389_PA_PIN);
    gpio_set_direction(ES8389_PA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ES8389_PA_PIN, 1);



    /* Test: write tone directly to I2S right after open */
    vTaskDelay(pdMS_TO_TICKS(100)); /* Let codec settle */
    int16_t test[1600*2]; /* 100ms @ 16kHz */
    for (int i = 0; i < 1600; i++) {
        int16_t v = (int16_t)(sinf(2 * M_PI * 1000 * i / 16000.0f) * 20000);
        test[i*2] = v; test[i*2+1] = v;
    }
    gpio_set_level(ES8389_PA_PIN, 1);
    size_t wr = 0;
    i2s_channel_write(g_tx_handle, test, sizeof(test), &wr, portMAX_DELAY);
    ESP_LOGI(TAG, "Init test tone: %d bytes written", wr);

    ESP_LOGI(TAG, "ES8389 ready");
    return ESP_OK;
}

esp_err_t audio_play_tone(int freq_hz, int duration_ms)
{
    ESP_LOGI(TAG, "play_tone: freq=%d dur=%dms", freq_hz, duration_ms);
    if (!g_tx_handle) return ESP_ERR_INVALID_STATE;

    int samples = 16000 * duration_ms / 1000;
    int total = samples * 2;
    int16_t *buf = heap_caps_malloc(total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return ESP_ERR_NO_MEM;

    for (int i = 0; i < samples; i++) {
        int16_t v = (int16_t)(sinf(2 * M_PI * freq_hz * i / 16000.0f) * 16000);
        buf[i*2] = v; buf[i*2+1] = v;
    }

    /* Like ES8311 example: write directly to I2S */
    gpio_set_level(ES8389_PA_PIN, 1);
    size_t written = 0;
    esp_err_t ret = i2s_channel_write(g_tx_handle, buf, total * sizeof(int16_t), &written, portMAX_DELAY);
    ESP_LOGI(TAG, "i2s write: %d bytes, ret=%d", written, ret);
    free(buf);
    return ESP_OK;
}

void audio_set_volume(int vol)
{
    if (codec_handle) esp_codec_dev_set_out_vol(codec_handle, vol);
}
