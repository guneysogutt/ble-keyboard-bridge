#include "battery.h"
#include "esp_log.h"

static const char *TAG = "BATTERY";

esp_err_t battery_monitor_init(int adc_channel) {
    ESP_LOGI(TAG, "Initializing ADC Battery Monitor...");
    return ESP_OK;
}

uint8_t battery_get_percentage(void) {
    return 100;
}

uint32_t battery_get_voltage_mv(void) {
    return 4200;
}
