#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max_records, uint16_t *record_count);
bool wifi_manager_is_connected(void);
