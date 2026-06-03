#include "wifi_manager.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

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

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
