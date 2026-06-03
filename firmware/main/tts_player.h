#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_codec_dev.h"

typedef void (*tts_player_level_cb_t)(int rms, int peak);

esp_err_t tts_player_init(esp_codec_dev_handle_t codec);
esp_err_t tts_player_enqueue_opus(const uint8_t *data, size_t bytes);
void tts_player_set_level_callback(tts_player_level_cb_t callback);
bool tts_player_is_busy(void);
void tts_player_reset(void);
