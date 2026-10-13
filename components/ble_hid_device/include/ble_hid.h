#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_hid_init(const char *device_name);
esp_err_t ble_hid_send_report(const uint8_t *report, size_t length);
bool ble_hid_is_connected(void);

#ifdef __cplusplus
}
#endif
