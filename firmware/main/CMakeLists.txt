idf_component_register(SRCS "main.c" "video.c" "input.c" "keyboard_state.c" "protocol.c" "usb_link.c"
    INCLUDE_DIRS "."
    REQUIRES m5stack_tab5_noglib usb esp_driver_jpeg esp_driver_ppa esp_driver_i2c esp_driver_gpio esp_lcd esp_lcd_touch esp_timer)
