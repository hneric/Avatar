#include "tts_player.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_codec_dev_types.h"
#include <math.h>
#include "opus.h"

static const char *TAG = "tts";

#define TTS_SAMPLE_RATE 16000
#define TTS_CHANNELS 1
#define TTS_MAX_FRAME_SAMPLES 960
#define TTS_MAX_PACKET_BYTES 512
#define TTS_QUEUE_DEPTH 24

typedef struct {
    size_t bytes;
    uint8_t data[TTS_MAX_PACKET_BYTES];
} tts_packet_t;

static QueueHandle_t s_queue = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static OpusDecoder *s_decoder = NULL;
static volatile bool s_reset_requested = false;
static volatile bool s_writing = false;
static volatile int s_pending_packets = 0;
static tts_player_level_cb_t s_level_cb = NULL;

static void tts_player_task(void *arg)
{
    int16_t *mono = heap_caps_malloc(TTS_MAX_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *stereo = heap_caps_malloc(TTS_MAX_FRAME_SAMPLES * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mono || !stereo) {
        ESP_LOGE(TAG, "pcm buffer alloc failed");
        free(mono);
        free(stereo);
        vTaskDelete(NULL);
    }

    tts_packet_t packet;
    int decoded_total = 0;
    while (1) {
        if (xQueueReceive(s_queue, &packet, portMAX_DELAY) != pdPASS) {
            continue;
        }

        if (s_reset_requested && s_decoder) {
            opus_decoder_ctl(s_decoder, OPUS_RESET_STATE);
            s_reset_requested = false;
            decoded_total = 0;
            ESP_LOGI(TAG, "decoder reset");
        }

        if (!s_decoder || !s_codec) {
            continue;
        }

        int samples = opus_decode(s_decoder, packet.data, (opus_int32)packet.bytes,
                                  mono, TTS_MAX_FRAME_SAMPLES, 0);
        if (samples <= 0) {
            ESP_LOGW(TAG, "decode failed: %d bytes=%u", samples, (unsigned)packet.bytes);
            if (s_pending_packets > 0) {
                s_pending_packets--;
            }
            continue;
        }

        int64_t sum_sq = 0;
        int peak = 0;
        for (int i = 0; i < samples; i++) {
            int s = mono[i];
            int a = abs(s);
            if (a > peak) {
                peak = a;
            }
            sum_sq += (int64_t)s * s;
            stereo[i * 2] = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        if (s_level_cb && samples > 0) {
            int rms = (int)sqrt((double)sum_sq / samples);
            s_level_cb(rms, peak);
        }

        s_writing = true;
        int ret = esp_codec_dev_write(s_codec, stereo, samples * 2 * sizeof(int16_t));
        s_writing = false;
        if (s_pending_packets > 0) {
            s_pending_packets--;
        }
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec write failed: %d", ret);
            continue;
        }

        decoded_total++;
        if (decoded_total % 50 == 0) {
            ESP_LOGI(TAG, "played opus packets=%d last_samples=%d", decoded_total, samples);
        }
    }
}

esp_err_t tts_player_init(esp_codec_dev_handle_t codec)
{
    if (!codec) {
        return ESP_ERR_INVALID_ARG;
    }
    s_codec = codec;

    if (!s_queue) {
        s_queue = xQueueCreate(TTS_QUEUE_DEPTH, sizeof(tts_packet_t));
        if (!s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_decoder) {
        int error = OPUS_OK;
        s_decoder = opus_decoder_create(TTS_SAMPLE_RATE, TTS_CHANNELS, &error);
        if (!s_decoder || error != OPUS_OK) {
            ESP_LOGE(TAG, "decoder create failed: %d", error);
            s_decoder = NULL;
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "decoder ready: %d Hz mono", TTS_SAMPLE_RATE);
    }

    static bool task_started = false;
    if (!task_started) {
        BaseType_t ok = xTaskCreate(tts_player_task, "tts_player", 24 * 1024, NULL, 3, NULL);
        if (ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
        task_started = true;
    }

    return ESP_OK;
}

void tts_player_set_level_callback(tts_player_level_cb_t callback)
{
    s_level_cb = callback;
}

esp_err_t tts_player_enqueue_opus(const uint8_t *data, size_t bytes)
{
    if (!s_queue || !data || bytes == 0 || bytes > TTS_MAX_PACKET_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    tts_packet_t packet = { 0 };
    packet.bytes = bytes;
    memcpy(packet.data, data, bytes);

    s_pending_packets++;
    if (xQueueSend(s_queue, &packet, 0) != pdPASS) {
        if (s_pending_packets > 0) {
            s_pending_packets--;
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool tts_player_is_busy(void)
{
    return s_writing || s_pending_packets > 0;
}

void tts_player_reset(void)
{
    if (s_queue) {
        xQueueReset(s_queue);
    }
    s_pending_packets = 0;
    s_reset_requested = true;
}
