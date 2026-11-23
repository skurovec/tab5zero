#pragma once

#include <stdbool.h>
#include <stdint.h>

/* On-screen menu opened by a long press: battery state, rotation and power off.
   Coordinates are in the 1280 × 720 frame, so the menu rotates with the picture. */
bool menu_is_open(void);
void menu_open(void);
/* Handles a tap released at a frame position while the menu is open. */
void menu_tap(uint16_t x, uint16_t y);
/* Draws the menu over a 1280 × 720 RGB565 frame if it is open. */
void menu_draw(uint16_t *frame);
