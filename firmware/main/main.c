#include "bsp/m5stack_tab5.h"
#include "usb_link.h"
#include "video.h"

void app_main(void)
{
    // USB-A 5 V powers and charges the Cardputer Zero attached as the USB device.
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_USB, true));
    esp_io_expander_handle_t power = bsp_io_expander1_init();
    ESP_ERROR_CHECK(power ? ESP_OK : ESP_FAIL);
    // PI4IOE5V6408 at 0x44: P7 is IP2326 CHG_EN, P5 is its active-low QC select (low = 1000 mA).
    // Driver initialization resets every pin to an input; a floating QC select kept a deeply
    // discharged pack from charging, so both pins are driven as the stock firmware does.
    const uint32_t charger_pins = IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_7;
    ESP_ERROR_CHECK(esp_io_expander_set_dir(power, charger_pins, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(power, IO_EXPANDER_PIN_NUM_5, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(power, IO_EXPANDER_PIN_NUM_7, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_output_mode(power, charger_pins, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL));

    ESP_ERROR_CHECK(video_start());
    ESP_ERROR_CHECK(input_start());
    ESP_ERROR_CHECK(usb_link_start());
}
