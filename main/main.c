#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include <assert.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "usb_hid_host.h"
#include "ble_hid.h"
// #include "battery.h" // Comment out temporarily

static const char *TAG = "APP_MAIN";
static QueueHandle_t s_kb_queue = NULL;

static void keyboard_consumer_task(void *pvParameters) {
    hid_keyboard_report_t report;
    ESP_LOGI(TAG, "Keyboard consumer task started on Core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(s_kb_queue, &report, portMAX_DELAY) == pdTRUE) {
            if (ble_hid_is_connected()) {
                ble_hid_send_report((const uint8_t *)&report, sizeof(report));
            }
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Starting ESP32-S3 USB Host to BLE Bridge...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_kb_queue = xQueueCreate(16, sizeof(hid_keyboard_report_t));
    configASSERT(s_kb_queue != NULL);

    // 1. Initialize BLE first
    ESP_ERROR_CHECK(ble_hid_init("Wireless Mechanical KB"));

    // 2. Spawn consumer task
    xTaskCreatePinnedToCore(keyboard_consumer_task, "kb_consumer", 4096, NULL, 5, NULL, 1);

    // 3. Give NimBLE 500ms to register and start advertising before initializing USB Host
    vTaskDelay(pdMS_TO_TICKS(500));

    // 4. Initialize USB Host
    ESP_ERROR_CHECK(usb_hid_host_init(s_kb_queue));

    // ESP_ERROR_CHECK(battery_monitor_init(0)); // Keep disabled until BLE is verified

    ESP_LOGI(TAG, "System operational.");
}
