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
//static esp_ota_handle_t update_handle = 0;

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
 * @brief Register OTA callbacks for client device
 *
 * Note: ESP-Zigbee SDK v5.5.1 handles OTA callbacks automatically when cluster is created.
 * OTA logging is enabled via esp_zb_set_trace_level_mask() in the main application.
 */
esp_err_t esp_zb_ota_register_callbacks(void)
{
    ESP_LOGI(TAG, "OTA callbacks handled automatically by ESP-Zigbee SDK cluster setup");
    // OTA tracing is enabled in esp_zb_hvac.c for detailed logging during upgrades
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
