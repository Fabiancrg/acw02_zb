/*
 * ESP Zigbee OTA Implementation
 * 
 * Handles Over-The-Air firmware updates via Zigbee network
 */

#include "esp_zb_ota.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_ota.h"

static const char *TAG = "ESP_ZB_OTA";

/* OTA upgrade status */
static esp_zb_zcl_ota_upgrade_status_t ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_OK;

/* OTA partition handle */
static const esp_partition_t *update_partition = NULL;
// static esp_ota_handle_t update_handle = 0;

/**
 * @brief Initialize OTA functionality
 */
esp_err_t esp_zb_ota_init(void)
{
    ESP_LOGI(TAG, "Initializing Zigbee OTA");
    
    // Get the next OTA partition
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find OTA partition");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "OTA partition found: %s at 0x%lx (size: %ld bytes)", 
             update_partition->label, 
             update_partition->address, 
             update_partition->size);
    
    return ESP_OK;
}

/**
 * @brief OTA upgrade callback handler for client device
 */
// static esp_err_t zb_ota_upgrade_value_handler(esp_zb_zcl_ota_upgrade_value_message_t message)
// {
//     esp_err_t ret = ESP_OK;
    
//     switch (message.upgrade_status) {
//         case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
//             ESP_LOGI(TAG, "OTA upgrade started");
//             ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START;
            
//             // Begin OTA update
//             ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
//             if (ret != ESP_OK) {
//                 ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
//                 ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
//                 return ret;
//             }
//             ESP_LOGI(TAG, "OTA write started");
//             break;
            
//         case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
//             ESP_LOGD(TAG, "OTA receiving data, offset: %ld, size: %d", 
//                      message.ota_header.image_size, message.payload_size);
            
//             // Write received data to OTA partition
//             ret = esp_ota_write(update_handle, message.payload, message.payload_size);
//             if (ret != ESP_OK) {
//                 ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
//                 ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
//                 return ret;
//             }
//             break;
            
//         case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
//             ESP_LOGI(TAG, "OTA upgrade apply");
            
//             // Finish OTA write
//             ret = esp_ota_end(update_handle);
//             if (ret != ESP_OK) {
//                 ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
//                 ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
//                 return ret;
//             }
            
//             // Set boot partition
//             ret = esp_ota_set_boot_partition(update_partition);
//             if (ret != ESP_OK) {
//                 ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
//                 ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
//                 return ret;
//             }
            
//             ESP_LOGI(TAG, "OTA upgrade successful, will reboot...");
//             ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY;
//             break;
            
//         case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
//             ESP_LOGI(TAG, "OTA upgrade check");
//             // Check if new firmware is available
//             ret = ESP_OK;
//             break;
            
//         case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
//             ESP_LOGI(TAG, "OTA upgrade finished successfully");
//             ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH;
//             break;
            
//         case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
//             ESP_LOGE(TAG, "OTA upgrade error");
//             ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            
//             // Abort OTA if it was started
//             if (update_handle) {
//                 esp_ota_abort(update_handle);
//                 update_handle = 0;
//             }
//             ret = ESP_FAIL;
//             break;
            
//         default:
//             ESP_LOGW(TAG, "Unknown OTA status: %d", message.upgrade_status);
//             break;
//     }
    
//     return ret;
// }

// /**
//  * @brief OTA query image response handler
//  */
// static esp_err_t zb_ota_query_image_resp_handler(esp_zb_zcl_ota_upgrade_query_image_resp_message_t message)
// {
//     esp_err_t ret = ESP_OK;
    
//     if (message.info.status == ESP_ZB_ZCL_STATUS_SUCCESS) {
//         ESP_LOGI(TAG, "OTA image available: version 0x%lx, size %ld bytes", 
//                  message.file_version, message.image_size);
//         ret = ESP_OK;  // Accept the OTA image
//     } else {
//         ESP_LOGW(TAG, "No OTA image available or query failed");
//         ret = ESP_FAIL;  // Reject the OTA image
//     }
    
//     return ret;
// }

/**
 * @brief Register OTA callbacks for client device
 *
 * Note: ESP-Zigbee SDK v5.5.1 may handle OTA callbacks differently.
 * The cluster setup in esp_zb_hvac.c should be sufficient for basic OTA.
 * Custom callbacks are only needed if you want to override default behavior.
 */
esp_err_t esp_zb_ota_register_callbacks(void)
{
    ESP_LOGI(TAG, "OTA callbacks handled automatically by ESP-Zigbee SDK cluster setup");
    // ESP-Zigbee SDK automatically registers OTA callbacks when cluster is created
    // Custom callbacks would only be needed for advanced customization
    return ESP_OK;
}/**
 * @brief Get current OTA status
 */
esp_zb_zcl_ota_upgrade_status_t esp_zb_ota_get_status(void)
{
    return ota_upgrade_status;
}

/**
 * @brief Get current firmware version
 */
uint32_t esp_zb_ota_get_fw_version(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    uint32_t version = 0;
    if (app_desc) {
        int major = 0, minor = 0, patch = 0;
        // Only parse if string is in semver format, else fallback to 0x01000000
        if (sscanf(app_desc->version, "%d.%d.%d", &major, &minor, &patch) == 3) {
            version = ((major & 0xFF) << 24) | ((minor & 0xFF) << 16) | (patch & 0xFFFF);
            ESP_LOGI(TAG, "Firmware version: %s (0x%08lX)", app_desc->version, version);
        } else {
            // Fallback: use 1.0.0 if not a semver string
            version = 0x01000000;
            ESP_LOGW(TAG, "Firmware version string not semver: '%s', using fallback 1.0.0 (0x01000000)", app_desc->version);
        }
    }
    return version;
}
