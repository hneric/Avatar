#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define OPUS_ENCODER_FRAME_MS 60
#define OPUS_ENCODER_SAMPLE_RATE 16000
#define OPUS_ENCODER_INPUT_SAMPLES (OPUS_ENCODER_SAMPLE_RATE * OPUS_ENCODER_FRAME_MS / 1000)
#define OPUS_ENCODER_FALLBACK_MAX_BYTES (OPUS_ENCODER_INPUT_SAMPLES * sizeof(int16_t))
#define OPUS_ENCODER_OUTPUT_MAX_BYTES 1500

esp_err_t audio_opus_encoder_init(void);
bool audio_opus_encoder_is_available(void);
size_t audio_opus_encoder_input_samples(void);
size_t audio_opus_encoder_output_max_bytes(void);
int audio_opus_encoder_encode(const int16_t *pcm, size_t samples, uint8_t *out, size_t out_size);
