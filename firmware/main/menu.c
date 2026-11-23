#include "menu.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "battery.h"
#include "esp_log.h"
#include "font8x8.h"
#include "video.h"

typedef struct {
    int x, y, width, height;
} rect_t;

enum {
    SCALE = 4,
    GLYPH = 8 * SCALE,
    TEXT_LEFT = 250,
};

/* RGB565, red in the high bits, matching the decoder output. */
enum {
    BLACK = 0x0000,
    WHITE = 0xffff,
    PANEL_COLOR = 0x2104,
    BORDER_COLOR = 0x8410,
    BUTTON_COLOR = 0x4208,
    SELECTED_COLOR = 0x07e0,
    WARNING_COLOR = 0xf800,
};

static const rect_t PANEL = {200, 100, 880, 520};
static const rect_t ROTATION_BUTTONS[ROTATION_COUNT] = {
    {250, 330, 170, 90}, {450, 330, 170, 90}, {650, 330, 170, 90}, {850, 330, 170, 90},
};
static const char *const ROTATION_LABELS[ROTATION_COUNT] = {"0\xb0", "90\xb0", "180\xb0", "270\xb0"};
static const rect_t CLOSE_BUTTON = {250, 470, 370, 90};
static const rect_t POWER_BUTTON = {650, 470, 370, 90};

static const char *TAG = "menu";
static atomic_bool open, power_off_armed;

bool menu_is_open(void)
{
    return atomic_load(&open);
}

void menu_open(void)
{
    atomic_store(&power_off_armed, false);
    atomic_store(&open, true);
    video_refresh();
}

static bool contains(rect_t rect, int x, int y)
{
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

void menu_tap(uint16_t x, uint16_t y)
{
    if (!menu_is_open()) return;
    if (!contains(PANEL, x, y) || contains(CLOSE_BUTTON, x, y)) {
        atomic_store(&open, false);
    } else if (contains(POWER_BUTTON, x, y)) {
        // The first tap only arms the button, so a stray touch cannot switch the Tab5 off.
        if (atomic_exchange(&power_off_armed, true)) {
            esp_err_t result = battery_power_off();
            ESP_LOGW(TAG, "Still running after power off (%s); USB power keeps the Tab5 on",
                     esp_err_to_name(result));
            atomic_store(&power_off_armed, false);
        }
    } else {
        atomic_store(&power_off_armed, false);
        for (rotation_t rotation = ROTATION_0; rotation < ROTATION_COUNT; ++rotation) {
            if (contains(ROTATION_BUTTONS[rotation], x, y)) video_set_rotation(rotation);
        }
    }
    video_refresh();
}

static void fill(uint16_t *frame, rect_t rect, uint16_t color)
{
    for (int row = rect.y; row < rect.y + rect.height; ++row) {
        uint16_t *line = frame + row * VIDEO_WIDTH;
        for (int column = rect.x; column < rect.x + rect.width; ++column) line[column] = color;
    }
}

static void outline(uint16_t *frame, rect_t rect, int thickness, uint16_t color)
{
    fill(frame, (rect_t){rect.x, rect.y, rect.width, thickness}, color);
    fill(frame, (rect_t){rect.x, rect.y + rect.height - thickness, rect.width, thickness}, color);
    fill(frame, (rect_t){rect.x, rect.y, thickness, rect.height}, color);
    fill(frame, (rect_t){rect.x + rect.width - thickness, rect.y, thickness, rect.height}, color);
}

static const uint8_t *glyph(unsigned char character)
{
    if (character == 0xb0) return font8x8[FONT_DEGREE];
    if (character < FONT_FIRST || character > FONT_LAST) character = '?';
    return font8x8[character - FONT_FIRST];
}

static void text(uint16_t *frame, int x, int y, const char *string, uint16_t color)
{
    for (; *string; ++string, x += GLYPH) {
        const uint8_t *rows = glyph((unsigned char)*string);
        for (int row = 0; row < 8; ++row) {
            for (int column = 0; column < 8; ++column) {
                if (rows[row] & (1 << column)) {
                    fill(frame, (rect_t){x + column * SCALE, y + row * SCALE, SCALE, SCALE}, color);
                }
            }
        }
    }
}

static void button(uint16_t *frame, rect_t rect, const char *label, uint16_t background, uint16_t foreground)
{
    fill(frame, rect, background);
    int width = (int)strlen(label) * GLYPH;
    text(frame, rect.x + (rect.width - width) / 2, rect.y + (rect.height - GLYPH) / 2, label, foreground);
}

static void battery_lines(uint16_t *frame)
{
    battery_reading_t reading;
    char line[32];
    uint16_t status_color = WHITE;
    const char *status = line;
    if (battery_read(&reading) != ESP_OK) {
        text(frame, TEXT_LEFT, 150, "Battery unavailable", WHITE);
        return;
    }
    if (reading.status == BATTERY_MISSING) {
        text(frame, TEXT_LEFT, 150, "Battery not connected", WHITE);
        return;
    }
    snprintf(line, sizeof(line), "Battery %d.%02d V %3d %%", reading.pack_mv / 1000,
             reading.pack_mv % 1000 / 10, reading.level_percent);
    text(frame, TEXT_LEFT, 150, line, WHITE);
    if (reading.status == BATTERY_CHARGING) {
        snprintf(line, sizeof(line), "Charging %d mA", reading.milliamps);
        status_color = SELECTED_COLOR;
    } else if (reading.status == BATTERY_DISCHARGING) {
        snprintf(line, sizeof(line), "Discharging %d mA", -reading.milliamps);
    } else {
        status = "Not charging";
    }
    text(frame, TEXT_LEFT, 210, status, status_color);
}

void menu_draw(uint16_t *frame)
{
    if (!menu_is_open()) return;
    fill(frame, PANEL, PANEL_COLOR);
    outline(frame, PANEL, 4, BORDER_COLOR);
    battery_lines(frame);
    text(frame, TEXT_LEFT, 280, "Rotation", WHITE);
    rotation_t current = video_rotation();
    for (rotation_t rotation = ROTATION_0; rotation < ROTATION_COUNT; ++rotation) {
        bool selected = rotation == current;
        button(frame, ROTATION_BUTTONS[rotation], ROTATION_LABELS[rotation],
               selected ? SELECTED_COLOR : BUTTON_COLOR, selected ? BLACK : WHITE);
    }
    button(frame, CLOSE_BUTTON, "Close", BUTTON_COLOR, WHITE);
    bool armed = atomic_load(&power_off_armed);
    button(frame, POWER_BUTTON, armed ? "Confirm" : "Power off", armed ? WARNING_COLOR : BUTTON_COLOR, WHITE);
}
