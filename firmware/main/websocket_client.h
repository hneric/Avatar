#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    WEBSOCKET_CLIENT_STATUS_CONNECTING = 0,
    WEBSOCKET_CLIENT_STATUS_CONNECTED,
    WEBSOCKET_CLIENT_STATUS_HELLO_SENT,
    WEBSOCKET_CLIENT_STATUS_HELLO_ACCEPTED,
    WEBSOCKET_CLIENT_STATUS_TEXT_MESSAGE,
    WEBSOCKET_CLIENT_STATUS_BINARY_MESSAGE,
    WEBSOCKET_CLIENT_STATUS_REJECTED,
    WEBSOCKET_CLIENT_STATUS_DISCONNECTED,
    WEBSOCKET_CLIENT_STATUS_LISTENING,
    WEBSOCKET_CLIENT_STATUS_THINKING,
    WEBSOCKET_CLIENT_STATUS_SPEAKING,
    WEBSOCKET_CLIENT_STATUS_ERROR,
} websocket_client_status_t;

typedef void (*websocket_client_status_cb_t)(websocket_client_status_t status, const char *detail);
typedef void (*websocket_client_binary_cb_t)(const uint8_t *data, size_t bytes);
typedef void (*websocket_client_tts_state_cb_t)(const char *state);
typedef void (*websocket_client_chat_cb_t)(const char *type, const char *state, const char *text);
typedef bool (*websocket_client_can_restart_listen_cb_t)(void);

void websocket_client_set_status_callback(websocket_client_status_cb_t callback);
void websocket_client_set_binary_callback(websocket_client_binary_cb_t callback);
void websocket_client_set_tts_state_callback(websocket_client_tts_state_cb_t callback);
void websocket_client_set_chat_callback(websocket_client_chat_cb_t callback);
void websocket_client_set_can_restart_listen_callback(websocket_client_can_restart_listen_cb_t callback);
esp_err_t websocket_client_start(void);
bool websocket_client_is_running(void);
bool websocket_client_is_audio_streaming(void);
esp_err_t websocket_client_send_audio_frame(const uint8_t *data, size_t bytes);
void websocket_client_request_listen_stop(void);
void websocket_client_request_presence_greeting(void);
