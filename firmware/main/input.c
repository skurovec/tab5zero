#include "video.h"
#include "usb_link.h"
#include <string.h>
#include "bsp/touch.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "keyboard_state.h"

static const char *TAG = "input";
static i2c_master_dev_handle_t keyboard;
static esp_lcd_touch_handle_t touch;
static keyboard_state_t keyboard_state;

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(keyboard, &reg, 1, value, 1, 20);
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(keyboard, data, sizeof(data), 20);
}

static void publish_keyboard(void)
{
    uint8_t report[8], record[RECORD_SIZE];
    keyboard_state_report(&keyboard_state, report);
    encode_keyboard(record, report);
    usb_link_input(record);
}

static void input_task(void *argument)
{
    bool present = false;
    int64_t next_probe = 0;
    uint16_t last_x = 0, last_y = 0;
    unsigned touch_failures = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (!present && now >= next_probe) {
            next_probe = now + 2000000;
            uint8_t version;
            esp_err_t result = read_register(0xfe, &version);
            if (result == ESP_OK) result = write_register(0x10, 0);
            if (result == ESP_OK) result = write_register(0x02, 0);
            if (result == ESP_OK) {
                present = true;
                ESP_LOGI(TAG, "Keyboard found, firmware 0x%02x", version);
            } else {
                ESP_LOGD(TAG, "Keyboard probe: %s; retry in 2s", esp_err_to_name(result));
            }
        }
        if (present) {
            uint8_t count = 0;
            esp_err_t result = read_register(0x02, &count);
            for (unsigned i = 0; result == ESP_OK && i < count && i < 32; ++i) {
                uint8_t raw;
                result = read_register(0x20, &raw);
                if (result != ESP_OK || raw == 0xff) continue;
                if (keyboard_state_event(&keyboard_state, raw)) {
                    // Publish per event so a press and release in one batch both reach the host.
                    publish_keyboard();
                } else {
                    ESP_LOGW(TAG, "Invalid keyboard event 0x%02x", raw);
                }
            }
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Keyboard detached/read failed: %s; retry in 2s", esp_err_to_name(result));
                present = false;
                memset(&keyboard_state, 0, sizeof(keyboard_state));
            }
        }
        // Also publishes the cleared state after a keyboard detach.
        publish_keyboard();
        uint8_t record[RECORD_SIZE];
        esp_err_t result = esp_lcd_touch_read_data(touch);
        esp_lcd_touch_point_data_t point;
        uint8_t points = 0;
        if (result == ESP_OK) {
            result = esp_lcd_touch_get_data(touch, &point, &points, 1);
        }
        bool pressed = result == ESP_OK && points != 0;
        uint16_t x = last_x, y = last_y;
        if (result == ESP_OK) {
            touch_failures = 0;
            if (pressed) {
                // Inverse of the display's clockwise pixel rotation.
                x = point.y < VIDEO_WIDTH ? point.y : VIDEO_WIDTH - 1;
                y = point.x < VIDEO_HEIGHT ? VIDEO_HEIGHT - 1 - point.x : 0;
            }
        } else {
            ++touch_failures;
            if (touch_failures == 1 || touch_failures % 100 == 0) {
                ESP_LOGW(TAG, "Touch read failed: %s; release pointer and retry", esp_err_to_name(result));
            }
        }
        encode_pointer(record, x, y, pressed);
        usb_link_input(record);
        last_x = x;
        last_y = y;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t input_start(void)
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_0,
        .scl_io_num = GPIO_NUM_1,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "Keyboard I2C bus");
    const i2c_device_config_t device = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x6d, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &device, &keyboard), TAG, "Keyboard I2C device");
    const gpio_config_t interrupt = {.pin_bit_mask = 1ULL << 50, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&interrupt), TAG, "Keyboard interrupt GPIO");
    ESP_RETURN_ON_ERROR(bsp_touch_new(NULL, &touch), TAG, "Touch controller");
    ESP_RETURN_ON_FALSE(xTaskCreate(input_task, "physical_input", 6144, NULL, 4, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "Input task");
    return ESP_OK;
}
