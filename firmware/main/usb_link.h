#pragma once

#include "esp_err.h"
#include "protocol.h"

esp_err_t usb_link_start(void);
void usb_link_input(const uint8_t record[RECORD_SIZE]);
