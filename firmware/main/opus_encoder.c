#include "opus_encoder.h"

#include <limits.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "opus";

#if defined(__has_include)
#if __has_include("esp_opus_enc.h") && __has_include("esp_audio_enc.h")
#define HAVE_ESP_OPUS_ENCODER 1
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#elif __has_include("opus.h")
#define HAVE_LIBOPUS_ENCODER 1
#include "opus.h"
#endif
#endif

#ifndef HAVE_ESP_OPUS_ENCODER
#define HAVE_ESP_OPUS_ENCODER 0
#endif
#ifndef HAVE_LIBOPUS_ENCODER
#define HAVE_LIBOPUS_ENCODER 0
#endif

#if HAVE_ESP_OPUS_ENCODER
static void *s_encoder = NULL;
static int s_in_bytes = 0;
static int s_out_bytes = 0;
#elif HAVE_LIBOPUS_ENCODER
static OpusEncoder *s_encoder = NULL;
#endif

esp_err_t audio_opus_encoder_init(void)
{
#if HAVE_ESP_OPUS_ENCODER
    if (s_encoder) {
        return ESP_OK;
    }

    esp_opus_enc_config_t cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = 0,
        .enable_fec = false,
        .enable_dtx = true,
        .enable_vbr = true,
    };

    int ret = esp_opus_enc_open(&cfg, sizeof(cfg), &s_encoder);
    if (ret != ESP_AUDIO_ERR_OK || !s_encoder) {
        ESP_LOGE(TAG, "open failed: %d", ret);
        s_encoder = NULL;
        return ESP_FAIL;
    }

    ret = esp_opus_enc_get_frame_size(s_encoder, &s_in_bytes, &s_out_bytes);
    if (ret != ESP_AUDIO_ERR_OK || s_in_bytes <= 0 || s_out_bytes <= 0) {
        ESP_LOGE(TAG, "get frame size failed: %d in=%d out=%d", ret, s_in_bytes, s_out_bytes);
        esp_opus_enc_close(s_encoder);
        s_encoder = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ready: in=%d bytes out_max=%d bytes", s_in_bytes, s_out_bytes);
    return ESP_OK;
#elif HAVE_LIBOPUS_ENCODER
    if (s_encoder) {
        return ESP_OK;
    }

    int error = OPUS_OK;
    s_encoder = opus_encoder_create(OPUS_ENCODER_SAMPLE_RATE, 1, OPUS_APPLICATION_AUDIO, &error);
    if (!s_encoder || error != OPUS_OK) {
        ESP_LOGE(TAG, "libopus create failed: %d", error);
        s_encoder = NULL;
        return ESP_FAIL;
    }

    opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(OPUS_AUTO));
    opus_encoder_ctl(s_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(s_encoder, OPUS_SET_DTX(1));

    ESP_LOGI(TAG, "libopus ready: %d Hz mono %d ms", OPUS_ENCODER_SAMPLE_RATE, OPUS_ENCODER_FRAME_MS);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "real Opus encoder component is not available; using PCM debug frames");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool audio_opus_encoder_is_available(void)
{
#if HAVE_ESP_OPUS_ENCODER || HAVE_LIBOPUS_ENCODER
    return s_encoder != NULL;
#else
    return false;
#endif
}

size_t audio_opus_encoder_input_samples(void)
{
    return OPUS_ENCODER_INPUT_SAMPLES;
}

size_t audio_opus_encoder_output_max_bytes(void)
{
#if HAVE_ESP_OPUS_ENCODER
    return s_out_bytes > 0 ? (size_t)s_out_bytes : OPUS_ENCODER_FALLBACK_MAX_BYTES;
#elif HAVE_LIBOPUS_ENCODER
    return OPUS_ENCODER_OUTPUT_MAX_BYTES;
#else
    return OPUS_ENCODER_FALLBACK_MAX_BYTES;
#endif
}

int audio_opus_encoder_encode(const int16_t *pcm, size_t samples, uint8_t *out, size_t out_size)
{
    if (!pcm || !out || samples != OPUS_ENCODER_INPUT_SAMPLES) {
        return -1;
    }

#if HAVE_ESP_OPUS_ENCODER
    if (!s_encoder || out_size < (size_t)s_out_bytes) {
        return -1;
    }

    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)pcm,
        .len = (uint32_t)(samples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t encoded = {
        .buffer = out,
        .len = (uint32_t)out_size,
        .encoded_bytes = 0,
    };

    int ret = esp_opus_enc_process(s_encoder, &in, &encoded);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "encode failed: %d", ret);
        return -1;
    }

    return (int)encoded.encoded_bytes;
#elif HAVE_LIBOPUS_ENCODER
    if (!s_encoder || out_size > INT_MAX) {
        return -1;
    }

    int encoded_bytes = opus_encode(s_encoder, pcm, (int)samples, out, (opus_int32)out_size);
    if (encoded_bytes <= 0) {
        ESP_LOGW(TAG, "libopus encode failed: %d", encoded_bytes);
        return -1;
    }

    return encoded_bytes;
#else
    size_t bytes = samples * sizeof(int16_t);
    if (out_size < bytes) {
        return -1;
    }
    memcpy(out, pcm, bytes);
    return (int)bytes;
#endif
}
