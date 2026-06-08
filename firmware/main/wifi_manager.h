#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    WIFI_MANAGER_PROV_IDLE = 0,
    WIFI_MANAGER_PROV_ADVERTISING,
    WIFI_MANAGER_PROV_BLE_CONNECTED,
    WIFI_MANAGER_PROV_SECURITY_OK,
    WIFI_MANAGER_PROV_CRED_RECV,
    WIFI_MANAGER_PROV_CRED_FAIL,
    WIFI_MANAGER_PROV_CRED_SUCCESS,
    WIFI_MANAGER_PROV_CONNECTED,
    WIFI_MANAGER_PROV_CLOSED,
    WIFI_MANAGER_PROV_ALREADY_PROVISIONED,
    WIFI_MANAGER_PROV_ERROR,
} wifi_manager_prov_state_t;

typedef void (*wifi_manager_prov_status_cb_t)(wifi_manager_prov_state_t state, const char *detail);

esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_start_ble_provisioning(wifi_manager_prov_status_cb_t cb);
esp_err_t wifi_manager_stop_ble_provisioning(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max_records, uint16_t *record_count);
bool wifi_manager_is_connected(void);
