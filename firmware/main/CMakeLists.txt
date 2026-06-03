file(GLOB EMOJI_SRCS "emoji/*.c")
file(GLOB AVATAR_SRCS "avatar_assets/*.c")

idf_component_register(
    SRCS
        "tts_player.c"
        "cbin_font.c"
        "font_emoji_32.c"
        ${EMOJI_SRCS}
        ${AVATAR_SRCS}
        "opus_encoder.c"
        "wifi_manager.c"
        "websocket_client.c"
        "main.c"
    INCLUDE_DIRS "."
    EMBED_FILES "font_puhui_common_16_4.bin"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_lcd
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        nvs_flash
        tcp_transport
        espressif__esp_codec_dev
        "78__esp-opus"
        espressif__esp_lcd_touch_gt1151
        lvgl__lvgl
)
