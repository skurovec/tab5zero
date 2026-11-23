#include "battery.h"

#include "bsp/m5stack_tab5.h"
#include "esp_check.h"
#include "freertos/task.h"

/* Register values follow M5Unified's Power_Class for the Tab5: an INA226 at 0x41 on the
   system I2C bus, 5 mΩ shunt, 2 A full scale, charge current reading negative. */
enum {
    INA226_ADDRESS = 0x41,
    REGISTER_CONFIG = 0x00,
    REGISTER_BUS_VOLTAGE = 0x02,
    REGISTER_CURRENT = 0x04,
    REGISTER_CALIBRATION = 0x05,
    REGISTER_DIE_ID = 0xff,
    DIE_ID = 0x2260,
    /* 16-sample average, 1.1 ms bus and shunt conversion, continuous. */
    CONFIG = 0x0527,
    /* 0.00512 / (2 A / 32768 × 0.005 Ω). */
    CALIBRATION = 0x4189,
    LOG_INTERVAL_MS = 10000,
};

/* Two Li-ion cells; the level is a linear estimate between these per-cell voltages. */
static const int CELL_EMPTY_MV = 3300, CELL_FULL_MV = 4150;
/* Below the 6.0 V shutdown threshold of the 2S pack by a margin; lower readings mean no pack. */
static const int NO_BATTERY_MV = 5000;

static const char *TAG = "battery";
static i2c_master_dev_handle_t monitor;
/* Set once the INA226 is identified and configured, before any reader can observe it. */
static bool ready;

static esp_err_t read_register(uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(monitor, &reg, 1, data, sizeof(data), 50),
                        TAG, "Reading INA226 register 0x%02x", reg);
    *value = (uint16_t)data[0] << 8 | data[1];
    return ESP_OK;
}

static esp_err_t write_register(uint8_t reg, uint16_t value)
{
    const uint8_t data[] = {reg, value >> 8, value & 0xff};
    return i2c_master_transmit(monitor, data, sizeof(data), 50);
}

static int level_percent(int pack_mv)
{
    int level = (pack_mv / 2 - CELL_EMPTY_MV) * 100 / (CELL_FULL_MV - CELL_EMPTY_MV);
    return level < 0 ? 0 : level > 100 ? 100 : level;
}

esp_err_t battery_read(battery_reading_t *reading)
{
    ESP_RETURN_ON_FALSE(ready, ESP_ERR_INVALID_STATE, TAG, "Battery monitor not started");
    uint16_t voltage_raw, current_raw;
    ESP_RETURN_ON_ERROR(read_register(REGISTER_BUS_VOLTAGE, &voltage_raw), TAG, "Battery voltage");
    ESP_RETURN_ON_ERROR(read_register(REGISTER_CURRENT, &current_raw), TAG, "Battery current");
    reading->pack_mv = voltage_raw * 125 / 100;
    /* Current LSB is 2 A / 32768; the shunt reads charge current as negative. */
    reading->milliamps = -(int16_t)current_raw * 2000 / 32768;
    reading->level_percent = level_percent(reading->pack_mv);
    // An absent or protection-tripped pack leaves only leakage on the bus pin.
    reading->status = reading->pack_mv < NO_BATTERY_MV ? BATTERY_MISSING
                    : reading->milliamps > 20          ? BATTERY_CHARGING
                    : reading->milliamps < -20         ? BATTERY_DISCHARGING
                                                       : BATTERY_IDLE;
    return ESP_OK;
}

esp_err_t battery_power_off(void)
{
    esp_io_expander_handle_t power = bsp_io_expander1_init();
    ESP_RETURN_ON_FALSE(power, ESP_FAIL, TAG, "Power IO expander");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(power, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_OUTPUT), TAG, "PWROFF_PULSE direction");
    // Same pulse train as M5Unified Power_Class::powerOff for the Tab5.
    for (unsigned i = 0; i < 10; ++i) {
        ESP_RETURN_ON_ERROR(esp_io_expander_set_level(power, IO_EXPANDER_PIN_NUM_4, i & 1), TAG, "PWROFF_PULSE");
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_OK;
}

static const char *status_name(battery_status_t status)
{
    switch (status) {
    case BATTERY_CHARGING: return "charging";
    case BATTERY_DISCHARGING: return "discharging";
    case BATTERY_IDLE: return "idle";
    case BATTERY_MISSING: break;
    }
    return "missing";
}

static void battery_task(void *argument)
{
    for (;;) {
        battery_reading_t reading;
        esp_err_t result = battery_read(&reading);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Battery read failed: %s", esp_err_to_name(result));
        } else if (reading.status == BATTERY_MISSING) {
            ESP_LOGW(TAG, "No battery voltage (%d mV): pack missing, not contacting or cut off by its protection",
                     reading.pack_mv);
        } else {
            ESP_LOGI(TAG, "%d.%02d V, about %d %%, %+d mA (%s)", reading.pack_mv / 1000, reading.pack_mv % 1000 / 10,
                     reading.level_percent, reading.milliamps, status_name(reading.status));
        }
        vTaskDelay(pdMS_TO_TICKS(LOG_INTERVAL_MS));
    }
}

esp_err_t battery_start(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "System I2C bus");
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &device, &monitor), TAG, "INA226 device");
    uint16_t id;
    ESP_RETURN_ON_ERROR(read_register(REGISTER_DIE_ID, &id), TAG, "INA226 identification");
    ESP_RETURN_ON_FALSE(id == DIE_ID, ESP_ERR_NOT_FOUND, TAG, "Unexpected INA226 die ID 0x%04x", id);
    ESP_RETURN_ON_ERROR(write_register(REGISTER_CONFIG, CONFIG), TAG, "INA226 configuration");
    ESP_RETURN_ON_ERROR(write_register(REGISTER_CALIBRATION, CALIBRATION), TAG, "INA226 calibration");
    ready = true;
    ESP_RETURN_ON_FALSE(xTaskCreate(battery_task, "battery", 3072, NULL, 2, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Battery task");
    return ESP_OK;
}
