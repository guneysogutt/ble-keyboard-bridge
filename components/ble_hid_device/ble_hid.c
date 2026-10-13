#include "ble_hid.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_HID";

// Forward declaration of NimBLE NVS store initializer
void ble_store_config_init(void);

// Standard Bluetooth SIG 16-bit UUIDs
#define UUID16_SVC_DEVICE_INFORMATION       0x180A
#define UUID16_CHR_MANUFACTURER_NAME        0x2A29
#define UUID16_CHR_MODEL_NUMBER             0x2A24
#define UUID16_CHR_PNP_ID                   0x2A50

#define UUID16_SVC_BATTERY_SERVICE          0x180F
#define UUID16_CHR_BATTERY_LEVEL            0x2A19

#define UUID16_SVC_HUMAN_INTERFACE_DEVICE   0x1812
#define UUID16_CHR_HID_INFORMATION          0x2A4A
#define UUID16_CHR_REPORT_MAP               0x2A4B
#define UUID16_CHR_HID_CONTROL_POINT        0x2A4C
#define UUID16_CHR_REPORT                   0x2A4D
#define UUID16_CHR_PROTOCOL_MODE            0x2A4E
#define UUID16_CHR_BOOT_KEYBOARD_INPUT      0x2A22
#define UUID16_DSC_REPORT_REFERENCE         0x2908

// Connection Handle & Report Value Handles
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_input_report_handle = 0;
static uint16_t s_boot_report_handle = 0;
static bool s_is_connected = false;
static bool s_notifications_enabled = false;
static char s_device_name[32] = "Wireless Mechanical KB";

// Standard USB HID Keyboard Report Descriptor (Boot protocol compatible)
static const uint8_t hid_report_map[] = {
    0x05, 0x01,  // USAGE_PAGE (Generic Desktop)
    0x09, 0x06,  // USAGE (Keyboard)
    0xA1, 0x01,  // COLLECTION (Application)
    0x05, 0x07,  //   USAGE_PAGE (Keyboard)
    0x19, 0xE0,  //   USAGE_MINIMUM (Keyboard LeftControl)
    0x29, 0xE7,  //   USAGE_MAXIMUM (Keyboard Right GUI)
    0x15, 0x00,  //   LOGICAL_MINIMUM (0)
    0x25, 0x01,  //   LOGICAL_MAXIMUM (1)
    0x75, 0x01,  //   REPORT_SIZE (1)
    0x95, 0x08,  //   REPORT_COUNT (8)
    0x81, 0x02,  //   INPUT (Data,Var,Abs) ; Modifiers
    0x95, 0x01,  //   REPORT_COUNT (1)
    0x75, 0x08,  //   REPORT_SIZE (8)
    0x81, 0x01,  //   INPUT (Cnst,Ary,Abs) ; Reserved byte
    0x95, 0x06,  //   REPORT_COUNT (6)
    0x75, 0x08,  //   REPORT_SIZE (8)
    0x15, 0x00,  //   LOGICAL_MINIMUM (0)
    0x25, 0x65,  //   LOGICAL_MAXIMUM (101)
    0x05, 0x07,  //   USAGE_PAGE (Keyboard)
    0x19, 0x00,  //   USAGE_MINIMUM (Reserved / no event)
    0x29, 0x65,  //   USAGE_MAXIMUM (Keyboard Application)
    0x81, 0x00,  //   INPUT (Data,Ary,Abs) ; Keycodes
    0xC0         // END_COLLECTION
};

// HID Information: [bcdHID (0x0111), bCountryCode (0), Flags (0x02: NormallyConnectable)]
static const uint8_t hid_info[] = {0x11, 0x01, 0x00, 0x02};
// Report Reference: [Report ID = 0, Report Type = 1 (Input)]
static const uint8_t hid_report_ref[] = {0x00, 0x01};
static uint8_t s_protocol_mode = 0x01; // Default: Report Protocol Mode

// Device Information: PnP ID is MANDATORY for Apple/Windows HOGP compliance
// Format: Vendor ID Source (0x02 = USB-IF), VID (0x02E5 = Espressif), PID (0xABCD), Version (0x0100)
static const uint8_t s_pnp_id[] = {0x02, 0xE5, 0x02, 0xCD, 0xAB, 0x00, 0x01};
static const char s_manufacturer_name[] = "Espressif";
static const char s_model_number[] = "ESP32S3-Bridge";

// Battery Level: 100%
static uint8_t s_battery_level = 100;

static void start_advertising(void);

/* ---------------- GATT Access Callback ---------------- */
static int gatt_svr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);

    switch (uuid16) {
    case UUID16_CHR_HID_INFORMATION:
        os_mbuf_append(ctxt->om, hid_info, sizeof(hid_info));
        return 0;

    case UUID16_CHR_REPORT_MAP:
        os_mbuf_append(ctxt->om, hid_report_map, sizeof(hid_report_map));
        return 0;

    case UUID16_CHR_PROTOCOL_MODE:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            os_mbuf_append(ctxt->om, &s_protocol_mode, sizeof(s_protocol_mode));
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            if (OS_MBUF_PKTLEN(ctxt->om) == 1) {
                s_protocol_mode = ctxt->om->om_data[0];
                ESP_LOGI(TAG, "Protocol mode changed to %d", s_protocol_mode);
            }
        }
        return 0;

    case UUID16_CHR_REPORT:
    case UUID16_CHR_BOOT_KEYBOARD_INPUT:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            uint8_t zero_report[8] = {0};
            os_mbuf_append(ctxt->om, zero_report, sizeof(zero_report));
        }
        return 0;

    case UUID16_CHR_HID_CONTROL_POINT:
        return 0;

    case UUID16_CHR_PNP_ID:
        os_mbuf_append(ctxt->om, s_pnp_id, sizeof(s_pnp_id));
        return 0;

    case UUID16_CHR_MANUFACTURER_NAME:
        os_mbuf_append(ctxt->om, s_manufacturer_name, strlen(s_manufacturer_name));
        return 0;

    case UUID16_CHR_MODEL_NUMBER:
        os_mbuf_append(ctxt->om, s_model_number, strlen(s_model_number));
        return 0;

    case UUID16_CHR_BATTERY_LEVEL:
        os_mbuf_append(ctxt->om, &s_battery_level, sizeof(s_battery_level));
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int gatt_svr_dsc_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    uint16_t uuid16 = ble_uuid_u16(ctxt->dsc->uuid);
    if (uuid16 == UUID16_DSC_REPORT_REFERENCE) {
        os_mbuf_append(ctxt->om, hid_report_ref, sizeof(hid_report_ref));
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ---------------- GATT Services Definition ---------------- */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    // 1. Device Information Service (Mandatory for HOGP compliance)
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID16_SVC_DEVICE_INFORMATION),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_PNP_ID),
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_access_cb,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_MANUFACTURER_NAME),
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_access_cb,
            },
            {
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_MODEL_NUMBER),
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_access_cb,
            },
            { 0 }
        },
    },

    // 2. Battery Service (Mandatory for HOGP compliance)
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID16_SVC_BATTERY_SERVICE),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_BATTERY_LEVEL),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .access_cb = gatt_svr_access_cb,
            },
            { 0 }
        },
    },

    // 3. Human Interface Device Service
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID16_SVC_HUMAN_INTERFACE_DEVICE),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // HID Information
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_HID_INFORMATION),
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // Report Map
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_REPORT_MAP),
                .flags = BLE_GATT_CHR_F_READ,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // Protocol Mode
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_PROTOCOL_MODE),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // HID Control Point
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_HID_CONTROL_POINT),
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
                .access_cb = gatt_svr_access_cb,
            },
            {
                // Input Report (Keypress streaming in Report Mode)
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_REPORT),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .access_cb = gatt_svr_access_cb,
                .val_handle = &s_input_report_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(UUID16_DSC_REPORT_REFERENCE),
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = gatt_svr_dsc_cb,
                    },
                    { 0 }
                }
            },
            {
                // Boot Keyboard Input Report (Boot Mode)
                .uuid = BLE_UUID16_DECLARE(UUID16_CHR_BOOT_KEYBOARD_INPUT),
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .access_cb = gatt_svr_access_cb,
                .val_handle = &s_boot_report_handle,
            },
            { 0 }
        },
    },
    { 0 }
};

/* ---------------- Low-Latency Connection Parameter Tuning ---------------- */
static void request_low_latency_params(uint16_t conn_handle) {
    // Apple / Android compliance: supervision_timeout must be at least 2000ms (200 * 10ms)
    struct ble_gap_upd_params params = {
        .itvl_min = 6,              // 6 * 1.25ms = 7.5ms (BLE minimum)
        .itvl_max = 12,             // 12 * 1.25ms = 15ms
        .latency = 0,               // 0 slave latency
        .supervision_timeout = 300, // 300 * 10ms = 3000ms (Apple requires >= 2.0s)
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    int rc = ble_gap_update_params(conn_handle, &params);
    ESP_LOGI(TAG, "Requested low-latency connection parameters (7.5ms - 15ms), status: %d", rc);
}

/* ---------------- GAP Event Handler ---------------- */
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_is_connected = true;
            ESP_LOGI(TAG, "Connected to Host (conn_handle: %d)", s_conn_handle);
            // Do NOT call request_low_latency_params here! Apple forbids parameter updates during initial handshake.
        } else {
            ESP_LOGW(TAG, "Connection failed; restarting advertising");
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected; reason=0x%04x; restarting advertising", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_is_connected = false;
        s_notifications_enabled = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection update event received, status: %d", event->conn_update.status);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Central wants to pair again - discard stale bond and re-pair */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};
        pkey.action = event->passkey.params.action;
        if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.numcmp_accept = 1;
        } else if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.passkey = 123456;
            ESP_LOGI(TAG, "Display passkey: 123456");
        }
        rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        ESP_LOGI(TAG, "ble_sm_inject_io action=%d, rc=%d", pkey.action, rc);
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe event: attr_handle=%d, cur_notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_input_report_handle ||
            event->subscribe.attr_handle == s_boot_report_handle) {
            s_notifications_enabled = event->subscribe.cur_notify;
            if (s_notifications_enabled) {
                // Now that pairing and GATT subscription are complete, request low-latency parameters safely!
                request_low_latency_params(s_conn_handle);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change event; status=%d", event->enc_change.status);
        return 0;

    default:
        return 0;
    }
}

/* ---------------- Advertising Routine ---------------- */
static void start_advertising(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields adv_fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    // 1. Primary Advertising Packet: Keep under 31 bytes!
    // Flags (3 bytes) + Appearance (4 bytes) + HID Service UUID (4 bytes) + Name (13 bytes) = 24 bytes <= 31 bytes!
    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Appearance: Keyboard (0x03C1)
    adv_fields.appearance = 0x03C1;
    adv_fields.appearance_is_present = 1;

    // Advertised Service: HID (0x1812)
    adv_fields.uuids16 = (ble_uuid16_t[]){ BLE_UUID16_INIT(UUID16_SVC_HUMAN_INTERFACE_DEVICE) };
    adv_fields.num_uuids16 = 1;
    adv_fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set advertising fields; rc=%d", rc);
        return;
    }

    // 2. Scan Response Packet: Carries the full device name
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t *)s_device_name;
    rsp_fields.name_len = strlen(s_device_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set scan response fields; rc=%d", rc);
        return;
    }

    // 3. Start Advertising
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 32; // 20ms
    adv_params.itvl_max = 64; // 40ms

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start advertising; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started as '%s'", s_device_name);
}

/* ---------------- NimBLE Host Task ---------------- */
static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started on Core %d", xPortGetCoreID());
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void) {
    ESP_LOGI(TAG, "NimBLE host synced; starting advertising");
    start_advertising();
}

/* ---------------- Public APIs ---------------- */
esp_err_t ble_hid_init(const char *device_name) {
    if (device_name) {
        strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
        s_device_name[sizeof(s_device_name) - 1] = '\0';
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nimble port: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) return ESP_FAIL;

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) return ESP_FAIL;

    ble_svc_gap_device_name_set(s_device_name);
    ble_svc_gap_device_appearance_set(0x03C1);

    // Initialize NVS storage for bonding keys
    ble_store_config_init();

    // Security Manager Configuration for HOGP Just Works pairing & bonding
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(ble_host_task);

    return ESP_OK;
}

esp_err_t ble_hid_send_report(const uint8_t *report, size_t length) {
    if (!s_is_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t report_handle = (s_protocol_mode == 0 && s_boot_report_handle != 0) ?
                             s_boot_report_handle : s_input_report_handle;

    if (report_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, length);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, report_handle, om);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

bool ble_hid_is_connected(void) {
    return s_is_connected && (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
}
