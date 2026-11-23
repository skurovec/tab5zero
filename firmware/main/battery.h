#pragma once

#include "esp_err.h"

/* Starts periodic logging of the Tab5 battery read from its INA226 power monitor. */
esp_err_t battery_start(void);
