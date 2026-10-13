idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES usb_hid_host ble_hid_device battery_monitor
                       PRIV_REQUIRES nvs_flash)
