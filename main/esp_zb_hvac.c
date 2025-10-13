/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * Zigbee HVAC Thermostat
 *
 * This code controls an HVAC device via UART and exposes it as a Zigbee thermostat
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_hvac.h"
#include "hvac_driver.h"

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile End Device source code.
#endif

static const char *TAG = "HVAC_ZIGBEE";

/* HVAC state update interval */
#define HVAC_UPDATE_INTERVAL_MS     30000  // 30 seconds

/* Boot button configuration for factory reset */
#define BOOT_BUTTON_GPIO            GPIO_NUM_9
#define BUTTON_LONG_PRESS_TIME_MS   5000

/********************* Function Declarations **************************/
static esp_err_t deferred_driver_init(void);
static void hvac_update_zigbee_attributes(uint8_t param);
static void hvac_periodic_update(uint8_t param);
static void button_init(void);
static void button_task(void *arg);
static void factory_reset_device(uint8_t param);

/* Factory reset function */
static void factory_reset_device(uint8_t param)
{
    ESP_LOGW(TAG, "[RESET] Performing factory reset...");
    
    /* Perform factory reset - this will clear all Zigbee network settings */
    esp_zb_factory_reset();
    
    ESP_LOGI(TAG, "[RESET] Factory reset successful - device will restart");
    
    /* Restart the device after a short delay */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* Button monitoring task */
static void button_task(void *arg)
{
    uint32_t press_start_time = 0;
    bool button_pressed = false;
    bool long_press_triggered = false;
    
    ESP_LOGI(TAG, "[BUTTON] Monitoring started on GPIO%d", BOOT_BUTTON_GPIO);
    ESP_LOGI(TAG, "[BUTTON] Press and hold for %d seconds to factory reset", BUTTON_LONG_PRESS_TIME_MS / 1000);
    
    while (1) {
        int button_level = gpio_get_level(BOOT_BUTTON_GPIO);
        
        if (button_level == 0) {  // Button pressed (active low)
            if (!button_pressed) {
                // Button just pressed
                button_pressed = true;
                long_press_triggered = false;
                press_start_time = xTaskGetTickCount();
                ESP_LOGI(TAG, "[BUTTON] Boot button pressed");
            } else {
                // Button still pressed - check for long press
                uint32_t press_duration = pdTICKS_TO_MS(xTaskGetTickCount() - press_start_time);
                
                if (press_duration >= BUTTON_LONG_PRESS_TIME_MS && !long_press_triggered) {
                    long_press_triggered = true;
                    ESP_LOGW(TAG, "[BUTTON] Long press detected! Triggering factory reset...");
                    
                    // Schedule factory reset in Zigbee context
                    esp_zb_scheduler_alarm((esp_zb_callback_t)factory_reset_device, 0, 100);
                }
            }
        } else {
            // Button released
            if (button_pressed) {
                uint32_t press_duration = pdTICKS_TO_MS(xTaskGetTickCount() - press_start_time);
                
                if (!long_press_triggered) {
                    ESP_LOGI(TAG, "[BUTTON] Released (held for %lu ms)", press_duration);
                }
                
                button_pressed = false;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));  // Check every 50ms
    }
}

/* Initialize boot button */
static void button_init(void)
{
    ESP_LOGI(TAG, "[INIT] Configuring GPIO%d for button...", BOOT_BUTTON_GPIO);
    
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Failed to configure boot button GPIO: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[OK] GPIO configured");
    
    // Create button monitoring task
    ESP_LOGI(TAG, "[INIT] Creating button monitoring task...");
    BaseType_t task_ret = xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "[ERROR] Failed to create button task");
        return;
    }
    ESP_LOGI(TAG, "[OK] Button task created");
    
    ESP_LOGI(TAG, "[OK] Boot button fully initialized on GPIO%d", BOOT_BUTTON_GPIO);
}

static esp_err_t deferred_driver_init(void)
{
    ESP_LOGI(TAG, "[INIT] Starting deferred driver initialization...");
    
    /* Initialize boot button for factory reset */
    ESP_LOGI(TAG, "[INIT] Initializing boot button...");
    button_init();
    ESP_LOGI(TAG, "[INIT] Boot button initialization complete");
    
    /* Initialize HVAC UART driver */
    ESP_LOGI(TAG, "[INIT] Initializing HVAC UART driver...");
    esp_err_t ret = ESP_OK;
    
    // Wrap HVAC init in try-catch equivalent to prevent crashes
    ret = hvac_driver_init();
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Failed to initialize HVAC driver: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "[WARN] Continuing without HVAC - endpoints will still be created");
        // Don't fail - we can still expose Zigbee endpoints without HVAC connected
    } else {
        ESP_LOGI(TAG, "[OK] HVAC driver initialized successfully");
    }
    
    ESP_LOGI(TAG, "[INIT] Deferred initialization complete");
    return ESP_OK;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , 
                       TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "[JOIN] Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
        
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        ESP_LOGI(TAG, "[JOIN] Device first start - factory new device");
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "[JOIN] Calling deferred driver initialization...");
            esp_err_t init_ret = deferred_driver_init();
            ESP_LOGI(TAG, "[JOIN] Deferred driver initialization %s (ret=%d)", 
                    init_ret == ESP_OK ? "successful" : "failed", init_ret);
            ESP_LOGI(TAG, "[JOIN] Starting network steering (searching for coordinator)...");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            ESP_LOGI(TAG, "[JOIN] Network steering initiated");
        } else {
            ESP_LOGW(TAG, "[JOIN] Failed to initialize Zigbee stack (status: %s)", 
                    esp_err_to_name(err_status));
        }
        break;
        
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        ESP_LOGI(TAG, "[JOIN] Device reboot - previously joined network");
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "[JOIN] Calling deferred driver initialization...");
            esp_err_t init_ret = deferred_driver_init();
            ESP_LOGI(TAG, "[JOIN] Deferred driver initialization %s (ret=%d)", 
                    init_ret == ESP_OK ? "successful" : "failed", init_ret);
            
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "[JOIN] Factory new - starting network steering...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "[JOIN] Rejoining previous network...");
                esp_zb_ieee_addr_t ieee_addr;
                esp_zb_get_long_address(ieee_addr);
                ESP_LOGI(TAG, "[JOIN] IEEE Address: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                         ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
                         ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);
            }
        } else {
            ESP_LOGW(TAG, "[JOIN] Failed to initialize Zigbee stack (status: %s)", 
                    esp_err_to_name(err_status));
        }
        break;
        
    case ESP_ZB_BDB_SIGNAL_STEERING:
        ESP_LOGI(TAG, "[JOIN] Steering signal received (status: %s)", esp_err_to_name(err_status));
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "[JOIN] *** SUCCESSFULLY JOINED NETWORK ***");
            ESP_LOGI(TAG, "[JOIN] Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0]);
            ESP_LOGI(TAG, "[JOIN] PAN ID: 0x%04hx", esp_zb_get_pan_id());
            ESP_LOGI(TAG, "[JOIN] Channel: %d", esp_zb_get_current_channel());
            ESP_LOGI(TAG, "[JOIN] Short Address: 0x%04hx", esp_zb_get_short_address());
            
            esp_zb_ieee_addr_t ieee_addr;
            esp_zb_get_long_address(ieee_addr);
            ESP_LOGI(TAG, "[JOIN] IEEE Address: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     ieee_addr[7], ieee_addr[6], ieee_addr[5], ieee_addr[4],
                     ieee_addr[3], ieee_addr[2], ieee_addr[1], ieee_addr[0]);
            
            ESP_LOGI(TAG, "[JOIN] Device is now online and ready");
            ESP_LOGI(TAG, "[JOIN] Scheduling periodic HVAC updates...");
            
            /* Start periodic HVAC status updates */
            esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_periodic_update, 0, 5000);
            ESP_LOGI(TAG, "[JOIN] Setup complete!");
        } else {
            ESP_LOGW(TAG, "[JOIN] Network steering failed (status: %s)", 
                    esp_err_to_name(err_status));
            ESP_LOGI(TAG, "[JOIN] Retrying network steering in 1 second...");
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, 
                                  ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
        
    default:
        ESP_LOGI(TAG, "[ZDO] Signal: %s (0x%x), status: %s", 
                esp_zb_zdo_signal_to_string(sig_type), sig_type,
                esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    
    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, 
                       TAG, "Received message: error status(%d)", message->info.status);
    
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", 
             message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    
    if (message->info.dst_endpoint == HA_ESP_HVAC_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT) {
            switch (message->attribute.id) {
            case ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID:
            case ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID:
                if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_S16) {
                    int16_t temp_setpoint = *(int16_t *)message->attribute.data.value;
                    // Temperature is in centidegrees (e.g., 2400 = 24.00°C)
                    uint8_t temp_c = (uint8_t)(temp_setpoint / 100);
                    ESP_LOGI(TAG, "Temperature setpoint changed to %d°C", temp_c);
                    hvac_set_temperature(temp_c);
                    
                    // Schedule update to sync back to Zigbee
                    esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
                }
                break;
                
            case ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID:
                if (message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM) {
                    uint8_t system_mode = *(uint8_t *)message->attribute.data.value;
                    ESP_LOGI(TAG, "System mode changed to %d", system_mode);
                    
                    // Map Zigbee system mode to HVAC mode
                    // 0x00 = Off, 0x01 = Auto, 0x03 = Cool, 0x04 = Heat, 0x05 = Emergency Heat, 
                    // 0x06 = Precooling, 0x07 = Fan only, 0x08 = Dry, 0x09 = Sleep
                    hvac_mode_t hvac_mode = HVAC_MODE_OFF;
                    switch (system_mode) {
                        case 0x00: hvac_mode = HVAC_MODE_OFF; hvac_set_power(false); break;
                        case 0x01: hvac_mode = HVAC_MODE_AUTO; hvac_set_mode(hvac_mode); break;
                        case 0x03: hvac_mode = HVAC_MODE_COOL; hvac_set_mode(hvac_mode); break;
                        case 0x04: hvac_mode = HVAC_MODE_HEAT; hvac_set_mode(hvac_mode); break;
                        case 0x07: hvac_mode = HVAC_MODE_FAN; hvac_set_mode(hvac_mode); break;
                        case 0x08: hvac_mode = HVAC_MODE_DRY; hvac_set_mode(hvac_mode); break;
                        default:
                            ESP_LOGW(TAG, "Unsupported system mode: %d", system_mode);
                            break;
                    }
                    
                    esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
                }
                break;
                
            default:
                ESP_LOGD(TAG, "Unhandled thermostat attribute: 0x%x", message->attribute.id);
                break;
            }
        } else if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID) {
                uint8_t fan_mode = *(uint8_t *)message->attribute.data.value;
                ESP_LOGI(TAG, "Fan mode changed to %d", fan_mode);
                
                // Map Zigbee fan mode to HVAC fan speed
                hvac_fan_t hvac_fan = HVAC_FAN_AUTO;
                switch (fan_mode) {
                    case 0: hvac_fan = HVAC_FAN_AUTO; break;      // Off
                    case 1: hvac_fan = HVAC_FAN_P20; break;       // Low
                    case 2: hvac_fan = HVAC_FAN_P40; break;       // Medium
                    case 3: hvac_fan = HVAC_FAN_P60; break;       // High
                    case 4: hvac_fan = HVAC_FAN_AUTO; break;      // On/Auto
                    case 5: hvac_fan = HVAC_FAN_P100; break;      // Smart/Max
                    default: hvac_fan = HVAC_FAN_AUTO; break;
                }
                
                hvac_set_fan_speed(hvac_fan);
                esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
            }
        }
    }
    /* Handle Eco Mode Switch - Endpoint 2 */
    else if (message->info.dst_endpoint == HA_ESP_ECO_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool on_off = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "[ECO] Mode %s", on_off ? "ON" : "OFF");
                hvac_set_eco_mode(on_off);
                esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
            }
        }
    }
    /* Handle Swing Switch - Endpoint 3 */
    else if (message->info.dst_endpoint == HA_ESP_SWING_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool on_off = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "[SWING] Mode %s", on_off ? "ON" : "OFF");
                hvac_set_swing(on_off);
                esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
            }
        }
    }
    /* Handle Display Switch - Endpoint 4 */
    else if (message->info.dst_endpoint == HA_ESP_DISPLAY_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool on_off = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "[DISPLAY] %s", on_off ? "ON" : "OFF");
                hvac_set_display(on_off);
                esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
            }
        }
    }
    
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
        
    case ESP_ZB_CORE_REPORT_ATTR_CB_ID:
        ESP_LOGD(TAG, "Report attribute callback");
        break;
        
    default:
        ESP_LOGD(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    
    return ret;
}

static void hvac_update_zigbee_attributes(uint8_t param)
{
    hvac_state_t state;
    esp_err_t ret = hvac_get_state(&state);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get HVAC state");
        return;
    }
    
    /* Update system mode */
    uint8_t system_mode = 0x00;  // Off
    if (state.power_on) {
        switch (state.mode) {
            case HVAC_MODE_AUTO: system_mode = 0x01; break;
            case HVAC_MODE_COOL: system_mode = 0x03; break;
            case HVAC_MODE_HEAT: system_mode = 0x04; break;
            case HVAC_MODE_FAN:  system_mode = 0x07; break;
            case HVAC_MODE_DRY:  system_mode = 0x08; break;
            default: system_mode = 0x00; break;
        }
    }
    
    esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, 
                                 ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID,
                                 &system_mode, false);
    
    /* Update temperature setpoint (in centidegrees) */
    int16_t temp_setpoint = state.target_temp_c * 100;
    esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID,
                                 &temp_setpoint, false);
    
    /* Update local temperature (ambient) */
    int16_t local_temp = state.ambient_temp_c * 100;
    esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID,
                                 &local_temp, false);
    
    /* Update Eco Mode switch state - Endpoint 2 */
    esp_zb_zcl_set_attribute_val(HA_ESP_ECO_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &state.eco_mode, false);
    
    /* Update Swing switch state - Endpoint 3 */
    esp_zb_zcl_set_attribute_val(HA_ESP_SWING_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &state.swing_on, false);
    
    /* Update Display switch state - Endpoint 4 */
    esp_zb_zcl_set_attribute_val(HA_ESP_DISPLAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &state.display_on, false);
    
    ESP_LOGI(TAG, "Updated Zigbee attributes: Mode=%d, Temp=%d°C, Eco=%d, Swing=%d, Display=%d", 
             system_mode, state.target_temp_c, state.eco_mode, state.swing_on, state.display_on);
}

static void hvac_periodic_update(uint8_t param)
{
    /* Request fresh status from HVAC unit */
    hvac_request_status();
    
    /* Update Zigbee attributes */
    hvac_update_zigbee_attributes(0);
    
    /* Send keepalive to HVAC */
    hvac_send_keepalive();
    
    /* Schedule next update */
    esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_periodic_update, 0, HVAC_UPDATE_INTERVAL_MS);
}

static void esp_zb_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[START] Starting Zigbee task...");
    
    /* Initialize Zigbee stack */
    ESP_LOGI(TAG, "[INIT] Initializing Zigbee stack as End Device...");
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    ESP_LOGI(TAG, "[OK] Zigbee stack initialized");
    
    /* Create endpoint list */
    ESP_LOGI(TAG, "[INIT] Creating endpoint list...");
    esp_zb_ep_list_t *esp_zb_ep_list = esp_zb_ep_list_create();
    
    /* Create thermostat cluster list */
    ESP_LOGI(TAG, "[HVAC] Creating HVAC thermostat clusters...");
    esp_zb_cluster_list_t *esp_zb_hvac_clusters = esp_zb_zcl_cluster_list_create();
    
    /* Create Basic cluster */
    ESP_LOGI(TAG, "  [+] Adding Basic cluster (0x0000)...");
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *esp_zb_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    
    /* Add optional Basic cluster attributes that Z2M expects */
    uint8_t app_version = 1;
    uint8_t stack_version = 2;
    uint8_t hw_version = 1;
    char date_code[] = "\x08""20251013";     // Length-prefixed: 8 chars = "20251013"
    char sw_build_id[] = "\x06""v1.0.0";     // Length-prefixed: 6 chars = "v1.0.0"
    
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app_version);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw_build_id);
    
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_hvac_clusters, esp_zb_basic_cluster, 
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  [OK] Basic cluster added with extended attributes");
    
    /* Create Thermostat cluster */
    ESP_LOGI(TAG, "  [+] Adding Thermostat cluster (0x0201)...");
    esp_zb_thermostat_cluster_cfg_t thermostat_cfg = {
        .local_temperature = 25 * 100,                    // 25°C in centidegrees
        .occupied_cooling_setpoint = 24 * 100,            // 24°C default cooling setpoint
        .occupied_heating_setpoint = 22 * 100,            // 22°C default heating setpoint
        .control_sequence_of_operation = 0x04,            // Cooling and heating
        .system_mode = 0x00,                              // Off
    };
    esp_zb_attribute_list_t *esp_zb_thermostat_cluster = esp_zb_thermostat_cluster_create(&thermostat_cfg);
    
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_thermostat_cluster(esp_zb_hvac_clusters, esp_zb_thermostat_cluster,
                                                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  [OK] Thermostat cluster added");
    
    /* Create Fan Control cluster */
    ESP_LOGI(TAG, "  [+] Adding Fan Control cluster (0x0202)...");
    esp_zb_fan_control_cluster_cfg_t fan_cfg = {
        .fan_mode = 0x00,  // Off/Auto
        .fan_mode_sequence = 0x02,  // Low/Med/High
    };
    esp_zb_attribute_list_t *esp_zb_fan_cluster = esp_zb_fan_control_cluster_create(&fan_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_fan_control_cluster(esp_zb_hvac_clusters, esp_zb_fan_cluster,
                                                                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  [OK] Fan Control cluster added");
    
    /* Add Identify cluster */
    ESP_LOGI(TAG, "  [+] Adding Identify cluster (0x0003)...");
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(esp_zb_hvac_clusters, 
                                                             esp_zb_identify_cluster_create(NULL),
                                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  [OK] Identify cluster added");
    
    /* Create HVAC endpoint */
    ESP_LOGI(TAG, "[EP] Creating HVAC endpoint %d (Profile: 0x%04X, Device: 0x%04X)...", 
             HA_ESP_HVAC_ENDPOINT, ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_THERMOSTAT_DEVICE_ID);
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HA_ESP_HVAC_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_THERMOSTAT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_hvac_clusters, endpoint_config);
    ESP_LOGI(TAG, "[OK] Endpoint %d added to endpoint list", HA_ESP_HVAC_ENDPOINT);
    
    /* Create Eco Mode Switch - Endpoint 2 */
    ESP_LOGI(TAG, "[ECO] Creating Eco Mode switch endpoint %d...", HA_ESP_ECO_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_eco_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_eco_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_eco_clusters, esp_zb_eco_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_on_off_cluster_cfg_t eco_on_off_cfg = {
        .on_off = false,
    };
    esp_zb_attribute_list_t *esp_zb_eco_on_off_cluster = esp_zb_on_off_cluster_create(&eco_on_off_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_eco_clusters, esp_zb_eco_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t eco_endpoint_config = {
        .endpoint = HA_ESP_ECO_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_eco_clusters, eco_endpoint_config);
    ESP_LOGI(TAG, "[OK] Eco Mode switch endpoint %d added", HA_ESP_ECO_ENDPOINT);
    
    /* Create Swing Switch - Endpoint 3 */
    ESP_LOGI(TAG, "[SWING] Creating Swing switch endpoint %d...", HA_ESP_SWING_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_swing_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_swing_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_swing_clusters, esp_zb_swing_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_on_off_cluster_cfg_t swing_on_off_cfg = {
        .on_off = false,
    };
    esp_zb_attribute_list_t *esp_zb_swing_on_off_cluster = esp_zb_on_off_cluster_create(&swing_on_off_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_swing_clusters, esp_zb_swing_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t swing_endpoint_config = {
        .endpoint = HA_ESP_SWING_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_swing_clusters, swing_endpoint_config);
    ESP_LOGI(TAG, "[OK] Swing switch endpoint %d added", HA_ESP_SWING_ENDPOINT);
    
    /* Create Display Switch - Endpoint 4 */
    ESP_LOGI(TAG, "[DISP] Creating Display switch endpoint %d...", HA_ESP_DISPLAY_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_display_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_display_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_display_clusters, esp_zb_display_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_on_off_cluster_cfg_t display_on_off_cfg = {
        .on_off = true,  // Display defaults to ON
    };
    esp_zb_attribute_list_t *esp_zb_display_on_off_cluster = esp_zb_on_off_cluster_create(&display_on_off_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_display_clusters, esp_zb_display_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t display_endpoint_config = {
        .endpoint = HA_ESP_DISPLAY_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_display_clusters, display_endpoint_config);
    ESP_LOGI(TAG, "[OK] Display switch endpoint %d added", HA_ESP_DISPLAY_ENDPOINT);
    
    /* Add manufacturer info */
    ESP_LOGI(TAG, "[INFO] Adding manufacturer info (Espressif, %s)...", CONFIG_IDF_TARGET);
    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_HVAC_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_ECO_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_SWING_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_DISPLAY_ENDPOINT, &info);
    ESP_LOGI(TAG, "[OK] Manufacturer info added to all endpoints");
    
    /* Register device */
    ESP_LOGI(TAG, "[REG] Registering Zigbee device...");
    esp_zb_device_register(esp_zb_ep_list);
    ESP_LOGI(TAG, "[OK] Device registered");
    
    ESP_LOGI(TAG, "[REG] Registering action handler...");
    esp_zb_core_action_handler_register(zb_action_handler);
    ESP_LOGI(TAG, "[OK] Action handler registered");
    
    ESP_LOGI(TAG, "[CFG] Setting Zigbee channel mask: 0x%08lX", (unsigned long)ESP_ZB_PRIMARY_CHANNEL_MASK);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    
    ESP_LOGI(TAG, "[START] Starting Zigbee stack...");
    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "[OK] Zigbee stack started successfully");
    
    /* Start main Zigbee stack loop */
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    
    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
