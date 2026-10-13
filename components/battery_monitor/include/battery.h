#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t battery_monitor_init(int adc_channel);
uint8_t battery_get_percentage(void);
uint32_t battery_get_voltage_mv(void);

#ifdef __cplusplus
}
#endif
