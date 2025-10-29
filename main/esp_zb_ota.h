/*
 * ESP Zigbee OTA Header
 * 
 * Handles Over-The-Air firmware updates via Zigbee network
 */

#pragma once

#include "esp_err.h"
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize OTA functionality
 * 
 * @return ESP_OK on success
 */
esp_err_t esp_zb_ota_init(void);

/**
 * @brief Register OTA callbacks
 * 
 * @return ESP_OK on success
 */
esp_err_t esp_zb_ota_register_callbacks(void);

/**
 * @brief Get current OTA upgrade status
 * 
 * @return Current OTA status
 */
esp_zb_zcl_ota_upgrade_status_t esp_zb_ota_get_status(void);

/**
 * @brief Get current firmware version
 * 
 * @return Firmware version in format 0xMMmmpppp (major.minor.patch)
 */
uint32_t esp_zb_ota_get_fw_version(void);

#ifdef __cplusplus
}
#endif
