idf_component_register(SRCS "ble_hid.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt esp_timer nvs_flash)
