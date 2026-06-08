#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*presence_service_nearby_cb_t)(const char *payload);

esp_err_t presence_service_start(presence_service_nearby_cb_t nearby_cb);
bool presence_service_is_running(void);
