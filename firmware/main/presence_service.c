#include "presence_service.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "presence";

static uint8_t s_own_addr_type;
static uint16_t s_presence_chr_handle;
static bool s_started;
static bool s_host_running;
static presence_service_nearby_cb_t s_nearby_cb;
static char s_presence_token[65] = { 0 };
static char s_last_payload[128] = "{\"state\":\"unbound\"}";

#define PRESENCE_NVS_NAMESPACE "presence"
#define PRESENCE_NVS_TOKEN_KEY "token"

static const ble_uuid128_t s_presence_svc_uuid =
    BLE_UUID128_INIT(0x31, 0x53, 0x00, 0x00, 0x7d, 0xb2, 0x44, 0x6a,
                     0x91, 0x99, 0x53, 0x31, 0x50, 0x52, 0x45, 0x53);

static const ble_uuid128_t s_presence_chr_uuid =
    BLE_UUID128_INIT(0x31, 0x53, 0x00, 0x01, 0x7d, 0xb2, 0x44, 0x6a,
                     0x91, 0x99, 0x53, 0x31, 0x50, 0x52, 0x45, 0x53);

static int presence_gap_event(struct ble_gap_event *event, void *arg);

static bool presence_extract_json_string(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return false;
    }
    out[0] = 0;

    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = 0;
    return i > 0;
}

static esp_err_t presence_load_token(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PRESENCE_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_presence_token[0] = 0;
        strlcpy(s_last_payload, "{\"state\":\"unbound\"}", sizeof(s_last_payload));
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open token nvs failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t len = sizeof(s_presence_token);
    ret = nvs_get_str(nvs, PRESENCE_NVS_TOKEN_KEY, s_presence_token, &len);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_presence_token[0] = 0;
        strlcpy(s_last_payload, "{\"state\":\"unbound\"}", sizeof(s_last_payload));
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read token failed: %s", esp_err_to_name(ret));
        return ret;
    }

    strlcpy(s_last_payload, "{\"state\":\"bound\"}", sizeof(s_last_payload));
    ESP_LOGI(TAG, "presence token loaded");
    return ESP_OK;
}

static esp_err_t presence_save_token(const char *token)
{
    if (!token || strlen(token) < 16 || strlen(token) >= sizeof(s_presence_token)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PRESENCE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open token nvs failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs, PRESENCE_NVS_TOKEN_KEY, token);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save token failed: %s", esp_err_to_name(ret));
        return ret;
    }

    strlcpy(s_presence_token, token, sizeof(s_presence_token));
    strlcpy(s_last_payload, "{\"state\":\"bound\"}", sizeof(s_last_payload));
    ESP_LOGI(TAG, "presence token bound");
    return ESP_OK;
}

static esp_err_t presence_clear_token(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PRESENCE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        ret = nvs_erase_key(nvs, PRESENCE_NVS_TOKEN_KEY);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    if (ret == ESP_OK) {
        s_presence_token[0] = 0;
        strlcpy(s_last_payload, "{\"state\":\"unbound\",\"result\":\"reset_ok\"}", sizeof(s_last_payload));
        ESP_LOGI(TAG, "presence token reset");
    } else {
        ESP_LOGW(TAG, "reset token failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static int presence_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (attr_handle != s_presence_chr_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, s_last_payload, strlen(s_last_payload));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t copied = 0;
        char payload[128] = { 0 };
        int rc = ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload) - 1, &copied);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        payload[copied] = 0;
        strlcpy(s_last_payload, payload, sizeof(s_last_payload));
        ESP_LOGI(TAG, "presence write len=%u: %s", copied, payload);

        char event[24];
        char token[65];
        presence_extract_json_string(payload, "event", event, sizeof(event));
        presence_extract_json_string(payload, "token", token, sizeof(token));

        if (strcmp(event, "bind") == 0) {
            if (s_presence_token[0] != 0) {
                ESP_LOGW(TAG, "presence bind rejected: already bound");
                strlcpy(s_last_payload, "{\"state\":\"bound\",\"result\":\"already_bound\"}", sizeof(s_last_payload));
                return 0;
            }
            esp_err_t save_ret = presence_save_token(token);
            strlcpy(s_last_payload,
                    save_ret == ESP_OK ? "{\"state\":\"bound\",\"result\":\"ok\"}" :
                                         "{\"state\":\"unbound\",\"result\":\"invalid_token\"}",
                    sizeof(s_last_payload));
            return 0;
        }

        if (strcmp(event, "reset_bind") == 0) {
            if (s_presence_token[0] == 0 || strcmp(token, s_presence_token) != 0) {
                ESP_LOGW(TAG, "presence reset rejected: token mismatch");
                strlcpy(s_last_payload, "{\"state\":\"bound\",\"result\":\"token_mismatch\"}", sizeof(s_last_payload));
                return 0;
            }
            presence_clear_token();
            return 0;
        }

        if (strcmp(event, "nearby") == 0 || strstr(payload, "nearby")) {
            if (s_presence_token[0] == 0 || strcmp(token, s_presence_token) != 0) {
                ESP_LOGW(TAG, "presence nearby rejected: token mismatch");
                strlcpy(s_last_payload, "{\"state\":\"bound\",\"result\":\"token_mismatch\"}", sizeof(s_last_payload));
                return 0;
            }
            strlcpy(s_last_payload, "{\"state\":\"bound\",\"result\":\"nearby_ok\"}", sizeof(s_last_payload));
            if (s_nearby_cb) {
                s_nearby_cb(payload);
            }
            int term_rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            if (term_rc != 0) {
                ESP_LOGW(TAG, "terminate presence client failed: %d", term_rc);
            }
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_presence_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_presence_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_presence_chr_uuid.u,
                .access_cb = presence_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_presence_chr_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static esp_err_t presence_advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set adv fields failed: %d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, presence_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "start advertising failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "advertising as %s", name);
    return ESP_OK;
}

static int presence_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "client %s status=%d",
                 event->connect.status == 0 ? "connected" : "connect failed",
                 event->connect.status);
        if (event->connect.status != 0) {
            presence_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "client disconnected, reason=%d", event->disconnect.reason);
        presence_advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising complete, reason=%d", event->adv_complete.reason);
        presence_advertise();
        return 0;
    default:
        return 0;
    }
}

static void presence_on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason=%d", reason);
}

static void presence_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr failed: %d", rc);
        return;
    }
    presence_advertise();
}

static void presence_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "host task started");
    nimble_port_run();
    s_host_running = false;
    nimble_port_freertos_deinit();
}

static esp_err_t presence_gatt_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_presence_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count gatt cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_presence_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add gatt svc failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "presence gatt service registered");
    return ESP_OK;
}

esp_err_t presence_service_start(presence_service_nearby_cb_t nearby_cb)
{
    if (s_started) {
        s_nearby_cb = nearby_cb;
        return ESP_OK;
    }

    s_nearby_cb = nearby_cb;
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.reset_cb = presence_on_reset;
    ble_hs_cfg.sync_cb = presence_on_sync;

    ret = presence_gatt_init();
    if (ret != ESP_OK) {
        nimble_port_deinit();
        return ret;
    }
    presence_load_token();

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    char name[16];
    snprintf(name, sizeof(name), "S31_%02X%02X%02X", mac[3], mac[4], mac[5]);
    int rc = ble_svc_gap_device_name_set(name);
    if (rc != 0) {
        ESP_LOGE(TAG, "set device name failed: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    s_started = true;
    s_host_running = true;
    nimble_port_freertos_init(presence_host_task);
    ESP_LOGI(TAG, "Presence service started, name=%s", name);
    return ESP_OK;
}

bool presence_service_is_running(void)
{
    return s_started && s_host_running;
}
