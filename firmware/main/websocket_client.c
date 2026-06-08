#include "websocket_client.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "sdkconfig.h"

static const char *TAG = "ws";
#define AUDIO_FRAME_MAX_BYTES (960 * sizeof(int16_t)) /* 60 ms @ 16 kHz mono PCM fallback max */
#define AUDIO_QUEUE_DEPTH 6

typedef struct {
    size_t bytes;
    uint8_t data[AUDIO_FRAME_MAX_BYTES];
} websocket_audio_frame_t;

static websocket_client_status_cb_t s_status_cb = NULL;
static websocket_client_binary_cb_t s_binary_cb = NULL;
static websocket_client_tts_state_cb_t s_tts_state_cb = NULL;
static websocket_client_chat_cb_t s_chat_cb = NULL;
static websocket_client_can_restart_listen_cb_t s_can_restart_listen_cb = NULL;
static QueueHandle_t s_audio_queue = NULL;
static volatile bool s_audio_streaming = false;
static volatile bool s_listen_stop_requested = false;
static volatile bool s_task_running = false;
static volatile bool s_presence_greeting_requested = false;
static char s_session_id[96] = { 0 };

static void notify_status(websocket_client_status_t status, const char *detail)
{
    if (s_status_cb) {
        s_status_cb(status, detail);
    }
}

static void copy_json_string_value(const char *data, const char *key, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = 0;

    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(data, pattern);
    if (!p) {
        return;
    }

    p = strchr(p + strlen(pattern), ':');
    if (!p) {
        return;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return;
    }
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_size) {
        out[i++] = *p++;
    }
    out[i] = 0;
}

static bool copy_json_int_value(const char *data, const char *key, int *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(data, pattern);
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
    if (*p < '0' || *p > '9') {
        return false;
    }

    int value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    *out = value;
    return true;
}

static bool parse_server_hello(const char *data, size_t len)
{
    (void)len;
    bool accepted = false;
    char type[16];
    char transport[16];
    copy_json_string_value(data, "type", type, sizeof(type));
    copy_json_string_value(data, "transport", transport, sizeof(transport));

    if (strcmp(type, "hello") == 0) {
        if (strcmp(transport, "websocket") == 0) {
            char session_id[64];
            copy_json_string_value(data, "session_id", session_id, sizeof(session_id));
            strlcpy(s_session_id, session_id, sizeof(s_session_id));
            ESP_LOGI(TAG, "server hello accepted, session=%s",
                session_id[0] ? session_id : "none");
            notify_status(WEBSOCKET_CLIENT_STATUS_HELLO_ACCEPTED, "Hello accepted");
            accepted = true;
        } else {
            ESP_LOGW(TAG, "server hello has unsupported transport");
            notify_status(WEBSOCKET_CLIENT_STATUS_REJECTED, "Server hello rejected");
        }
    } else {
        notify_status(WEBSOCKET_CLIENT_STATUS_TEXT_MESSAGE, "Server message received");
    }

    return accepted;
}

static int websocket_send_text(esp_transport_handle_t ws, char *text, int timeout_ms)
{
    return esp_transport_ws_send_raw(ws,
        (ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN),
        text, strlen(text), timeout_ms);
}

static int websocket_send_mcp_result(esp_transport_handle_t ws, int id, const char *result_json)
{
    char message[512];
    snprintf(message, sizeof(message),
        "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}}",
        s_session_id, id, result_json);
    int ret = websocket_send_text(ws, message, 3000);
    return ret;
}

static void websocket_send_mcp_error(esp_transport_handle_t ws, int id, const char *error)
{
    char message[384];
    snprintf(message, sizeof(message),
        "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"message\":\"%s\"}}}",
        s_session_id, id, error);
    websocket_send_text(ws, message, 3000);
}

static bool websocket_start_audio_stream(esp_transport_handle_t ws, const char *reason);
static void websocket_stop_audio_stream(esp_transport_handle_t ws, const char *reason);

static void websocket_handle_mcp_message(esp_transport_handle_t ws, const char *data)
{
    if (!strstr(data, "\"type\":\"mcp\"")) {
        return;
    }

    int id = 0;
    if (!copy_json_int_value(data, "id", &id)) {
        return;
    }

    char method[32];
    copy_json_string_value(data, "method", method, sizeof(method));

    if (strcmp(method, "initialize") == 0) {
        websocket_send_mcp_result(ws, id,
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
            "\"serverInfo\":{\"name\":\"esp32-s31-korvo-1\",\"version\":\"0.1.0\"}}");
    } else if (strcmp(method, "tools/list") == 0) {
        websocket_send_mcp_result(ws, id, "{\"tools\":[]}");
    } else if (strcmp(method, "tools/call") == 0) {
        websocket_send_mcp_error(ws, id, "No tools registered");
    } else {
        websocket_send_mcp_error(ws, id, "Method not implemented");
    }
}

static void websocket_handle_chat_message(esp_transport_handle_t ws, const char *data);

static void websocket_log_idle(esp_transport_handle_t ws, bool hello_accepted, int idle_reads)
{
    if ((!hello_accepted && idle_reads % 5 == 0) || (hello_accepted && idle_reads % 100 == 0)) {
        ESP_LOGI(TAG, "%s idle=%d errno=%d",
            hello_accepted ? "waiting server message..." : "waiting server hello...",
            idle_reads, esp_transport_get_errno(ws));
    }
}

static bool websocket_read_should_idle(int err)
{
    return err == 0 || err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

static int websocket_send_binary(esp_transport_handle_t ws, uint8_t *data, size_t bytes, int timeout_ms)
{
    return esp_transport_ws_send_raw(ws,
        (ws_transport_opcodes_t)(WS_TRANSPORT_OPCODES_BINARY | WS_TRANSPORT_OPCODES_FIN),
        (char *)data, bytes, timeout_ms);
}

static bool websocket_send_listen_state(esp_transport_handle_t ws, const char *state, const char *mode)
{
    char message[192];
    if (mode && mode[0]) {
        snprintf(message, sizeof(message),
            "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\",\"mode\":\"%s\"}",
            s_session_id, state, mode);
    } else {
        snprintf(message, sizeof(message),
            "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"%s\"}",
            s_session_id, state);
    }

    int ret = websocket_send_text(ws, message, 3000);
    ESP_LOGI(TAG, "listen %s sent: %d bytes", state, ret);
    return ret >= 0;
}

static bool websocket_start_audio_stream(esp_transport_handle_t ws, const char *reason)
{
    if (s_audio_streaming) {
        return true;
    }
    if (s_audio_queue) {
        xQueueReset(s_audio_queue);
    }
    s_listen_stop_requested = false;
    if (!websocket_send_listen_state(ws, "start", "auto")) {
        return false;
    }
    s_audio_streaming = true;
    notify_status(WEBSOCKET_CLIENT_STATUS_LISTENING, "Listening");
    ESP_LOGI(TAG, "audio upload enabled: %s", reason ? reason : "start");
    return true;
}

static bool websocket_send_presence_greeting(esp_transport_handle_t ws)
{
    const char *greeting_text = "My phone is nearby. Please greet me in Chinese.";
    char message[256];
    snprintf(message, sizeof(message),
        "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"detect\","
        "\"text\":\"My phone is nearby. Please greet me in Chinese.\"}",
        s_session_id);

    int ret = websocket_send_text(ws, message, 3000);
    ESP_LOGI(TAG, "presence greeting sent: %d bytes, text=%s", ret, greeting_text);
    if (ret >= 0) {
        notify_status(WEBSOCKET_CLIENT_STATUS_THINKING, "Presence greeting sent");
        return true;
    }
    notify_status(WEBSOCKET_CLIENT_STATUS_ERROR, "Presence greeting failed");
    return false;
}

static void websocket_handle_chat_message(esp_transport_handle_t ws, const char *data)
{
    char type[16];
    char state[24];
    char text[256];
    copy_json_string_value(data, "type", type, sizeof(type));
    copy_json_string_value(data, "state", state, sizeof(state));
    copy_json_string_value(data, "text", text, sizeof(text));

    if (strcmp(type, "stt") == 0 && text[0]) {
        ESP_LOGI(TAG, "user text: %s", text);
    } else if (strcmp(type, "tts") == 0 && text[0]) {
        ESP_LOGI(TAG, "tts text state=%s: %s", state[0] ? state : "-", text);
    }

    if (s_chat_cb && (strcmp(type, "stt") == 0 || strcmp(type, "tts") == 0)) {
        s_chat_cb(type, state, text);
    }

    if (strcmp(type, "tts") == 0) {
        if (s_tts_state_cb && state[0]) {
            s_tts_state_cb(state);
        }
        if (strcmp(state, "start") == 0) {
            websocket_stop_audio_stream(ws, "tts start");
            notify_status(WEBSOCKET_CLIENT_STATUS_SPEAKING, "Speaking");
        }
        if (strcmp(state, "stop") == 0) {
            ESP_LOGI(TAG, "tts stopped; wait playback drain");
        }
    }
}

static void websocket_drain_audio_queue(esp_transport_handle_t ws)
{
    static int total_sent = 0;
    websocket_audio_frame_t frame;
    int sent = 0;
    size_t last_bytes = 0;
    while (s_audio_streaming && s_audio_queue &&
           xQueueReceive(s_audio_queue, &frame, 0) == pdPASS) {
        int ret = websocket_send_binary(ws, frame.data, frame.bytes, 1000);
        if (ret < 0) {
            ESP_LOGW(TAG, "audio frame send failed: %d errno=%d", ret, esp_transport_get_errno(ws));
            break;
        }
        sent++;
        total_sent++;
        last_bytes = frame.bytes;
    }

    if (sent > 0 && total_sent % 50 == 0) {
        ESP_LOGI(TAG, "audio frames sent total=%d last_bytes=%u", total_sent, (unsigned)last_bytes);
    }
}

static void websocket_stop_audio_stream(esp_transport_handle_t ws, const char *reason)
{
    if (!s_audio_streaming) {
        return;
    }
    s_audio_streaming = false;
    if (s_audio_queue) {
        xQueueReset(s_audio_queue);
    }
    s_listen_stop_requested = false;
    websocket_send_listen_state(ws, "stop", NULL);
    if (reason && strcmp(reason, "vad") == 0) {
        notify_status(WEBSOCKET_CLIENT_STATUS_THINKING, "Thinking");
    }
    ESP_LOGI(TAG, "audio upload stopped: %s", reason ? reason : "requested");
}

static void websocket_task(void *arg)
{
    if (strlen(CONFIG_KORVO_WS_HOST) == 0) {
        ESP_LOGW(TAG, "WebSocket host is empty; skip connection");
        s_task_running = false;
        vTaskDelete(NULL);
    }

    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    if (!tcp || !ws) {
        ESP_LOGE(TAG, "create transport failed");
        if (ws) esp_transport_destroy(ws);
        if (tcp) esp_transport_destroy(tcp);
        s_task_running = false;
        vTaskDelete(NULL);
    }

    char headers[256] = { 0 };
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(headers, sizeof(headers),
        "Protocol-Version: 1\r\n"
        "Device-Id: %02x:%02x:%02x:%02x:%02x:%02x\r\n"
        "Client-Id: esp32-s31-korvo-1\r\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_transport_ws_set_path(ws, CONFIG_KORVO_WS_PATH);
    esp_transport_ws_set_headers(ws, headers);
    if (strlen(CONFIG_KORVO_WS_TOKEN) > 0) {
        esp_transport_ws_set_auth(ws, CONFIG_KORVO_WS_TOKEN);
    }

    ESP_LOGI(TAG, "connecting ws://%s:%d%s", CONFIG_KORVO_WS_HOST,
        CONFIG_KORVO_WS_PORT, CONFIG_KORVO_WS_PATH);
    notify_status(WEBSOCKET_CLIENT_STATUS_CONNECTING, "WebSocket connecting");
    int ret = esp_transport_connect(ws, CONFIG_KORVO_WS_HOST, CONFIG_KORVO_WS_PORT, 10000);
    if (ret < 0) {
        ESP_LOGE(TAG, "connect failed, status=%d errno=%d",
            esp_transport_ws_get_upgrade_request_status(ws),
            esp_transport_get_errno(ws));
        notify_status(WEBSOCKET_CLIENT_STATUS_ERROR, "WebSocket connect failed");
        esp_transport_destroy(ws);
        s_task_running = false;
        vTaskDelete(NULL);
    }
    notify_status(WEBSOCKET_CLIENT_STATUS_CONNECTED, "WebSocket connected");

    char hello[] =
        "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},"
        "\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
        "\"channels\":1,\"frame_duration\":60}}";
    ret = websocket_send_text(ws, hello, 5000);
    ESP_LOGI(TAG, "hello sent: %d bytes", ret);
    if (ret < 0) {
        notify_status(WEBSOCKET_CLIENT_STATUS_ERROR, "Hello send failed");
        esp_transport_close(ws);
        esp_transport_destroy(ws);
        s_task_running = false;
        vTaskDelete(NULL);
    }
    notify_status(WEBSOCKET_CLIENT_STATUS_HELLO_SENT, "Hello sent");

    bool hello_accepted = false;
    bool restart_listen_after_tts = false;
    bool tts_response_active = false;
    int64_t restart_listen_earliest = 0;
    int64_t restart_listen_deadline = 0;
    int64_t hello_deadline = esp_timer_get_time() + 30000000LL;
    int idle_reads = 0;
    char rx[1024];
    while (1) {
        websocket_drain_audio_queue(ws);
        if (s_listen_stop_requested) {
            websocket_stop_audio_stream(ws, "vad");
        }
        if (hello_accepted && s_presence_greeting_requested) {
            s_presence_greeting_requested = false;
            if (s_audio_streaming) {
                websocket_stop_audio_stream(ws, "presence greeting");
            }
            websocket_send_presence_greeting(ws);
        }
        int len = esp_transport_read(ws, rx, sizeof(rx) - 1, hello_accepted ? 50 : 1000);
        if (len > 0) {
            idle_reads = 0;
            ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(ws);
            if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
                rx[len] = 0;
                if (parse_server_hello(rx, len)) {
                    hello_accepted = true;
                    if (s_presence_greeting_requested) {
                        s_presence_greeting_requested = false;
                        websocket_send_presence_greeting(ws);
                    } else {
                        websocket_start_audio_stream(ws, "hello accepted");
                    }
                } else {
                    websocket_handle_mcp_message(ws, rx);
                    char msg_type[16];
                    char msg_state[24];
                    copy_json_string_value(rx, "type", msg_type, sizeof(msg_type));
                    copy_json_string_value(rx, "state", msg_state, sizeof(msg_state));
                    if (strcmp(msg_type, "tts") == 0 && strcmp(msg_state, "start") == 0) {
                        restart_listen_after_tts = false;
                        tts_response_active = true;
                    } else if (strcmp(msg_type, "tts") == 0 && strcmp(msg_state, "stop") == 0 && tts_response_active) {
                        restart_listen_after_tts = true;
                        tts_response_active = false;
                        restart_listen_earliest = esp_timer_get_time() + 500000LL;
                        restart_listen_deadline = esp_timer_get_time() + 3000000LL;
                    }
                    websocket_handle_chat_message(ws, rx);
                }
            } else {
                if (s_binary_cb) {
                    s_binary_cb((const uint8_t *)rx, (size_t)len);
                }
                notify_status(WEBSOCKET_CLIENT_STATUS_BINARY_MESSAGE, "Binary message received");
            }
        } else if (len < 0) {
            int err = esp_transport_get_errno(ws);
            if (websocket_read_should_idle(err)) {
                idle_reads++;
                websocket_log_idle(ws, hello_accepted, idle_reads);
                continue;
            }
            ESP_LOGW(TAG, "read ended: %d errno=%d", len, err);
            notify_status(hello_accepted ? WEBSOCKET_CLIENT_STATUS_DISCONNECTED : WEBSOCKET_CLIENT_STATUS_REJECTED,
                hello_accepted ? "WebSocket disconnected" : "Server rejected");
            break;
        } else {
            idle_reads++;
            websocket_log_idle(ws, hello_accepted, idle_reads);
        }

        if (!hello_accepted && esp_timer_get_time() > hello_deadline) {
            ESP_LOGE(TAG, "failed to receive server hello within 30 seconds");
            notify_status(WEBSOCKET_CLIENT_STATUS_REJECTED, "Hello timeout");
            break;
        }

        if (restart_listen_after_tts && !s_audio_streaming) {
            int64_t now = esp_timer_get_time();
            bool can_restart = (now > restart_listen_earliest &&
                                (!s_can_restart_listen_cb || s_can_restart_listen_cb())) ||
                               now > restart_listen_deadline;
            if (can_restart) {
                restart_listen_after_tts = false;
                ESP_LOGI(TAG, "tts playback drained; restart listen");
                websocket_start_audio_stream(ws, "tts stop");
            }
        }
    }

    if (hello_accepted && s_audio_streaming) {
        websocket_stop_audio_stream(ws, "disconnect");
    }
    if (s_audio_queue) {
        xQueueReset(s_audio_queue);
    }
    s_session_id[0] = 0;
    esp_transport_close(ws);
    esp_transport_destroy(ws);
    s_task_running = false;
    vTaskDelete(NULL);
}

void websocket_client_set_status_callback(websocket_client_status_cb_t callback)
{
    s_status_cb = callback;
}

void websocket_client_set_binary_callback(websocket_client_binary_cb_t callback)
{
    s_binary_cb = callback;
}

void websocket_client_set_tts_state_callback(websocket_client_tts_state_cb_t callback)
{
    s_tts_state_cb = callback;
}

void websocket_client_set_chat_callback(websocket_client_chat_cb_t callback)
{
    s_chat_cb = callback;
}

void websocket_client_set_can_restart_listen_callback(websocket_client_can_restart_listen_cb_t callback)
{
    s_can_restart_listen_cb = callback;
}

esp_err_t websocket_client_start(void)
{
    if (strlen(CONFIG_KORVO_WS_HOST) == 0) {
        ESP_LOGW(TAG, "WebSocket host is empty; skip connection");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_running) {
        ESP_LOGW(TAG, "WebSocket client is already running");
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_audio_queue) {
        s_audio_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(websocket_audio_frame_t));
        if (!s_audio_queue) {
            return ESP_ERR_NO_MEM;
        }
    } else {
        xQueueReset(s_audio_queue);
    }
    s_audio_streaming = false;
    s_listen_stop_requested = false;
    s_task_running = true;

    BaseType_t ok = xTaskCreate(websocket_task, "ws_client", 7168, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_task_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool websocket_client_is_running(void)
{
    return s_task_running;
}

bool websocket_client_is_audio_streaming(void)
{
    return s_audio_streaming && s_audio_queue;
}

esp_err_t websocket_client_send_audio_frame(const uint8_t *data, size_t bytes)
{
    if (!websocket_client_is_audio_streaming()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || bytes == 0 || bytes > AUDIO_FRAME_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    websocket_audio_frame_t frame = { 0 };
    frame.bytes = bytes;
    memcpy(frame.data, data, bytes);

    if (xQueueSend(s_audio_queue, &frame, 0) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

void websocket_client_request_listen_stop(void)
{
    if (s_audio_streaming) {
        s_listen_stop_requested = true;
    }
}

void websocket_client_request_presence_greeting(void)
{
    s_presence_greeting_requested = true;
}
