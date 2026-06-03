/* ESP32-S31 Korvo-1 综合固件：LCD + 触控 + LVGL + ES8389 音频 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt1151.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "lvgl.h"
#include "board.h"
#include "wifi_manager.h"
#include "websocket_client.h"
#include "opus_encoder.h"
#include "tts_player.h"
#include "cbin_font.h"
#include "font_emoji.h"

#if __has_include("avatar_assets/croc_avatar_assets.h")
#include "avatar_assets/croc_avatar_assets.h"
#define HAVE_CROC_AVATAR_ASSETS 1
#else
#define HAVE_CROC_AVATAR_ASSETS 0
#endif

LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);

static const char *TAG = "main";

extern const uint8_t font_puhui_common_16_4_bin_start[] asm("_binary_font_puhui_common_16_4_bin_start");
extern const uint8_t font_puhui_common_16_4_bin_end[] asm("_binary_font_puhui_common_16_4_bin_end");

/* ====== LCD / LVGL ====== */
#define LVGL_TICK_MS        2
#define LVGL_TASK_STACK     (6 * 1024)
#define LVGL_TASK_PRIO      2
#define LVGL_TASK_MAX_DELAY 500
#define LVGL_TASK_MIN_DELAY 100

static _lock_t lvgl_lock;
static lv_display_t *lvgl_disp = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *mic_label = NULL;
static lv_obj_t *user_text_label = NULL;
static lv_obj_t *assistant_text_label = NULL;
static lv_obj_t *avatar_head = NULL;
static lv_obj_t *avatar_left_eye = NULL;
static lv_obj_t *avatar_right_eye = NULL;
static lv_obj_t *avatar_mouth = NULL;
static lv_obj_t *avatar_tongue = NULL;
static lv_obj_t *avatar_img_base = NULL;
static lv_obj_t *avatar_img_mouth = NULL;
static lv_obj_t *avatar_img_blink = NULL;
static lv_obj_t *avatar_img_expression = NULL;
static lv_obj_t *avatar_state_label = NULL;
static lv_obj_t *reconnect_btn = NULL;
static lv_font_t *dialog_font = NULL;
static esp_lcd_touch_handle_t tp_handle = NULL;
static lv_obj_t *wifi_panel = NULL;
static lv_obj_t *wifi_dropdown = NULL;
static lv_obj_t *wifi_password_ta = NULL;
static lv_obj_t *wifi_keyboard = NULL;
static lv_obj_t *selected_wifi_label = NULL;
static char selected_wifi_ssid[33];
static char selected_wifi_password[65] = "18689922388";
static char wifi_options[20 * 40];
static char user_text[256] = "You: --";
static char assistant_text[512] = "Esp32: --";

typedef enum {
    AVATAR_STATE_IDLE = 0,
    AVATAR_STATE_LISTENING,
    AVATAR_STATE_THINKING,
    AVATAR_STATE_SPEAKING,
    AVATAR_STATE_ERROR,
} avatar_state_t;

static volatile avatar_state_t avatar_state = AVATAR_STATE_IDLE;
static volatile int avatar_tts_rms = 0;
static volatile int avatar_tts_peak = 0;
static volatile int64_t avatar_tts_level_us = 0;
static volatile bool avatar_visible = false;

#define AVATAR_IMAGE_X 40
#define AVATAR_IMAGE_Y 92

/* ====== Audio ====== */
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static esp_codec_dev_handle_t codec_handle = NULL;

#define VAD_SPEECH_RMS_THRESHOLD 180
#define VAD_SILENCE_RMS_THRESHOLD 120
#define VAD_SILENCE_FRAMES_TO_STOP 16 /* 16 * 60 ms ~= 1 second */
#define VAD_SPEECH_FRAMES_TO_START 2  /* 2 * 60 ms */
#define VAD_STARTUP_IGNORE_FRAMES 5   /* 5 * 60 ms ~= 300 ms */
#define VAD_MAX_STREAM_FRAMES 200     /* 200 * 60 ms ~= 12 seconds */

static void ui_set_status(const char *text)
{
    if (!status_label) {
        return;
    }
    _lock_acquire(&lvgl_lock);
    lv_label_set_text(status_label, text);
    _lock_release(&lvgl_lock);
}

static void ui_set_mic_stats(int rms, int peak)
{
    if (!mic_label) {
        return;
    }
    _lock_acquire(&lvgl_lock);
    lv_label_set_text_fmt(mic_label, "Mic RMS: %d | Peak: %d", rms, peak);
    _lock_release(&lvgl_lock);
}

static void ui_set_reconnect_visible(bool visible)
{
    if (!reconnect_btn) {
        return;
    }
    _lock_acquire(&lvgl_lock);
    if (visible) {
        lv_obj_remove_flag(reconnect_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(reconnect_btn, LV_OBJ_FLAG_HIDDEN);
    }
    _lock_release(&lvgl_lock);
}

static const char *avatar_state_text(avatar_state_t state)
{
    switch (state) {
    case AVATAR_STATE_LISTENING:
        return "Listening";
    case AVATAR_STATE_THINKING:
        return "Thinking";
    case AVATAR_STATE_SPEAKING:
        return "Speaking";
    case AVATAR_STATE_ERROR:
        return "Error";
    case AVATAR_STATE_IDLE:
    default:
        return "Idle";
    }
}

static void ui_set_avatar_state(avatar_state_t state)
{
    avatar_state = state;
}

static void ui_set_avatar_visible_locked(bool visible)
{
    avatar_visible = visible;
    lv_obj_t *objs[] = {
        avatar_head,
        avatar_img_base,
        avatar_img_mouth,
        avatar_img_blink,
        avatar_img_expression,
        avatar_state_label,
        user_text_label,
        assistant_text_label,
    };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (!objs[i]) {
            continue;
        }
#if HAVE_CROC_AVATAR_ASSETS
        if (objs[i] == avatar_head || objs[i] == avatar_img_blink || objs[i] == avatar_img_expression) {
            lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
#endif
        if (visible) {
            lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void tts_level_cb(int rms, int peak)
{
    avatar_tts_rms = rms;
    avatar_tts_peak = peak;
    avatar_tts_level_us = esp_timer_get_time();
}

#if HAVE_CROC_AVATAR_ASSETS
static void avatar_set_image_layer(lv_obj_t *obj, const croc_avatar_layer_t *layer)
{
    if (!obj || !layer || !layer->img) {
        return;
    }
    lv_image_set_src(obj, layer->img);
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, AVATAR_IMAGE_X + layer->x, AVATAR_IMAGE_Y + layer->y);
}
#endif

static void avatar_anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!avatar_visible) {
        return;
    }

    int64_t now = esp_timer_get_time();
    avatar_state_t state = avatar_state;
    int rms = avatar_tts_rms;
    int peak = avatar_tts_peak;
    bool level_fresh = (now - avatar_tts_level_us) < 180000;
    int blink_phase = (int)((now / 100000) % 36);

#if HAVE_CROC_AVATAR_ASSETS
    if (avatar_img_base && avatar_img_mouth) {
        const croc_avatar_layer_t *mouth_layer = &croc_avatar_mouth_0_layer;
        if (state == AVATAR_STATE_SPEAKING && level_fresh) {
            int open = MIN(MAX((rms - 220) / 80, 0), 22);
            int peak_open = MIN(MAX((peak - 1400) / 900, 0), 10);
            open = MAX(open, peak_open);
            int mouth_phase = (int)((now / 85000) % 4);
            if (open < 5) {
                mouth_layer = &croc_avatar_mouth_0_layer;
            } else if (open < 12 || mouth_phase == 0) {
                mouth_layer = &croc_avatar_mouth_1_layer;
            } else if (mouth_phase == 1) {
                mouth_layer = &croc_avatar_mouth_3_layer;
            } else if (mouth_phase == 2) {
                mouth_layer = &croc_avatar_mouth_2_layer;
            } else if (open >= 16) {
                mouth_layer = &croc_avatar_mouth_4_layer;
            } else {
                mouth_layer = &croc_avatar_mouth_2_layer;
            }
        }
        avatar_set_image_layer(avatar_img_mouth, mouth_layer);

        if (avatar_img_blink) {
            if (blink_phase == 0) {
                avatar_set_image_layer(avatar_img_blink, &croc_avatar_blink_layer);
                lv_obj_remove_flag(avatar_img_blink, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(avatar_img_blink, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (avatar_img_expression) {
            if (state == AVATAR_STATE_THINKING) {
                avatar_set_image_layer(avatar_img_expression, &croc_avatar_thinking_layer);
                lv_obj_remove_flag(avatar_img_expression, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(avatar_img_expression, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (avatar_state_label) {
            lv_label_set_text(avatar_state_label, avatar_state_text(state));
        }
        return;
    }
#endif

    if (!avatar_head || !avatar_left_eye || !avatar_right_eye || !avatar_mouth) {
        return;
    }

    lv_color_t face_color = lv_color_hex(0xfde68a);
    lv_color_t mouth_color = lv_color_hex(0x334155);
    int eye_h = 30;
    int eye_y = 74;
    int mouth_w = 88;
    int mouth_h = 12;
    int mouth_y = 178;
    bool tongue_visible = false;

    if (blink_phase == 0) {
        eye_h = 5;
        eye_y = 88;
    }

    switch (state) {
    case AVATAR_STATE_LISTENING:
        face_color = lv_color_hex(0xbfdbfe);
        mouth_h = 10;
        mouth_y = 182;
        break;
    case AVATAR_STATE_THINKING:
        face_color = lv_color_hex(0xd8b4fe);
        eye_h = 16;
        eye_y = 82;
        mouth_w = 54;
        mouth_h = 8;
        mouth_y = 188;
        break;
    case AVATAR_STATE_SPEAKING:
        face_color = lv_color_hex(0xfde68a);
        if (level_fresh) {
            int open = MIN(MAX((rms - 180) / 55, 0), 46);
            int peak_open = MIN(MAX((peak - 900) / 650, 0), 18);
            open = MAX(open, peak_open);
            int mouth_phase = (int)((now / 85000) % 4);
            if (mouth_phase == 0) {
                mouth_w = 98;
                mouth_h = 12 + open / 2;
            } else if (mouth_phase == 1) {
                mouth_w = 64 + open / 2;
                mouth_h = 18 + open;
            } else if (mouth_phase == 2) {
                mouth_w = 88 + open / 3;
                mouth_h = 14 + open * 2 / 3;
            } else {
                mouth_w = 54 + open / 3;
                mouth_h = 16 + open * 4 / 5;
            }
            mouth_y = 184 - mouth_h / 2;
            tongue_visible = mouth_h > 30;
        } else {
            mouth_h = 12;
            mouth_w = 80;
            mouth_y = 180;
        }
        break;
    case AVATAR_STATE_ERROR:
        face_color = lv_color_hex(0xfca5a5);
        mouth_color = lv_color_hex(0x7f1d1d);
        mouth_h = 8;
        mouth_w = 96;
        mouth_y = 194;
        break;
    case AVATAR_STATE_IDLE:
    default:
        face_color = lv_color_hex(0x86efac);
        break;
    }

    lv_obj_set_style_bg_color(avatar_head, face_color, 0);
    lv_obj_set_size(avatar_left_eye, 34, eye_h);
    lv_obj_set_size(avatar_right_eye, 34, eye_h);
    lv_obj_align(avatar_left_eye, LV_ALIGN_TOP_LEFT, 70, eye_y);
    lv_obj_align(avatar_right_eye, LV_ALIGN_TOP_LEFT, 166, eye_y);
    lv_obj_set_size(avatar_mouth, mouth_w, mouth_h);
    lv_obj_set_style_bg_color(avatar_mouth, mouth_color, 0);
    lv_obj_align(avatar_mouth, LV_ALIGN_TOP_MID, 0, mouth_y);
    if (avatar_tongue) {
        if (tongue_visible) {
            lv_obj_remove_flag(avatar_tongue, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(avatar_tongue, mouth_w * 3 / 5, MAX(mouth_h / 4, 8));
            lv_obj_align(avatar_tongue, LV_ALIGN_BOTTOM_MID, 0, -4);
        } else {
            lv_obj_add_flag(avatar_tongue, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (avatar_state_label) {
        lv_label_set_text(avatar_state_label, avatar_state_text(state));
    }
}

static void ui_set_dialog_text(const char *user, const char *assistant)
{
    _lock_acquire(&lvgl_lock);
    if (user && user_text_label) {
        lv_label_set_text(user_text_label, user);
    }
    if (assistant && assistant_text_label) {
        lv_label_set_text(assistant_text_label, assistant);
    }
    _lock_release(&lvgl_lock);
}

static void ui_apply_chinese_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, dialog_font ? dialog_font : &lv_font_source_han_sans_sc_16_cjk, 0);
}

static void sanitize_dialog_text(const char *in, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    out[0] = 0;
    if (!in) {
        return;
    }

    size_t pos = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && pos + 1 < out_size) {
        if ((*p & 0x80) == 0) {
            if (*p >= 0x20 || *p == '\n') {
                out[pos++] = (char)*p;
            }
            p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1]) {
            if (pos + 2 >= out_size) {
                break;
            }
            out[pos++] = (char)p[0];
            out[pos++] = (char)p[1];
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            if (pos + 3 >= out_size) {
                break;
            }
            out[pos++] = (char)p[0];
            out[pos++] = (char)p[1];
            out[pos++] = (char)p[2];
            p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            if (pos + 4 >= out_size) {
                break;
            }
            out[pos++] = (char)p[0];
            out[pos++] = (char)p[1];
            out[pos++] = (char)p[2];
            out[pos++] = (char)p[3];
            p += 4;
        } else {
            p++;
        }
    }
    out[pos] = 0;
}

static void ui_init_dialog_font(void)
{
    if (dialog_font) {
        return;
    }

    dialog_font = cbin_font_create((uint8_t *)font_puhui_common_16_4_bin_start);
    if (!dialog_font) {
        ESP_LOGW(TAG, "xiaozhi cbin font create failed; use LVGL CJK font");
        return;
    }

    const lv_font_t *emoji_font = font_emoji_32_init();
    if (emoji_font) {
        dialog_font->fallback = emoji_font;
    }

    ESP_LOGI(TAG, "xiaozhi dialog font ready: %u bytes",
        (unsigned)(font_puhui_common_16_4_bin_end - font_puhui_common_16_4_bin_start));
}

static void ui_set_wifi_panel_visible(bool visible)
{
    if (!wifi_panel) {
        return;
    }
    _lock_acquire(&lvgl_lock);
    if (visible) {
        lv_obj_remove_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
        ui_set_avatar_visible_locked(false);
    } else {
        lv_obj_add_flag(wifi_panel, LV_OBJ_FLAG_HIDDEN);
        ui_set_avatar_visible_locked(true);
        if (wifi_keyboard) {
            lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_keyboard_set_textarea(wifi_keyboard, NULL);
        }
    }
    lv_obj_invalidate(lv_scr_act());
    _lock_release(&lvgl_lock);
}

static void websocket_status_cb(websocket_client_status_t status, const char *detail)
{
    switch (status) {
    case WEBSOCKET_CLIENT_STATUS_CONNECTING:
        ui_set_reconnect_visible(false);
        ui_set_status("WiFi connected | WebSocket connecting");
        break;
    case WEBSOCKET_CLIENT_STATUS_CONNECTED:
        ui_set_reconnect_visible(false);
        ui_set_status("WiFi connected | WebSocket connected");
        break;
    case WEBSOCKET_CLIENT_STATUS_HELLO_SENT:
        ui_set_status("WiFi connected | Hello sent");
        break;
    case WEBSOCKET_CLIENT_STATUS_HELLO_ACCEPTED:
        ui_set_reconnect_visible(false);
        ui_set_status("WiFi connected | Hello accepted");
        ui_set_avatar_state(AVATAR_STATE_IDLE);
        break;
    case WEBSOCKET_CLIENT_STATUS_TEXT_MESSAGE:
        break;
    case WEBSOCKET_CLIENT_STATUS_BINARY_MESSAGE:
        break;
    case WEBSOCKET_CLIENT_STATUS_REJECTED:
        ui_set_status(detail ? detail : "Server rejected");
        ui_set_avatar_state(AVATAR_STATE_ERROR);
        ui_set_reconnect_visible(true);
        break;
    case WEBSOCKET_CLIENT_STATUS_DISCONNECTED:
        ui_set_status("WebSocket disconnected");
        ui_set_avatar_state(AVATAR_STATE_IDLE);
        ui_set_reconnect_visible(true);
        break;
    case WEBSOCKET_CLIENT_STATUS_LISTENING:
        ui_set_reconnect_visible(false);
        ui_set_status("Listening");
        ui_set_avatar_state(AVATAR_STATE_LISTENING);
        break;
    case WEBSOCKET_CLIENT_STATUS_THINKING:
        ui_set_status("Thinking");
        ui_set_avatar_state(AVATAR_STATE_THINKING);
        break;
    case WEBSOCKET_CLIENT_STATUS_SPEAKING:
        ui_set_status("Speaking");
        ui_set_avatar_state(AVATAR_STATE_SPEAKING);
        break;
    case WEBSOCKET_CLIENT_STATUS_ERROR:
    default:
        ui_set_status(detail ? detail : "WebSocket error");
        ui_set_avatar_state(AVATAR_STATE_ERROR);
        ui_set_reconnect_visible(true);
        break;
    }
}

static void websocket_binary_cb(const uint8_t *data, size_t bytes)
{
    esp_err_t ret = tts_player_enqueue_opus(data, bytes);
    if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "queue tts opus failed: %s", esp_err_to_name(ret));
    }
}

static void websocket_tts_state_cb(const char *state)
{
    if (state && strcmp(state, "start") == 0) {
        avatar_tts_rms = 0;
        avatar_tts_peak = 0;
        avatar_tts_level_us = 0;
        tts_player_reset();
    }
}

static bool websocket_can_restart_listen_cb(void)
{
    return !tts_player_is_busy();
}

static void websocket_chat_cb(const char *type, const char *state, const char *text)
{
    if (!type) {
        return;
    }

    if (strcmp(type, "stt") == 0 && text && text[0]) {
        char clean[220];
        sanitize_dialog_text(text, clean, sizeof(clean));
        snprintf(user_text, sizeof(user_text), "You: %s", clean);
        ui_set_dialog_text(user_text, NULL);
    } else if (strcmp(type, "tts") == 0) {
        if (state && strcmp(state, "start") == 0) {
            strlcpy(assistant_text, "Esp32: ", sizeof(assistant_text));
            ui_set_dialog_text(NULL, assistant_text);
        } else if (state && strcmp(state, "sentence_start") == 0 && text && text[0]) {
            char clean[220];
            sanitize_dialog_text(text, clean, sizeof(clean));
            if (strcmp(assistant_text, "Esp32: ") != 0) {
                strlcat(assistant_text, " ", sizeof(assistant_text));
            }
            strlcat(assistant_text, clean, sizeof(assistant_text));
            ui_set_dialog_text(NULL, assistant_text);
        }
    }
}

static void mic_rms_task(void *arg)
{
    const int frame_count = audio_opus_encoder_input_samples(); /* 60 ms @ 16 kHz */
    const int channel_count = 2;
    int16_t *samples = heap_caps_malloc(frame_count * channel_count * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int16_t *mono = heap_caps_malloc(frame_count * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *encoded = heap_caps_malloc(audio_opus_encoder_output_max_bytes(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!samples || !mono || !encoded) {
        ESP_LOGE(TAG, "mic rms buffer alloc failed");
        free(samples);
        free(mono);
        free(encoded);
        vTaskDelete(NULL);
    }

    int frames = 0;
    double sum_sq_acc = 0;
    int32_t peak_acc = 0;
    bool vad_streaming_prev = false;
    bool vad_speech_seen = false;
    int vad_silence_frames = 0;
    int vad_speech_frames = 0;
    int vad_stream_frames = 0;

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(rx_handle, samples, frame_count * channel_count * sizeof(int16_t),
                                         &bytes_read, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG, "mic read failed: ret=%s bytes=%u", esp_err_to_name(ret), (unsigned)bytes_read);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int got_frames = bytes_read / (channel_count * sizeof(int16_t));
        if (got_frames <= 0) {
            ESP_LOGW(TAG, "mic read incomplete frame: bytes=%u", (unsigned)bytes_read);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int64_t sum_sq = 0;
        int32_t peak = 0;
        for (int i = 0; i < got_frames; i++) {
            int32_t s = samples[i * channel_count]; /* left channel */
            mono[i] = (int16_t)s;
            int32_t a = abs(s);
            if (a > peak) {
                peak = a;
            }
            sum_sq += (int64_t)s * s;
        }

        sum_sq_acc += (double)sum_sq / got_frames;
        if (peak > peak_acc) {
            peak_acc = peak;
        }
        frames++;

        int rms_now = (int)sqrt((double)sum_sq / got_frames);
        bool audio_streaming = websocket_client_is_audio_streaming();
        if (audio_streaming && !vad_streaming_prev) {
            vad_speech_seen = false;
            vad_silence_frames = 0;
            vad_speech_frames = 0;
            vad_stream_frames = 0;
            ESP_LOGI(TAG, "vad armed: startup_ignore=%d max_frames=%d", VAD_STARTUP_IGNORE_FRAMES, VAD_MAX_STREAM_FRAMES);
        } else if (!audio_streaming && vad_streaming_prev) {
            vad_speech_seen = false;
            vad_silence_frames = 0;
            vad_speech_frames = 0;
            vad_stream_frames = 0;
        }
        vad_streaming_prev = audio_streaming;

        if (audio_streaming) {
            int encoded_bytes = audio_opus_encoder_encode(mono, got_frames, encoded, audio_opus_encoder_output_max_bytes());
            esp_err_t send_ret = encoded_bytes > 0
                ? websocket_client_send_audio_frame(encoded, encoded_bytes)
                : ESP_FAIL;
            if (send_ret != ESP_OK && send_ret != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "queue audio frame failed: %s", esp_err_to_name(send_ret));
            }

            vad_stream_frames++;
            if (vad_stream_frames <= VAD_STARTUP_IGNORE_FRAMES) {
                vad_speech_frames = 0;
                vad_silence_frames = 0;
            } else if (rms_now >= VAD_SPEECH_RMS_THRESHOLD) {
                if (vad_speech_frames < VAD_SPEECH_FRAMES_TO_START) {
                    vad_speech_frames++;
                }
                if (vad_speech_frames >= VAD_SPEECH_FRAMES_TO_START) {
                    vad_speech_seen = true;
                    vad_silence_frames = 0;
                }
            } else if (vad_speech_seen && rms_now <= VAD_SILENCE_RMS_THRESHOLD) {
                vad_silence_frames++;
            } else if (vad_speech_seen) {
                vad_silence_frames = 0;
            } else {
                vad_speech_frames = 0;
            }

            if ((vad_speech_seen && vad_silence_frames >= VAD_SILENCE_FRAMES_TO_STOP) ||
                vad_stream_frames >= VAD_MAX_STREAM_FRAMES) {
                ESP_LOGI(TAG, "vad stop requested: speech=%d speech_frames=%d silence_frames=%d stream_frames=%d rms=%d",
                    vad_speech_seen, vad_speech_frames, vad_silence_frames, vad_stream_frames, rms_now);
                websocket_client_request_listen_stop();
                vad_speech_seen = false;
                vad_silence_frames = 0;
                vad_speech_frames = 0;
                vad_stream_frames = 0;
            }
        }

        if (frames >= 16) {
            int rms = (int)sqrt(sum_sq_acc / frames);
            ui_set_mic_stats(rms, peak_acc);
            frames = 0;
            sum_sq_acc = 0;
            peak_acc = 0;
        }
    }
}

/* Audio init */
static esp_err_t audio_init(i2c_master_bus_handle_t i2c_bus)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle), TAG, "I2S new channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN, .bclk = I2S_BCLK_PIN, .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN, .din = I2S_DIN_PIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = 256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG, "I2S init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_handle, &std_cfg), TAG, "I2S RX init");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx_handle, .tx_handle = tx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if);

    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_PORT, .addr = ES8389_I2C_ADDR, .bus_handle = i2c_bus };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    es8389_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if, .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = ES8389_PA_PIN, .use_mclk = false,
        .master_mode = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&es_cfg);
    assert(codec_if);

    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = codec_if, .data_if = data_if };
    codec_handle = esp_codec_dev_new(&dev_cfg);
    assert(codec_handle);

    esp_codec_dev_sample_info_t sample_cfg = { .bits_per_sample = 16, .channel = 2, .channel_mask = 0x03, .sample_rate = 16000 };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(codec_handle, &sample_cfg), TAG, "codec open");
    esp_codec_dev_set_out_vol(codec_handle, 80);
    esp_codec_dev_set_in_gain(codec_handle, 30.0);
    esp_codec_dev_set_in_mute(codec_handle, false);
    return ESP_OK;
}

/* ====== LVGL callbacks ====== */
static bool notify_flush_ready(esp_lcd_panel_handle_t p, const esp_lcd_rgb_panel_event_data_t *e, void *ud)
{
    lv_display_flush_ready((lv_display_t *)ud);
    return false;
}

static void lvgl_flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    esp_lcd_panel_handle_t p = lv_display_get_user_data(d);
    esp_lcd_panel_draw_bitmap(p, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px);
}

static void lvgl_tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_MS); }

static void lvgl_task(void *arg)
{
    while (1) {
        _lock_acquire(&lvgl_lock);
        uint32_t t = lv_timer_handler();
        _lock_release(&lvgl_lock);
        usleep(1000 * MAX(MIN(t, LVGL_TASK_MAX_DELAY), LVGL_TASK_MIN_DELAY));
    }
}

/* ====== Touch ====== */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    if (!tp_handle) return;
    esp_lcd_touch_read_data(tp_handle);
    uint16_t x, y, strength;
    uint8_t cnt;
    esp_lcd_touch_get_coordinates(tp_handle, &x, &y, &strength, &cnt, 1);
    if (cnt) {
        data->point.x = x; data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void touch_init(lv_display_t *disp, i2c_master_bus_handle_t i2c_bus)
{
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES, .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT1151_ADDRESS,
        .control_phase_bytes = 1, .lcd_cmd_bits = 16,
        .flags.disable_control_phase = 1, .scl_speed_hz = 400000,
    };
    esp_lcd_panel_io_handle_t tp_io;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_cfg, &tp_io));
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt1151(tp_io, &tp_cfg, &tp_handle));
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    ESP_LOGI(TAG, "GT1151 touch ready");
}

/* ====== UI ====== */
static void btn_beep_cb(lv_event_t *e)
{
    if (status_label) lv_label_set_text(status_label, "Playing 1kHz...");
    if (codec_handle && tx_handle) {
        int16_t *buf = heap_caps_malloc(8000 * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM); /* 0.5s @ 16kHz stereo */
        if (buf) {
            for (int i = 0; i < 8000; i++) {
                int16_t v = (int16_t)(sinf(2 * M_PI * 1000 * i / 16000.0f) * 16000);
                buf[i * 2] = v;
                buf[i * 2 + 1] = v;
            }
            esp_codec_dev_write(codec_handle, buf, 8000 * 2 * sizeof(int16_t));
            free(buf);
        }
    }
    if (status_label) lv_label_set_text(status_label, "Ready");
}

static void wifi_ssid_selected_cb(lv_event_t *e)
{
    lv_dropdown_get_selected_str(wifi_dropdown, selected_wifi_ssid, sizeof(selected_wifi_ssid));
    if (selected_wifi_ssid[0] == 0 || strcmp(selected_wifi_ssid, "Scan WiFi first") == 0) return;
    lv_label_set_text_fmt(selected_wifi_label, "Selected: %s", selected_wifi_ssid);
    lv_label_set_text(status_label, "Enter password, then connect");
}

static void wifi_password_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(wifi_keyboard, wifi_password_ta);
        lv_obj_remove_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(wifi_keyboard, NULL);
    }
}

static void wifi_scan_task(void *arg)
{
    ui_set_status("Scanning WiFi...");

    wifi_ap_record_t records[20] = { 0 };
    uint16_t count = 0;
    esp_err_t ret = wifi_manager_scan(records, 20, &count);

    _lock_acquire(&lvgl_lock);
    if (ret != ESP_OK) {
        lv_dropdown_set_options(wifi_dropdown, "Scan failed");
        lv_label_set_text(status_label, "WiFi scan failed");
    } else if (count == 0) {
        lv_dropdown_set_options(wifi_dropdown, "No networks found");
        lv_label_set_text(status_label, "No WiFi found");
    } else {
        wifi_options[0] = 0;
        for (int i = 0; i < count; i++) {
            char ssid[33] = { 0 };
            memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
            if (ssid[0] == 0) {
                continue;
            }
            if (wifi_options[0] != 0) {
                strlcat(wifi_options, "\n", sizeof(wifi_options));
            }
            strlcat(wifi_options, ssid, sizeof(wifi_options));
        }
        lv_dropdown_set_options(wifi_dropdown, wifi_options);
        lv_dropdown_get_selected_str(wifi_dropdown, selected_wifi_ssid, sizeof(selected_wifi_ssid));
        lv_label_set_text_fmt(selected_wifi_label, "Selected: %s", selected_wifi_ssid);
        lv_label_set_text_fmt(status_label, "Found %u networks", count);
    }
    lv_obj_invalidate(lv_scr_act());
    _lock_release(&lvgl_lock);

    vTaskDelete(NULL);
}

static void wifi_scan_cb(lv_event_t *e)
{
    lv_label_set_text(status_label, "Starting scan...");
    xTaskCreate(wifi_scan_task, "wifi_scan", 6144, NULL, 3, NULL);
}

static void websocket_configure_callbacks(void)
{
    websocket_client_set_status_callback(websocket_status_cb);
    websocket_client_set_binary_callback(websocket_binary_cb);
    websocket_client_set_tts_state_callback(websocket_tts_state_cb);
    websocket_client_set_chat_callback(websocket_chat_cb);
    websocket_client_set_can_restart_listen_callback(websocket_can_restart_listen_cb);
}

static void websocket_reconnect_task(void *arg)
{
    (void)arg;
    websocket_configure_callbacks();
    if (websocket_client_is_running()) {
        ui_set_status("WebSocket already running");
        vTaskDelete(NULL);
    }

    ui_set_reconnect_visible(false);
    ui_set_status("WebSocket reconnecting...");
    esp_err_t ret = websocket_client_start();
    if (ret == ESP_ERR_INVALID_STATE) {
        ui_set_status("WebSocket not configured");
        ui_set_reconnect_visible(true);
    } else if (ret != ESP_OK) {
        ui_set_status("WebSocket reconnect failed");
        ui_set_reconnect_visible(true);
    }
    vTaskDelete(NULL);
}

static void websocket_reconnect_cb(lv_event_t *e)
{
    (void)e;
    xTaskCreate(websocket_reconnect_task, "ws_reconnect", 4096, NULL, 3, NULL);
}

static void wifi_connect_task(void *arg)
{
    ui_set_status("Connecting WiFi...");
    esp_err_t ret = wifi_manager_connect(selected_wifi_ssid, selected_wifi_password);
    if (ret != ESP_OK) {
        ui_set_wifi_panel_visible(true);
        ui_set_status("WiFi connect failed");
        vTaskDelete(NULL);
    }

    ui_set_wifi_panel_visible(false);
    ui_set_status("WiFi connected");
    websocket_configure_callbacks();
    ret = websocket_client_start();
    if (ret == ESP_ERR_INVALID_STATE) {
        ui_set_status("WiFi connected | WebSocket not configured");
    } else {
        ui_set_status(ret == ESP_OK ? "WiFi connected | WebSocket starting" : "WiFi connected | WebSocket failed");
    }
    vTaskDelete(NULL);
}

static void wifi_connect_cb(lv_event_t *e)
{
    if (selected_wifi_ssid[0] == 0) {
        lv_label_set_text(status_label, "Select a WiFi first");
        return;
    }

    const char *password = lv_textarea_get_text(wifi_password_ta);
    strlcpy(selected_wifi_password, password ? password : "", sizeof(selected_wifi_password));
    lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(wifi_keyboard, NULL);
    lv_label_set_text(status_label, "Connecting...");
    xTaskCreate(wifi_connect_task, "wifi_connect", 6144, NULL, 3, NULL);
}

static void create_ui(void)
{
    ui_init_dialog_font();

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f172a), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ESP32-S31 Avatar Chat");
    lv_obj_set_style_text_color(title, lv_color_hex(0x38bdf8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *info = lv_label_create(scr);
    lv_label_set_text(info, "LCD OK | Touch OK | Audio OK");
    lv_obj_set_style_text_color(info, lv_color_hex(0x22c55e), 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 42);

    mic_label = lv_label_create(scr);
    lv_label_set_text(mic_label, "Mic RMS: -- | Peak: --");
    lv_obj_set_style_text_color(mic_label, lv_color_hex(0xfacc15), 0);
    lv_obj_set_width(mic_label, LCD_H_RES - 40);
    lv_obj_set_style_text_align(mic_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mic_label, LV_ALIGN_TOP_MID, 0, 66);

    avatar_head = lv_obj_create(scr);
    lv_obj_remove_style_all(avatar_head);
    lv_obj_set_size(avatar_head, 270, 270);
    lv_obj_align(avatar_head, LV_ALIGN_TOP_LEFT, 40, 100);
    lv_obj_set_style_radius(avatar_head, 135, 0);
    lv_obj_set_style_bg_opa(avatar_head, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(avatar_head, lv_color_hex(0x86efac), 0);
    lv_obj_set_style_border_width(avatar_head, 6, 0);
    lv_obj_set_style_border_color(avatar_head, lv_color_hex(0xf8fafc), 0);
    lv_obj_clear_flag(avatar_head, LV_OBJ_FLAG_SCROLLABLE);

    avatar_left_eye = lv_obj_create(avatar_head);
    lv_obj_remove_style_all(avatar_left_eye);
    lv_obj_set_style_bg_opa(avatar_left_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(avatar_left_eye, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_radius(avatar_left_eye, 20, 0);

    avatar_right_eye = lv_obj_create(avatar_head);
    lv_obj_remove_style_all(avatar_right_eye);
    lv_obj_set_style_bg_opa(avatar_right_eye, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(avatar_right_eye, lv_color_hex(0x0f172a), 0);
    lv_obj_set_style_radius(avatar_right_eye, 20, 0);

    avatar_mouth = lv_obj_create(avatar_head);
    lv_obj_remove_style_all(avatar_mouth);
    lv_obj_set_style_bg_opa(avatar_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(avatar_mouth, lv_color_hex(0x334155), 0);
    lv_obj_set_style_radius(avatar_mouth, 28, 0);
    lv_obj_clear_flag(avatar_mouth, LV_OBJ_FLAG_SCROLLABLE);

    avatar_tongue = lv_obj_create(avatar_mouth);
    lv_obj_remove_style_all(avatar_tongue);
    lv_obj_set_style_bg_opa(avatar_tongue, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(avatar_tongue, lv_color_hex(0xf87171), 0);
    lv_obj_set_style_radius(avatar_tongue, 16, 0);
    lv_obj_add_flag(avatar_tongue, LV_OBJ_FLAG_HIDDEN);

#if HAVE_CROC_AVATAR_ASSETS
    lv_obj_add_flag(avatar_head, LV_OBJ_FLAG_HIDDEN);
    avatar_img_base = lv_image_create(scr);
    avatar_set_image_layer(avatar_img_base, &croc_avatar_base_layer);

    avatar_img_mouth = lv_image_create(scr);
    avatar_set_image_layer(avatar_img_mouth, &croc_avatar_mouth_0_layer);

    avatar_img_blink = lv_image_create(scr);
    avatar_set_image_layer(avatar_img_blink, &croc_avatar_blink_layer);
    lv_obj_add_flag(avatar_img_blink, LV_OBJ_FLAG_HIDDEN);

    avatar_img_expression = lv_image_create(scr);
    avatar_set_image_layer(avatar_img_expression, &croc_avatar_thinking_layer);
    lv_obj_add_flag(avatar_img_expression, LV_OBJ_FLAG_HIDDEN);
#endif

    avatar_state_label = lv_label_create(scr);
    lv_label_set_text(avatar_state_label, "Idle");
    lv_obj_set_style_text_color(avatar_state_label, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_width(avatar_state_label, 270);
    lv_obj_set_style_text_align(avatar_state_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(avatar_state_label, LV_ALIGN_TOP_LEFT, 40, 382);
    ui_set_avatar_visible_locked(false);

    user_text_label = lv_label_create(scr);
    lv_label_set_text(user_text_label, user_text);
    lv_obj_set_style_text_color(user_text_label, lv_color_hex(0xe2e8f0), 0);
    ui_apply_chinese_font(user_text_label);
    lv_obj_set_width(user_text_label, 430);
    lv_label_set_long_mode(user_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(user_text_label, LV_ALIGN_TOP_LEFT, 340, 106);

    assistant_text_label = lv_label_create(scr);
    lv_label_set_text(assistant_text_label, assistant_text);
    lv_obj_set_style_text_color(assistant_text_label, lv_color_hex(0xa7f3d0), 0);
    ui_apply_chinese_font(assistant_text_label);
    lv_obj_set_width(assistant_text_label, 430);
    lv_label_set_long_mode(assistant_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(assistant_text_label, LV_ALIGN_TOP_LEFT, 340, 178);
    ui_set_avatar_visible_locked(false);

    reconnect_btn = lv_btn_create(scr);
    lv_obj_set_size(reconnect_btn, 150, 42);
    lv_obj_align(reconnect_btn, LV_ALIGN_BOTTOM_RIGHT, -34, -54);
    lv_obj_set_style_bg_color(reconnect_btn, lv_color_hex(0x2563eb), 0);
    lv_obj_add_event_cb(reconnect_btn, websocket_reconnect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reconnect_lbl = lv_label_create(reconnect_btn);
    lv_label_set_text(reconnect_lbl, "Chat Again");
    lv_obj_center(reconnect_lbl);
    lv_obj_add_flag(reconnect_btn, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(avatar_anim_timer_cb, 80, NULL);

    wifi_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(wifi_panel);
    lv_obj_set_size(wifi_panel, LCD_H_RES, 210);
    lv_obj_align(wifi_panel, LV_ALIGN_TOP_LEFT, 0, 86);

    wifi_dropdown = lv_dropdown_create(wifi_panel);
    lv_obj_set_size(wifi_dropdown, 360, 52);
    lv_obj_align(wifi_dropdown, LV_ALIGN_TOP_LEFT, 40, 34);
    lv_dropdown_set_options(wifi_dropdown, "Scan WiFi first");
    lv_obj_add_event_cb(wifi_dropdown, wifi_ssid_selected_cb, LV_EVENT_VALUE_CHANGED, NULL);

    selected_wifi_label = lv_label_create(wifi_panel);
    lv_label_set_text(selected_wifi_label, "Selected: none");
    lv_obj_set_style_text_color(selected_wifi_label, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_width(selected_wifi_label, 700);
    lv_obj_align(selected_wifi_label, LV_ALIGN_TOP_LEFT, 40, 2);

    wifi_password_ta = lv_textarea_create(wifi_panel);
    lv_obj_set_size(wifi_password_ta, 340, 46);
    lv_obj_align(wifi_password_ta, LV_ALIGN_TOP_LEFT, 420, 34);
    lv_textarea_set_one_line(wifi_password_ta, true);
    lv_textarea_set_password_mode(wifi_password_ta, true);
    lv_textarea_set_text(wifi_password_ta, selected_wifi_password);
    lv_textarea_set_placeholder_text(wifi_password_ta, "Password");
    lv_obj_add_event_cb(wifi_password_ta, wifi_password_focus_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *scan_btn = lv_btn_create(wifi_panel);
    lv_obj_set_size(scan_btn, 150, 52);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_LEFT, 40, 114);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(0x2563eb), 0);
    lv_obj_add_event_cb(scan_btn, wifi_scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_lbl = lv_label_create(scan_btn);
    lv_label_set_text(scan_lbl, "Scan");
    lv_obj_center(scan_lbl);

    lv_obj_t *connect_btn = lv_btn_create(wifi_panel);
    lv_obj_set_size(connect_btn, 170, 52);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_LEFT, 220, 114);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x16a34a), 0);
    lv_obj_add_event_cb(connect_btn, wifi_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, "Connect");
    lv_obj_center(connect_lbl);

    lv_obj_t *btn = lv_btn_create(wifi_panel);
    lv_obj_set_size(btn, 150, 52);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 420, 114);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xff4444), 0);
    lv_obj_add_event_cb(btn, btn_beep_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Beep");
    lv_obj_center(lbl);

    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Ready");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_width(status_label, LCD_H_RES - 40);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    wifi_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(wifi_keyboard, LCD_H_RES, 180);
    lv_obj_align(wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/* ====== LCD init ====== */
static void lcd_init(void)
{
    esp_lcd_rgb_panel_config_t panel_conf = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .dma_burst_size = 64, .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .de_gpio_num = LCD_PIN_DE, .pclk_gpio_num = LCD_PIN_PCLK,
        .vsync_gpio_num = LCD_PIN_VSYNC, .hsync_gpio_num = LCD_PIN_HSYNC,
        .disp_gpio_num = LCD_PIN_DISP,
        .data_gpio_nums = { LCD_PIN_DATA0, LCD_PIN_DATA1, LCD_PIN_DATA2, LCD_PIN_DATA3,
            LCD_PIN_DATA4, LCD_PIN_DATA5, LCD_PIN_DATA6, LCD_PIN_DATA7,
            LCD_PIN_DATA8, LCD_PIN_DATA9, LCD_PIN_DATA10, LCD_PIN_DATA11,
            LCD_PIN_DATA12, LCD_PIN_DATA13, LCD_PIN_DATA14, LCD_PIN_DATA15 },
        .timings = {
            .pclk_hz = 18 * 1000 * 1000,
            .h_res = LCD_H_RES, .v_res = LCD_V_RES,
            .hsync_pulse_width = 40, .hsync_back_porch = 40, .hsync_front_porch = 48,
            .vsync_pulse_width = 23, .vsync_back_porch = 32, .vsync_front_porch = 13,
            .flags.pclk_active_neg = true,
        },
        .flags.fb_in_psram = 1, .num_fbs = 1,
        .bounce_buffer_size_px = LCD_H_RES * 10,
    };

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_conf, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    lv_init();
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(disp, panel);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

    size_t buf_sz = LCD_H_RES * 50 * 2;
    void *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(buf);
    lv_display_set_buffers(disp, buf, NULL, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_color_trans_done = notify_flush_ready };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, disp));

    lvgl_disp = disp;
}

/* ====== Main ====== */
void app_main(void)
{
    ESP_LOGI(TAG, "Korvo-1 Combined Firmware");

    /* I2C bus (shared) */
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_PORT, .sda_io_num = I2C_SDA_PIN, .scl_io_num = I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    /* LCD + LVGL */
    lcd_init();

    /* Touch */
    touch_init(lvgl_disp, i2c_bus);

    /* Audio */
    audio_init(i2c_bus);
    audio_opus_encoder_init();
    tts_player_init(codec_handle);
    tts_player_set_level_callback(tts_level_cb);
    ESP_LOGI(TAG, "audio uplink format: %s", audio_opus_encoder_is_available() ? "opus" : "pcm-debug");
    gpio_set_level(ES8389_PA_PIN, 1);
    xTaskCreate(mic_rms_task, "mic_rms", 32 * 1024, NULL, 2, NULL);

    /* LVGL tick timer */
    esp_timer_handle_t tick_timer;
    esp_timer_create_args_t tick_args = { .callback = lvgl_tick_cb, .name = "lvgl_tick" };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    /* LVGL task */
    xTaskCreate(lvgl_task, "LVGL", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL);

    /* UI */
    _lock_acquire(&lvgl_lock);
    create_ui();
    _lock_release(&lvgl_lock);

    ESP_LOGI(TAG, "All systems ready");
}
