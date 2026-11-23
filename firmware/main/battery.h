#pragma once

#include "esp_err.h"

typedef enum {
    BATTERY_MISSING,
    BATTERY_CHARGING,
    BATTERY_DISCHARGING,
    BATTERY_IDLE,
} battery_status_t;

typedef struct {
    int pack_mv;
    int level_percent;
    /* Positive while charging. */
    int milliamps;
    battery_status_t status;
} battery_reading_t;

/* Starts periodic logging of the Tab5 battery read from its INA226 power monitor. */
esp_err_t battery_start(void);

/* Reads the battery now; fails with ESP_ERR_INVALID_STATE before a successful battery_start(). */
esp_err_t battery_read(battery_reading_t *reading);

/* Switches the Tab5 off; returns only if the power path stays on, for example on USB power. */
esp_err_t battery_power_off(void);
