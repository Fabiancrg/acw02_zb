/*
 * ESP Zigbee OTA Implementation
 * 
 * Handles Over-The-Air firmware updates via Zigbee network
 */

#include "esp_zb_ota.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_zigbee_core.h"

static const char *TAG = "ESP_ZB_OTA";

/* OTA upgrade status */
static esp_zb_zcl_ota_upgrade_status_t ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_NORMAL;

/* OTA partition handle */
static const esp_partition_t *update_partition = NULL;
static esp_ota_handle_t update_handle = 0;

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
 * @brief OTA upgrade callback handler
 */
static esp_err_t zb_ota_upgrade_status_handler(esp_zb_zcl_ota_upgrade_status_message_t message)
{
    esp_err_t ret = ESP_OK;
    
    switch (message.status) {
        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
            ESP_LOGI(TAG, "OTA upgrade started");
            ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START;
            
            // Begin OTA update
            ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }
            ESP_LOGI(TAG, "OTA write started");
            break;
            
        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
            ESP_LOGD(TAG, "OTA receiving data, offset: %ld, size: %d", 
                     message.ota_header.image_size, message.payload_size);
            
            // Write received data to OTA partition
            ret = esp_ota_write(update_handle, message.payload, message.payload_size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }
            break;
            
        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
            ESP_LOGI(TAG, "OTA upgrade apply");
            
            // Finish OTA write
            ret = esp_ota_end(update_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }
            
            // Set boot partition
            ret = esp_ota_set_boot_partition(update_partition);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }
            
            ESP_LOGI(TAG, "OTA upgrade successful, will reboot...");
            ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY;
            break;
            
        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
            ESP_LOGI(TAG, "OTA upgrade check");
            // Check if new firmware is available
            ret = ESP_OK;
            break;
            
        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
            ESP_LOGE(TAG, "OTA upgrade error");
            ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
            
            // Abort OTA if it was started
            if (update_handle) {
                esp_ota_abort(update_handle);
                update_handle = 0;
            }
            ret = ESP_FAIL;
            break;
            
        default:
            ESP_LOGW(TAG, "Unknown OTA status: %d", message.status);
            break;
    }
    
    return ret;
}

/**
 * @brief Register OTA callbacks
 */
esp_err_t esp_zb_ota_register_callbacks(void)
{
    ESP_LOGI(TAG, "Registering OTA callbacks");
    
    // Register OTA upgrade status handler
    esp_zb_core_action_handler_register(ESP_ZB_CORE_OTA_UPGRADE_STATUS_CB_ID,
                                        (esp_zb_core_action_callback_t)zb_ota_upgrade_status_handler);
    
    return ESP_OK;
}

/**
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
    
    // Create version from app version string (e.g., "1.0.0" -> 0x01000000)
    // Format: major.minor.patch -> 0xMMmmpppp
    uint32_t version = 0;
    
    if (app_desc && app_desc->version) {
        int major = 0, minor = 0, patch = 0;
        sscanf(app_desc->version, "%d.%d.%d", &major, &minor, &patch);
        version = ((major & 0xFF) << 24) | ((minor & 0xFF) << 16) | (patch & 0xFFFF);
        ESP_LOGI(TAG, "Firmware version: %s (0x%08lX)", app_desc->version, version);
    }
    
    return version;
}
