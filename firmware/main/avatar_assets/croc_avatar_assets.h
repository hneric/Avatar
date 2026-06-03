#pragma once
#include "lvgl.h"

typedef struct {
    const lv_image_dsc_t *img;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} croc_avatar_layer_t;

extern const lv_image_dsc_t croc_avatar_base;
extern const croc_avatar_layer_t croc_avatar_base_layer;
extern const lv_image_dsc_t croc_avatar_mouth_0;
extern const croc_avatar_layer_t croc_avatar_mouth_0_layer;
extern const lv_image_dsc_t croc_avatar_mouth_1;
extern const croc_avatar_layer_t croc_avatar_mouth_1_layer;
extern const lv_image_dsc_t croc_avatar_mouth_2;
extern const croc_avatar_layer_t croc_avatar_mouth_2_layer;
extern const lv_image_dsc_t croc_avatar_mouth_3;
extern const croc_avatar_layer_t croc_avatar_mouth_3_layer;
extern const lv_image_dsc_t croc_avatar_mouth_4;
extern const croc_avatar_layer_t croc_avatar_mouth_4_layer;
extern const lv_image_dsc_t croc_avatar_blink;
extern const croc_avatar_layer_t croc_avatar_blink_layer;
extern const lv_image_dsc_t croc_avatar_thinking;
extern const croc_avatar_layer_t croc_avatar_thinking_layer;
