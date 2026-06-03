#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define RGB_LED_GPIO    37
#define BTN_ARRAY_ADC   42  // ADC button array (PLAY/SET/VOL-/VOL+)

void app_main(void)
{
    printf("ESP32-S31-Korvo-1 Hello World!\n");
    printf("Chip: %s\n", CONFIG_IDF_TARGET);

    gpio_reset_pin(RGB_LED_GPIO);
    gpio_set_direction(RGB_LED_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        gpio_set_level(RGB_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(RGB_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
