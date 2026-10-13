#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard 8-byte USB HID Keyboard Boot Protocol Report
 */
typedef struct {
    uint8_t modifiers;      /**< Bitfield: LCtrl, LShift, LAlt, LGUI, RCtrl, RShift, RAlt, RGUI */
    uint8_t reserved;       /**< Reserved byte (OEM use) */
    uint8_t keys[6];        /**< Up to 6 concurrent keycodes (HID Usage Table 0x07) */
} __attribute__((packed)) hid_keyboard_report_t;

/**
 * @brief Initialize the USB Host Stack and the HID Host Driver
 *
 * @param keyboard_event_queue Pre-allocated FreeRTOS queue handle of item size sizeof(hid_keyboard_report_t)
 * @return ESP_OK on success, or appropriate error code
 */
esp_err_t usb_hid_host_init(QueueHandle_t keyboard_event_queue);

/**
 * @brief Check if a USB HID keyboard is currently connected and enumerated
 *
 * @return true if keyboard device is active, false otherwise
 */
bool usb_hid_host_is_device_connected(void);

#ifdef __cplusplus
}
#endif
