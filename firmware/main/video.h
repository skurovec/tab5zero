#pragma once

#include "esp_err.h"
#include "protocol.h"

esp_err_t video_start(void);
void video_submit(const uint8_t *data, size_t length);
esp_err_t input_start(void);
