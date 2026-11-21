#include <assert.h>
#include <string.h>
#include "keyboard_state.h"

int main(void)
{
    keyboard_state_t state = {0};
    uint8_t report[8];
    assert(keyboard_state_event(&state, 0xb2)); // A down.
    keyboard_state_report(&state, report);
    assert(report[2] == 4 && report[0] == 0);
    assert(keyboard_state_event(&state, 0x32));
    keyboard_state_report(&state, report);
    assert(!memcmp(report, (uint8_t[8]){0}, 8));

    assert(keyboard_state_event(&state, 0xb0)); // Sym down, then Left becomes Home.
    assert(keyboard_state_event(&state, 0xca));
    keyboard_state_report(&state, report);
    assert(report[2] == 0x4a);
    assert(keyboard_state_event(&state, 0x30)); // Releasing Sym must retain Home until Left release.
    keyboard_state_report(&state, report);
    assert(report[2] == 0x4a);
    assert(keyboard_state_event(&state, 0x4a));
    keyboard_state_report(&state, report);
    assert(report[2] == 0);

    assert(keyboard_state_event(&state, 0xb1)); // Aa adds Shift to letters, not number/symbol keys.
    assert(keyboard_state_event(&state, 0xb2));
    keyboard_state_report(&state, report);
    assert(report[0] == 2 && report[2] == 4);
    memset(&state, 0, sizeof(state));
    assert(keyboard_state_event(&state, 0xb1));
    assert(keyboard_state_event(&state, 0x81));
    keyboard_state_report(&state, report);
    assert(report[0] == 0 && report[2] == 0x1e);

    memset(&state, 0, sizeof(state));
    for (unsigned i = 1; i <= 7; ++i) assert(keyboard_state_event(&state, 0x80 | i));
    keyboard_state_report(&state, report);
    assert(!memcmp(report + 2, (uint8_t[6]){1,1,1,1,1,1}, 6));
    assert(keyboard_state_event(&state, 0x07));
    keyboard_state_report(&state, report);
    assert(report[2] == 0x1e && report[7] == 0x23);
    assert(!keyboard_state_event(&state, 0xff));
    return 0;
}
