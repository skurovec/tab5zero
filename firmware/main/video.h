#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "protocol.h"

/* Clockwise rotation of the picture relative to the default landscape orientation.
   90° and 270° show the 1280 × 720 frame scaled down on the portrait panel. */
typedef enum {
    ROTATION_0,
    ROTATION_90,
    ROTATION_180,
    ROTATION_270,
    ROTATION_COUNT,
} rotation_t;

esp_err_t video_start(void);
void video_submit(const uint8_t *data, size_t length);
/* Redraws the last frame, for example after the menu changed. */
void video_refresh(void);
rotation_t video_rotation(void);
/* Applies and persists a rotation. */
void video_set_rotation(rotation_t rotation);
/* Maps a touch in native panel coordinates to frame coordinates; false outside the picture. */
bool video_touch_to_frame(uint16_t panel_x, uint16_t panel_y, uint16_t *x, uint16_t *y);
esp_err_t input_start(void);
