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
static esp_ota_handle_t update_handle = 0;
static uint32_t binary_file_len = 0;
static uint32_t total_received = 0;

/**
 * @brief Initialize OTA functionality
 */
esp_err_t esp_zb_ota_init(void)
{
    ESP_LOGI(TAG, "Initializing Zigbee OTA");
    
    // Get the currently running partition
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition != NULL) {
        ESP_LOGI(TAG, "Currently running from partition: %s at 0x%lx", 
                 running_partition->label, 
                 running_partition->address);
    }
    
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
esp_err_t zb_ota_upgrade_value_handler(esp_zb_zcl_ota_upgrade_value_message_t message)
{
    esp_err_t ret = ESP_OK;

    switch (message.upgrade_status) {
        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
            ESP_LOGI(TAG, "OTA upgrade started");
            ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START;
            total_received = 0;
            binary_file_len = 0;

            // Begin OTA update
            ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }
            ESP_LOGI(TAG, "OTA write session started");
            break;

        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
            // Handle the first chunk specially to detect and skip OTA header
            if (total_received == 0) {
                // Debug: dump first few bytes to understand the data format
                ESP_LOGI(TAG, "First chunk received: %d bytes", message.payload_size);
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, message.payload, 
                                        message.payload_size > 128 ? 128 : message.payload_size, 
                                        ESP_LOG_INFO);
                
                // Search for the ESP32 magic byte (0xE9) in the first chunk
                int magic_offset = -1;
                for (int i = 0; i < message.payload_size && i < 256; i++) {
                    if (message.payload[i] == 0xE9) {
                        magic_offset = i;
                        ESP_LOGI(TAG, "Found ESP32 magic byte (0xE9) at offset %d", magic_offset);
                        break;
                    }
                }
                
                if (magic_offset >= 0) {
                    // Found the ESP32 binary, skip everything before it
                    ESP_LOGI(TAG, "Skipping %d bytes before ESP32 binary", magic_offset);
                    ESP_LOGI(TAG, "Writing %d bytes from first chunk", 
                             message.payload_size - magic_offset);
                    
                    ret = esp_ota_write(update_handle, message.payload + magic_offset, 
                                      message.payload_size - magic_offset);
                    total_received += message.payload_size - magic_offset;
                } else {
                    ESP_LOGE(TAG, "No ESP32 magic byte (0xE9) found in first %d bytes", 
                             message.payload_size);
                    ESP_LOGE(TAG, "Cannot proceed with OTA update");
                    ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                    return ESP_ERR_INVALID_ARG;
                }
            } else {
                // Subsequent chunks - write directly
                ESP_LOGD(TAG, "OTA receiving chunk: %d bytes (total: %ld bytes)",
                         message.payload_size, total_received);
                
                ret = esp_ota_write(update_handle, message.payload, message.payload_size);
                total_received += message.payload_size;
            }
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }
            
            // Log progress every ~50KB
            static uint32_t last_log = 0;
            if (total_received - last_log > 50000) {
                ESP_LOGI(TAG, "OTA progress: %ld bytes written", total_received);
                last_log = total_received;
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

            // Verify the written image
            ESP_LOGI(TAG, "Verifying OTA image...");
            esp_app_desc_t new_app_info;
            ret = esp_ota_get_partition_description(update_partition, &new_app_info);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get new app description: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }
            
            ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);
            ESP_LOGI(TAG, "New firmware compile time: %s %s", new_app_info.date, new_app_info.time);

            // CRITICAL: Set boot partition to the newly written partition
            ret = esp_ota_set_boot_partition(update_partition);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
                ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR;
                return ret;
            }

            ESP_LOGI(TAG, "OTA upgrade successful!");
            ESP_LOGI(TAG, "Next boot will be from: %s", update_partition->label);
            ESP_LOGI(TAG, "Firmware will enter validation mode on next boot");
            ESP_LOGI(TAG, "Rebooting in 3 seconds...");
            
            ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY;
            
            // Small delay to ensure logs are printed
            vTaskDelay(pdMS_TO_TICKS(3000));
            
            // Reboot to apply the update
            // After reboot, the new firmware will be in ESP_OTA_IMG_PENDING_VERIFY state
            // and must call esp_ota_mark_app_valid_cancel_rollback() to confirm it works
            esp_restart();
            break;

        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK:
            ESP_LOGI(TAG, "OTA upgrade check");
            ret = ESP_OK;
            break;

        case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
            ESP_LOGI(TAG, "OTA upgrade finished successfully");
            ota_upgrade_status = ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH;
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
            ESP_LOGW(TAG, "Unknown OTA status: %d", message.upgrade_status);
            break;
    }

    return ret;
}

/**
 * @brief OTA query image response handler
 */
esp_err_t zb_ota_query_image_resp_handler(esp_zb_zcl_ota_upgrade_query_image_resp_message_t message)
{
    esp_err_t ret = ESP_OK;

    if (message.info.status == ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "OTA image available: version 0x%lx, size %ld bytes",
                 message.file_version, message.image_size);
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "No OTA image available or query failed");
        ret = ESP_FAIL;
    }

    return ret;
}

/**
 * @brief Register OTA callbacks for client device
 *
 * Note: OTA callbacks are registered in the main action handler (esp_zb_hvac.c)
 * This function is kept for compatibility but callbacks are handled automatically.
 */
esp_err_t esp_zb_ota_register_callbacks(void)
{
    ESP_LOGI(TAG, "OTA callbacks integrated in main action handler");
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