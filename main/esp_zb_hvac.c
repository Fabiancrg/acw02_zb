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
    ESP_LOGW(TAG, "🔄 Performing factory reset...");
    
    /* Perform factory reset - this will clear all Zigbee network settings */
    esp_zb_factory_reset();
    
    ESP_LOGI(TAG, "✅ Factory reset successful - device will restart");
    
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
    
    ESP_LOGI(TAG, "🔘 Button monitoring started on GPIO%d", BOOT_BUTTON_GPIO);
    ESP_LOGI(TAG, "📋 Press and hold for %d seconds to factory reset", BUTTON_LONG_PRESS_TIME_MS / 1000);
    
    while (1) {
        int button_level = gpio_get_level(BOOT_BUTTON_GPIO);
        
        if (button_level == 0) {  // Button pressed (active low)
            if (!button_pressed) {
                // Button just pressed
                button_pressed = true;
                long_press_triggered = false;
                press_start_time = xTaskGetTickCount();
                ESP_LOGI(TAG, "🔘 Boot button pressed");
            } else {
                // Button still pressed - check for long press
                uint32_t press_duration = pdTICKS_TO_MS(xTaskGetTickCount() - press_start_time);
                
                if (press_duration >= BUTTON_LONG_PRESS_TIME_MS && !long_press_triggered) {
                    long_press_triggered = true;
                    ESP_LOGW(TAG, "🔄 Long press detected! Triggering factory reset...");
                    
                    // Schedule factory reset in Zigbee context
                    esp_zb_scheduler_alarm((esp_zb_callback_t)factory_reset_device, 0, 100);
                }
            }
        } else {
            // Button released
            if (button_pressed) {
                uint32_t press_duration = pdTICKS_TO_MS(xTaskGetTickCount() - press_start_time);
                
                if (!long_press_triggered) {
                    ESP_LOGI(TAG, "🔘 Boot button released (held for %lu ms)", press_duration);
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
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure boot button GPIO");
        return;
    }
    
    // Create button monitoring task
    BaseType_t task_ret = xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return;
    }
    
    ESP_LOGI(TAG, "✅ Boot button initialized on GPIO%d", BOOT_BUTTON_GPIO);
}

static esp_err_t deferred_driver_init(void)
{
    ESP_LOGI(TAG, "🔧 Starting deferred driver initialization...");
    
    /* Initialize boot button for factory reset */
    button_init();
    
    /* Initialize HVAC UART driver */
    ESP_LOGI(TAG, "🔧 Initializing HVAC UART driver...");
    esp_err_t ret = hvac_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to initialize HVAC driver (UART connection issue?)");
        ESP_LOGW(TAG, "⚠️  Continuing without HVAC - endpoints will still be created");
        // Don't fail - we can still expose Zigbee endpoints without HVAC connected
    } else {
        ESP_LOGI(TAG, "✅ HVAC driver initialized successfully");
    }
    
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
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
        
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", 
                    deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", 
                    esp_zb_bdb_is_factory_new() ? "" : "non");
            
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", 
                    esp_err_to_name(err_status));
        }
        break;
        
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            
            /* Start periodic HVAC status updates */
            esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_periodic_update, 0, 5000);
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", 
                    esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, 
                                  ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
        
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", 
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
    
    ESP_LOGI(TAG, "Updated Zigbee attributes: Mode=%d, Temp=%d°C", 
             system_mode, state.target_temp_c);
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
    ESP_LOGI(TAG, "🚀 Starting Zigbee task...");
    
    /* Initialize Zigbee stack */
    ESP_LOGI(TAG, "📡 Initializing Zigbee stack as End Device...");
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    ESP_LOGI(TAG, "✅ Zigbee stack initialized");
    
    /* Create endpoint list */
    ESP_LOGI(TAG, "📋 Creating endpoint list...");
    esp_zb_ep_list_t *esp_zb_ep_list = esp_zb_ep_list_create();
    
    /* Create thermostat cluster list */
    ESP_LOGI(TAG, "🌡️  Creating HVAC thermostat clusters...");
    esp_zb_cluster_list_t *esp_zb_hvac_clusters = esp_zb_zcl_cluster_list_create();
    
    /* Create Basic cluster */
    ESP_LOGI(TAG, "  ➕ Adding Basic cluster (0x0000)...");
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
    ESP_LOGI(TAG, "  ✅ Basic cluster added with extended attributes");
    
    /* Create Thermostat cluster */
    ESP_LOGI(TAG, "  ➕ Adding Thermostat cluster (0x0201)...");
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
    ESP_LOGI(TAG, "  ✅ Thermostat cluster added");
    
    /* Create Fan Control cluster */
    ESP_LOGI(TAG, "  ➕ Adding Fan Control cluster (0x0202)...");
    esp_zb_fan_control_cluster_cfg_t fan_cfg = {
        .fan_mode = 0x00,  // Off/Auto
        .fan_mode_sequence = 0x02,  // Low/Med/High
    };
    esp_zb_attribute_list_t *esp_zb_fan_cluster = esp_zb_fan_control_cluster_create(&fan_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_fan_control_cluster(esp_zb_hvac_clusters, esp_zb_fan_cluster,
                                                                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  ✅ Fan Control cluster added");
    
    /* Add Identify cluster */
    ESP_LOGI(TAG, "  ➕ Adding Identify cluster (0x0003)...");
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(esp_zb_hvac_clusters, 
                                                             esp_zb_identify_cluster_create(NULL),
                                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  ✅ Identify cluster added");
    
    /* Create HVAC endpoint */
    ESP_LOGI(TAG, "🔌 Creating HVAC endpoint %d (Profile: 0x%04X, Device: 0x%04X)...", 
             HA_ESP_HVAC_ENDPOINT, ESP_ZB_AF_HA_PROFILE_ID, ESP_ZB_HA_THERMOSTAT_DEVICE_ID);
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = HA_ESP_HVAC_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_THERMOSTAT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_hvac_clusters, endpoint_config);
    ESP_LOGI(TAG, "✅ Endpoint %d added to endpoint list", HA_ESP_HVAC_ENDPOINT);
    
    /* Add manufacturer info */
    ESP_LOGI(TAG, "🏭 Adding manufacturer info (Espressif, %s)...", CONFIG_IDF_TARGET);
    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_HVAC_ENDPOINT, &info);
    ESP_LOGI(TAG, "✅ Manufacturer info added");
    
    /* Register device */
    ESP_LOGI(TAG, "📝 Registering Zigbee device...");
    esp_zb_device_register(esp_zb_ep_list);
    ESP_LOGI(TAG, "✅ Device registered");
    
    ESP_LOGI(TAG, "🔧 Registering action handler...");
    esp_zb_core_action_handler_register(zb_action_handler);
    ESP_LOGI(TAG, "✅ Action handler registered");
    
    ESP_LOGI(TAG, "📻 Setting Zigbee channel mask: 0x%08lX", (unsigned long)ESP_ZB_PRIMARY_CHANNEL_MASK);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    
    ESP_LOGI(TAG, "🚀 Starting Zigbee stack...");
    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "✅ Zigbee stack started successfully");
    
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
