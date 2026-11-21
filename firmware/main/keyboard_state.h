#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint8_t code, mod; } keyboard_key_t;
typedef struct {
    bool held[70];
    keyboard_key_t down[70];
} keyboard_state_t;

bool keyboard_state_event(keyboard_state_t *state, uint8_t raw);
void keyboard_state_report(const keyboard_state_t *state, uint8_t report[8]);
