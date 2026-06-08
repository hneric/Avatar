#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocomm_ble.h"
#include "protocomm_security.h"
#include "sdkconfig.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;
static bool s_connected;
static bool s_connecting;
static bool s_initialized;
static bool s_started;
static bool s_prov_initialized;
static wifi_manager_prov_status_cb_t s_prov_status_cb;

static void wifi_manager_notify_prov(wifi_manager_prov_state_t state, const char *detail)
{
    if (s_prov_status_cb) {
        s_prov_status_cb(state, detail);
    }
}

static void wifi_manager_get_service_name(char *service_name, size_t service_name_size)
{
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(service_name, service_name_size, "S31_%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        ESP_LOGW(TAG, "disconnected, reason=%d", event ? event->reason : -1);
        if (!s_connecting) {
            return;
        }
        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "retry WiFi connection (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            s_connecting = false;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        s_connecting = false;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_manager_notify_prov(WIFI_MANAGER_PROV_CONNECTED, NULL);
    } else if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "BLE provisioning started");
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_ADVERTISING, NULL);
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            char detail[48];
            snprintf(detail, sizeof(detail), "SSID: %s", (const char *)wifi_sta_cfg->ssid);
            ESP_LOGI(TAG, "received WiFi credentials for %s", (const char *)wifi_sta_cfg->ssid);
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_CRED_RECV, detail);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            network_prov_wifi_sta_fail_reason_t *reason = (network_prov_wifi_sta_fail_reason_t *)event_data;
            const char *detail = "WiFi connect failed";
            if (reason && *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR) {
                detail = "WiFi auth failed";
            } else if (reason && *reason == NETWORK_PROV_WIFI_STA_AP_NOT_FOUND) {
                detail = "WiFi AP not found";
            }
            ESP_LOGE(TAG, "provisioning failed: %s", detail);
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_CRED_FAIL, detail);
            network_prov_mgr_reset_wifi_sm_state_on_failure();
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "provisioning credentials accepted");
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_CRED_SUCCESS, NULL);
            break;
        case NETWORK_PROV_END:
            ESP_LOGI(TAG, "BLE provisioning ended");
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_CLOSED, NULL);
            if (s_prov_initialized) {
                network_prov_mgr_deinit();
                s_prov_initialized = false;
            }
            break;
        default:
            break;
        }
    } else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            ESP_LOGI(TAG, "BLE client connected");
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_BLE_CONNECTED, NULL);
        } else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            ESP_LOGI(TAG, "BLE client disconnected");
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_ADVERTISING, NULL);
        }
    } else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (event_id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) {
            ESP_LOGI(TAG, "BLE provisioning secure session established");
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_SECURITY_OK, NULL);
        } else if (event_id == PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS ||
                   event_id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH) {
            ESP_LOGE(TAG, "BLE provisioning security handshake failed");
            wifi_manager_notify_prov(WIFI_MANAGER_PROV_ERROR, "Security handshake failed");
        }
    }
}

static esp_err_t wifi_manager_init_once(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "init nvs");

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group, ESP_ERR_NO_MEM, TAG, "create event group");

    ESP_ERROR_CHECK(esp_netif_init());
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &instance_any_id), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &instance_got_ip), TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL), TAG, "prov handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL), TAG, "ble handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL), TAG, "security handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
    s_initialized = true;
    return ESP_OK;
}

static esp_err_t wifi_manager_start_sta(void)
{
    ESP_RETURN_ON_ERROR(wifi_manager_init_once(), TAG, "init wifi");
    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_started = true;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(ssid && ssid[0], ESP_ERR_INVALID_ARG, TAG, "empty ssid");
    password = password ? password : "";

    ESP_RETURN_ON_ERROR(wifi_manager_start_sta(), TAG, "start sta");

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_LOGI(TAG, "connecting to SSID: %s", ssid);

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_connected = false;
    s_connecting = true;

    esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to %s", ssid);
        return ESP_OK;
    }
    s_connecting = false;
    ESP_LOGE(TAG, "failed to connect to %s", ssid);
    return ESP_FAIL;
}

esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t max_records, uint16_t *record_count)
{
    ESP_RETURN_ON_FALSE(records && record_count && max_records > 0, ESP_ERR_INVALID_ARG, TAG, "bad scan args");
    ESP_RETURN_ON_ERROR(wifi_manager_start_sta(), TAG, "start sta");

    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(&scan_config, true), TAG, "scan start");

    uint16_t count = max_records;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records), TAG, "scan records");
    *record_count = count;
    ESP_LOGI(TAG, "scan found %u APs", count);
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (strlen(CONFIG_KORVO_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "WiFi SSID is empty; skip network startup");
        return ESP_ERR_INVALID_STATE;
    }
    return wifi_manager_connect(CONFIG_KORVO_WIFI_SSID, CONFIG_KORVO_WIFI_PASSWORD);
}

esp_err_t wifi_manager_start_ble_provisioning(wifi_manager_prov_status_cb_t cb)
{
    s_prov_status_cb = cb;
    ESP_RETURN_ON_ERROR(wifi_manager_init_once(), TAG, "init wifi");
    if (s_prov_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    network_prov_mgr_config_t config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT,
        .app_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = WIFI_MAX_RETRY,
        },
    };

    ESP_RETURN_ON_ERROR(network_prov_mgr_init(config), TAG, "prov init");
    s_prov_initialized = true;

    bool provisioned = false;
    esp_err_t ret = network_prov_mgr_is_wifi_provisioned(&provisioned);
    if (ret != ESP_OK) {
        network_prov_mgr_deinit();
        s_prov_initialized = false;
        return ret;
    }

    if (provisioned) {
        ESP_LOGI(TAG, "already provisioned; starting WiFi STA");
        wifi_manager_notify_prov(WIFI_MANAGER_PROV_ALREADY_PROVISIONED, NULL);
        network_prov_mgr_deinit();
        s_prov_initialized = false;
        ESP_RETURN_ON_ERROR(wifi_manager_start_sta(), TAG, "start sta");
        s_connecting = true;
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect stored wifi");
        return ESP_OK;
    }

    char service_name[16] = { 0 };
    wifi_manager_get_service_name(service_name, sizeof(service_name));

    uint8_t service_uuid[16] = {
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x31, 0x90, 0x1a, 0x02,
    };
    ESP_RETURN_ON_ERROR(network_prov_scheme_ble_set_service_uuid(service_uuid), TAG, "set ble uuid");

    const char *pop = CONFIG_KORVO_BLE_PROV_POP;
    network_prov_security_t security = NETWORK_PROV_SECURITY_1;
    network_prov_security1_params_t *sec_params = (network_prov_security1_params_t *)pop;

    ESP_LOGI(TAG, "starting BLE provisioning service=%s pop=%s", service_name, pop);
    ESP_LOGI(TAG, "provisioning QR payload: {\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"ble\"}",
             service_name, pop);
    ret = network_prov_mgr_start_provisioning(security, (const void *)sec_params, service_name, NULL);
    if (ret != ESP_OK) {
        network_prov_mgr_deinit();
        s_prov_initialized = false;
        wifi_manager_notify_prov(WIFI_MANAGER_PROV_ERROR, esp_err_to_name(ret));
        return ret;
    }

    wifi_manager_notify_prov(WIFI_MANAGER_PROV_ADVERTISING, service_name);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_ble_provisioning(void)
{
    if (!s_prov_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "stopping BLE provisioning");
    s_prov_initialized = false;
    esp_err_t ret = network_prov_mgr_deinit();
    wifi_manager_notify_prov(WIFI_MANAGER_PROV_CLOSED, NULL);
    return ret;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
