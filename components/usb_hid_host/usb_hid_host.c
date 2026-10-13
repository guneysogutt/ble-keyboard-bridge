#include "usb_hid_host.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid.h"

static const char *TAG = "USB_HID_HOST";

#define USB_HOST_TASK_PRIORITY      (configMAX_PRIORITIES - 2)
#define USB_HID_TASK_PRIORITY       (configMAX_PRIORITIES - 3)
#define USB_DEVICE_TASK_PRIORITY    (configMAX_PRIORITIES - 4)
#define USB_TASK_STACK_SIZE         (4096)

typedef enum {
    USB_EVT_DEVICE_CONNECTED,
    USB_EVT_DEVICE_DISCONNECTED,
} usb_evt_type_t;

typedef struct {
    usb_evt_type_t type;
    hid_host_device_handle_t handle;
} usb_host_event_msg_t;

static QueueHandle_t s_kb_queue = NULL;
static QueueHandle_t s_usb_evt_queue = NULL;
static hid_host_device_handle_t s_hid_dev_handle = NULL;
static bool s_is_device_connected = false;

/* ---------------- USB Host Daemon Task ---------------- */
static void usb_host_lib_daemon_task(void *pvParameters) {
    ESP_LOGI(TAG, "USB Host Daemon running on Core %d", xPortGetCoreID());

    while (1) {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err == ESP_OK) {
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGD(TAG, "USB Host: No clients registered");
                usb_host_device_free_all();
            }
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                ESP_LOGD(TAG, "USB Host: All devices freed");
            }
        }
    }
}

/* ---------------- HID Report Dispatch Callback ---------------- */
static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                        const hid_host_interface_event_t event,
                                        void *arg) {
    uint8_t report_buf[64];
    size_t report_len = 0;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        esp_err_t ret = hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                                 report_buf,
                                                                 sizeof(report_buf),
                                                                 &report_len);
        if (ret == ESP_OK && report_len >= sizeof(hid_keyboard_report_t)) {
            hid_keyboard_report_t report;
            const uint8_t *src = report_buf;

            // In boot protocol, report is 8 bytes.
            // If device prepended a Report ID (length >= 9), offset by 1.
            if (report_len == sizeof(hid_keyboard_report_t) + 1) {
                src = &report_buf[1];
            }
            memcpy(&report, src, sizeof(hid_keyboard_report_t));

            if (s_kb_queue != NULL) {
                if (xQueueSend(s_kb_queue, &report, 0) != pdPASS) {
                    ESP_LOGW(TAG, "HID report queue full - report dropped");
                }
            }
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
        ESP_LOGI(TAG, "HID device disconnected event received");
        usb_host_event_msg_t msg = {
            .type = USB_EVT_DEVICE_DISCONNECTED,
            .handle = hid_device_handle
        };
        if (s_usb_evt_queue != NULL) {
            xQueueSend(s_usb_evt_queue, &msg, 0);
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGE(TAG, "HID interface transfer error");
        break;
    default:
        break;
    }
}

/* ---------------- HID Host Device Event Handler ---------------- */
static void hid_host_device_event_callback(hid_host_device_handle_t hid_device_handle,
                                          const hid_host_driver_event_t event,
                                          void *arg) {
    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED: {
        usb_host_event_msg_t msg = {
            .type = USB_EVT_DEVICE_CONNECTED,
            .handle = hid_device_handle
        };
        if (s_usb_evt_queue != NULL) {
            xQueueSend(s_usb_evt_queue, &msg, 0);
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------- Device Lifecycle Worker Task ---------------- */
static void handle_device_connected(hid_host_device_handle_t hid_device_handle) {
    hid_host_dev_params_t dev_params;
    esp_err_t ret = hid_host_device_get_params(hid_device_handle, &dev_params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get device params: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "HID interface detected: addr=%d, iface=%d, proto=%d, subclass=%d",
             dev_params.addr, dev_params.iface_num, dev_params.proto, dev_params.sub_class);

    // Filter: only bind to keyboard interfaces (or boot protocol interfaces)
    if (dev_params.proto != HID_PROTOCOL_KEYBOARD && dev_params.sub_class != HID_SUBCLASS_BOOT_INTERFACE) {
        ESP_LOGI(TAG, "Skipping non-keyboard interface (proto: %d, subclass: %d)",
                 dev_params.proto, dev_params.sub_class);
        return;
    }

    if (s_hid_dev_handle != NULL) {
        ESP_LOGW(TAG, "A keyboard interface is already registered, ignoring secondary interface");
        return;
    }

    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback,
        .callback_arg = NULL
    };

    ret = hid_host_device_open(hid_device_handle, &dev_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HID device: %s", esp_err_to_name(ret));
        return;
    }

    // Now that this runs in a dedicated task (while hid_host driver task handles events),
    // control requests complete asynchronously without deadlock!
    ret = hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Set protocol request failed (%s), continuing with default format", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Keyboard switched to Boot Protocol successfully");
    }

    ret = hid_class_request_set_idle(hid_device_handle, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Set idle request failed (%s)", esp_err_to_name(ret));
    }

    ret = hid_host_device_start(hid_device_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HID interface: %s", esp_err_to_name(ret));
        hid_host_device_close(hid_device_handle);
        return;
    }

    s_hid_dev_handle = hid_device_handle;
    s_is_device_connected = true;
    ESP_LOGI(TAG, "USB Keyboard opened and active");
}

static void handle_device_disconnected(hid_host_device_handle_t hid_device_handle) {
    if (s_hid_dev_handle == hid_device_handle) {
        s_is_device_connected = false;
        s_hid_dev_handle = NULL;
        ESP_LOGI(TAG, "USB Keyboard disconnected");
    }
    hid_host_device_close(hid_device_handle);
}

static void usb_hid_device_task(void *pvParameters) {
    usb_host_event_msg_t msg;
    ESP_LOGI(TAG, "USB HID Device Lifecycle Task running on Core %d", xPortGetCoreID());

    while (1) {
        if (xQueueReceive(s_usb_evt_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.type == USB_EVT_DEVICE_CONNECTED) {
                handle_device_connected(msg.handle);
            } else if (msg.type == USB_EVT_DEVICE_DISCONNECTED) {
                handle_device_disconnected(msg.handle);
            }
        }
    }
}

/* ---------------- Public API Initialization ---------------- */
esp_err_t usb_hid_host_init(QueueHandle_t keyboard_event_queue) {
    if (keyboard_event_queue == NULL) {
        ESP_LOGE(TAG, "Queue handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    s_kb_queue = keyboard_event_queue;

    s_usb_evt_queue = xQueueCreate(8, sizeof(usb_host_event_msg_t));
    if (s_usb_evt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create USB event queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        usb_hid_device_task,
        "usb_hid_worker",
        USB_TASK_STACK_SIZE,
        NULL,
        USB_DEVICE_TASK_PRIORITY,
        NULL,
        0
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB device worker task");
        return ESP_FAIL;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB Host library: %s", esp_err_to_name(err));
        return err;
    }

    task_ret = xTaskCreatePinnedToCore(
        usb_host_lib_daemon_task,
        "usb_host_daemon",
        USB_TASK_STACK_SIZE,
        NULL,
        USB_HOST_TASK_PRIORITY,
        NULL,
        0
    );
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB daemon task");
        return ESP_FAIL;
    }

    // Use driver background task to handle low-level USB events
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = USB_HID_TASK_PRIORITY,
        .stack_size = USB_TASK_STACK_SIZE,
        .core_id = 0,
        .callback = hid_host_device_event_callback,
        .callback_arg = NULL
    };
    err = hid_host_install(&hid_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install HID Host driver: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB Host HID Subsystem initialized successfully on Core 0");
    return ESP_OK;
}

bool usb_hid_host_is_device_connected(void) {
    return s_is_device_connected;
}
