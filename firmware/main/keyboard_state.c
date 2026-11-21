#include "keyboard_state.h"
#include <string.h>
#include "keyboard_map.h"

bool keyboard_state_event(keyboard_state_t *state, uint8_t raw)
{
    unsigned row = (raw >> 4) & 7, column = raw & 15;
    if (row >= 5 || column >= 14) {
        return false;
    }
    unsigned index = row * 14 + column;
    state->held[index] = (raw & 0x80) != 0;
    if (!state->held[index]) {
        state->down[index] = (KbdKey){0};
        return true;
    }
    KbdKey key = KBD_BASE[index];
    if (state->held[KIDX_SYM]) {
        for (unsigned i = 0; i < sizeof(KBD_SYM) / sizeof(KBD_SYM[0]); ++i) {
            if (KBD_SYM[i].idx == index) key = KBD_SYM[i].key;
        }
    }
    bool symbol = (key.code >= 0x1e && key.code <= 0x27) || (key.code >= 0x2d && key.code <= 0x38);
    if (state->held[KIDX_AA] && !symbol) key.mod |= 2;
    state->down[index] = key;
    return true;
}

void keyboard_state_report(const keyboard_state_t *state, uint8_t report[8])
{
    memset(report, 0, 8);
    report[0] = (state->held[KIDX_CTRL] ? 1 : 0) | (state->held[KIDX_ALT] ? 4 : 0);
    unsigned count = 0;
    for (unsigned i = 0; i < 70; ++i) {
        if (!state->down[i].code) continue;
        report[0] |= state->down[i].mod;
        if (count < 6) report[2 + count] = state->down[i].code;
        ++count;
    }
    if (count > 6) memset(report + 2, 1, 6); // HID ErrorRollOver until enough keys are released.
}

