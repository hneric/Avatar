#pragma once

#include "esp_err.h"
#include <driver/i2c_master.h>

esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus);
esp_err_t audio_play_tone(int freq_hz, int duration_ms);
void audio_set_volume(int volume);
