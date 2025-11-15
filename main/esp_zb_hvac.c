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
#include "freertos/queue.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "board.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_hvac.h"
#include "hvac_driver.h"
#include "esp_zb_ota.h"
#include "esp_zigbee_trace.h"
#include "sdkconfig.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/timers.h"
#include <math.h>


#if !defined ZB_ROUTER_ROLE
#error Define ZB_ROUTER_ROLE in idf.py menuconfig to compile Router source code.
#endif

static const char *TAG = "HVAC_ZIGBEE";

/* HVAC state polling interval - detects physical remote changes, event-driven logic prevents unnecessary Zigbee traffic */
#define HVAC_UPDATE_INTERVAL_MS     30000  // 30 seconds (near real-time for physical remote detection)

/* Boot button configuration for factory reset */
#define BOOT_BUTTON_GPIO            GPIO_NUM_9
#define BUTTON_LONG_PRESS_TIME_MS   5000

/********************* Function Declarations **************************/
static esp_err_t deferred_driver_init(void);
static void hvac_update_zigbee_attributes(uint8_t param);
static void hvac_periodic_update(uint8_t param);
static esp_err_t button_init(void);
static void button_task(void *arg);
static void factory_reset_device(uint8_t param);

/* OTA validation functions */
void ota_validation_start(void);
void ota_validation_hw_init_ok(void);
void ota_validation_zigbee_init_ok(void);
void ota_validation_zigbee_connected(void);
void ota_validation_mark_invalid(void);

// Helper to fill a Zigbee ZCL string (first byte = length, then chars)
static void fill_zcl_string(char *buf, size_t bufsize, const char *src) {
    size_t len = strlen(src);
    if (len > bufsize - 1) len = bufsize - 1; // Reserve 1 byte for length
    buf[0] = (uint8_t)len;
    memcpy(&buf[1], src, len);
}

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

/* Boot button queue and ISR handler (interrupt-based like working project) */
static QueueHandle_t button_evt_queue = NULL;

static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = BOOT_BUTTON_GPIO;
    xQueueSendFromISR(button_evt_queue, &gpio_num, NULL);
}

/* Button monitoring task */
static void button_task(void *arg)
{
    uint32_t io_num;
    TickType_t press_start_time = 0;
    bool long_press_triggered = false;
    const TickType_t LONG_PRESS_DURATION = pdMS_TO_TICKS(BUTTON_LONG_PRESS_TIME_MS);
    const TickType_t DEBOUNCE_TIME = pdMS_TO_TICKS(50);  // 50ms debounce
    
    ESP_LOGI(TAG, "[BUTTON] Task started - waiting for button events");
    
    for (;;) {
        // Wait for button interrupt
        if (xQueueReceive(button_evt_queue, &io_num, portMAX_DELAY)) {
            // Debounce delay
            vTaskDelay(DEBOUNCE_TIME);
            
            // Disable interrupts during handling
            gpio_intr_disable(BOOT_BUTTON_GPIO);
            
            // Check button state
            int button_level = gpio_get_level(BOOT_BUTTON_GPIO);
            
            if (button_level == 0) {  // Button pressed (falling edge)
                press_start_time = xTaskGetTickCount();
                long_press_triggered = false;
                ESP_LOGI(TAG, "[BUTTON] Pressed - hold 5 sec for factory reset");
                
                // Wait while button is held with timeout to prevent infinite loop
                TickType_t poll_count = 0;
                const TickType_t MAX_POLL_TIME = pdMS_TO_TICKS(10000);  // 10s max
                
                while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && poll_count < MAX_POLL_TIME) {
                    TickType_t current_time = xTaskGetTickCount();
                    if ((current_time - press_start_time) >= LONG_PRESS_DURATION && !long_press_triggered) {
                        long_press_triggered = true;
                        ESP_LOGW(TAG, "[BUTTON] Long press detected! Triggering factory reset...");
                        // Schedule factory reset in Zigbee context
                        esp_zb_scheduler_alarm((esp_zb_callback_t)factory_reset_device, 0, 100);
                    }
                    vTaskDelay(pdMS_TO_TICKS(100));
                    poll_count += pdMS_TO_TICKS(100);
                }
                
                // Button released
                if (!long_press_triggered) {
                    uint32_t press_duration = pdTICKS_TO_MS(xTaskGetTickCount() - press_start_time);
                    ESP_LOGI(TAG, "[BUTTON] Released (held for %lu ms)", press_duration);
                }
            }
            
            // Re-enable interrupts after debounce
            vTaskDelay(DEBOUNCE_TIME);
            gpio_intr_enable(BOOT_BUTTON_GPIO);
        }
    }
}

/* Initialize boot button with interrupt-based handling */
static esp_err_t button_init(void)
{
    ESP_LOGI(TAG, "[INIT] Initializing boot button on GPIO%d", BOOT_BUTTON_GPIO);
    
    // Configure GPIO
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,  // Interrupt on button press (falling edge)
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Failed to configure GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[OK] GPIO configured");
    
    // Create event queue
    button_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!button_evt_queue) {
        ESP_LOGE(TAG, "[ERROR] Failed to create event queue");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[OK] Event queue created");
    
    // Install GPIO ISR service (may already be installed, that's OK)
    esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "[ERROR] Failed to install ISR service: %s", esp_err_to_name(isr_ret));
        return isr_ret;
    }
    ESP_LOGI(TAG, "[OK] ISR service ready");
    
    // Add ISR handler
    ret = gpio_isr_handler_add(BOOT_BUTTON_GPIO, button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Failed to add ISR handler: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[OK] ISR handler added");
    
    // Create button task
    BaseType_t task_ret = xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "[ERROR] Failed to create button task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[OK] Button task created");
    
    ESP_LOGI(TAG, "[OK] Boot button initialization complete");
    return ESP_OK;
}

static esp_err_t deferred_driver_init(void)
{
    ESP_LOGI(TAG, "[INIT] Starting deferred driver initialization...");
    
    /* Initialize boot button for factory reset */
    ESP_LOGI(TAG, "[INIT] Initializing boot button...");
    esp_err_t button_ret = button_init();
    if (button_ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Button initialization failed");
        return button_ret;
    }
    ESP_LOGI(TAG, "[INIT] Boot button initialization complete");
    
    /* Small delay to ensure logs are flushed */
    vTaskDelay(pdMS_TO_TICKS(10));
    
    /* Initialize HVAC UART driver */
    ESP_LOGI(TAG, "[INIT] Initializing HVAC UART driver...");
    ESP_LOGI(TAG, "[INIT] About to call hvac_driver_init()");
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
        
    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "[LEAVE] *** DEVICE LEFT THE NETWORK ***");
        if (err_status == ESP_OK) {
            esp_zb_zdo_signal_leave_params_t *leave_params = 
                (esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
            
            if (leave_params) {
                ESP_LOGW(TAG, "[LEAVE] Leave type: %s", 
                        leave_params->leave_type == 0 ? "RESET (no rejoin)" : "REJOIN");
                
                if (leave_params->leave_type == 0) {
                    // Device was removed from coordinator (factory reset type leave)
                    ESP_LOGW(TAG, "[LEAVE] Device was removed from the coordinator");
                    ESP_LOGW(TAG, "[LEAVE] Waiting 30 seconds before attempting to rejoin...");
                    
                    // Schedule rejoin after 30 seconds
                    esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                          ESP_ZB_BDB_MODE_NETWORK_STEERING, 30000);
                    
                    ESP_LOGI(TAG, "[LEAVE] Auto-rejoin scheduled in 30 seconds");
                } else {
                    // Leave with rejoin flag - device will rejoin automatically
                    ESP_LOGI(TAG, "[LEAVE] Leave with rejoin - device will rejoin automatically");
                }
            } else {
                ESP_LOGW(TAG, "[LEAVE] No leave parameters available");
                ESP_LOGW(TAG, "[LEAVE] Scheduling rejoin attempt in 30 seconds...");
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                      ESP_ZB_BDB_MODE_NETWORK_STEERING, 30000);
            }
        } else {
            ESP_LOGE(TAG, "[LEAVE] Leave error: %s", esp_err_to_name(err_status));
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
            
#ifdef ZB_ROUTER_ROLE
            ESP_LOGI(TAG, "[JOIN] Device Type: ROUTER (can route messages for other devices)");
#elif defined(ZB_ED_ROLE)
            ESP_LOGI(TAG, "[JOIN] Device Type: END DEVICE");
#else
            ESP_LOGI(TAG, "[JOIN] Device Type: UNKNOWN");
#endif
            
            ESP_LOGI(TAG, "[JOIN] Device is now online and ready");
            
            /* Mark Zigbee connection as successful for OTA validation */
            ota_validation_zigbee_connected();
            
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

    case ESP_ZB_NLME_STATUS_INDICATION:
        ESP_LOGD(TAG, "[NLME] Network status indication (status: %s)", esp_err_to_name(err_status));
        if (err_status == ESP_OK) {
            ESP_LOGD(TAG, "[NLME] Device successfully connected to network");
            /* Mark Zigbee connection as successful for OTA validation */
            ota_validation_zigbee_connected();
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
    
    /* Log RX message details - RSSI/LQI may be in message->info or require lower-layer API */
    ESP_LOGI(TAG, "RX: endpoint(%d), cluster(0x%x), attr(0x%x), size(%d)", 
             message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    
    /* Note: To get actual RSSI/LQI, check esp_zb SDK docs for message->info fields or 
     * use esp_zigbee_zcl_get_attribute() / lower-layer ieee802154 stats if available */
    
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
                ESP_LOGI(TAG, "Fan mode changed to 0x%02X", fan_mode);
                
                // Z2M sends ACW02 protocol values directly (0x00-0x06)
                // Just pass through to HVAC driver
                hvac_fan_t hvac_fan = (hvac_fan_t)fan_mode;
                
                ESP_LOGI(TAG, "Setting HVAC fan to: 0x%02X", hvac_fan);
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
    /* Handle Night Mode Switch - Endpoint 5 */
    else if (message->info.dst_endpoint == HA_ESP_NIGHT_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool on_off = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "[NIGHT] Mode %s", on_off ? "ON" : "OFF");
                hvac_set_night_mode(on_off);
                esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
            }
        }
    }
    /* Handle Purifier Switch - Endpoint 6 */
    else if (message->info.dst_endpoint == HA_ESP_PURIFIER_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool on_off = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "[PURIFIER] %s", on_off ? "ON" : "OFF");
                hvac_set_purifier(on_off);
                esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_update_zigbee_attributes, 0, 500);
            }
        }
    }
    /* Clean Status Binary Sensor - Endpoint 7 (Read-Only, no commands) */
    else if (message->info.dst_endpoint == HA_ESP_CLEAN_ENDPOINT) {
        // Clean status is read-only from AC, no commands accepted
        ESP_LOGW(TAG, "[CLEAN] Clean status is read-only");
    }
    /* Handle Mute Switch - Endpoint 8 */
    else if (message->info.dst_endpoint == HA_ESP_MUTE_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool on_off = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "[MUTE] %s", on_off ? "ON" : "OFF");
                hvac_set_mute(on_off);
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
        
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        ESP_LOGD(TAG, "[OTA] Upgrade value callback triggered");
        ret = zb_ota_upgrade_value_handler(*(esp_zb_zcl_ota_upgrade_value_message_t *)message);
        break;
        
    case ESP_ZB_CORE_OTA_UPGRADE_QUERY_IMAGE_RESP_CB_ID:
        ESP_LOGI(TAG, "[OTA] Query image response callback triggered");
        ret = zb_ota_query_image_resp_handler(*(esp_zb_zcl_ota_upgrade_query_image_resp_message_t *)message);
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
    
    uint8_t running_mode = 0x00;  // Declare at function scope for final log
    
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
    
    /* Update running mode based on power and mode */
    /* Note: Running mode shows what the AC is CURRENTLY doing (idle/heat/cool/fan)
     * This is different from system mode which is what it's SET to (off/auto/cool/heat/dry/fan)
     * For AUTO/DRY modes, we report 'idle' as running mode since we don't know what it's actually doing */
    running_mode = 0x00;  // Off/Idle (already declared at function scope)
    if (state.power_on) {
        switch (state.mode) {
            case HVAC_MODE_HEAT:
                running_mode = 0x04;  // Heat mode
                ESP_LOGD(TAG, "Running mode: HEAT (0x04)");
                break;
            case HVAC_MODE_COOL:
                running_mode = 0x03;  // Cool mode
                ESP_LOGD(TAG, "Running mode: COOL (0x03)");
                break;
            case HVAC_MODE_FAN:
                running_mode = 0x07;  // Fan only mode
                ESP_LOGD(TAG, "Running mode: FAN (0x07)");
                break;
            case HVAC_MODE_AUTO:
            case HVAC_MODE_DRY:
            default:
                running_mode = 0x00;  // Idle for auto/dry/unknown modes
                ESP_LOGD(TAG, "Running mode: IDLE/AUTO/DRY (0x00), state.mode=%d", state.mode);
                break;
        }
    } else {
        ESP_LOGD(TAG, "Running mode: OFF/IDLE (0x00)");
    }
    
    ESP_LOGI(TAG, "Setting running_mode=0x%02X to Zigbee (Power=%d, HVAC Mode=%d)", 
             running_mode, state.power_on, state.mode);
    
    esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID,
                                 &running_mode, false);
    
    /* Note: running_mode is not auto-reportable in ESP-Zigbee stack.
     * Z2M will read this attribute when needed (e.g., on state refresh or periodic polling). */
    
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
    
    /* Update Night Mode switch state - Endpoint 5 */
    esp_zb_zcl_set_attribute_val(HA_ESP_NIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &state.night_mode, false);
    
    /* Update Purifier switch state - Endpoint 6 */
    esp_zb_zcl_set_attribute_val(HA_ESP_PURIFIER_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &state.purifier_on, false);
    
    /* Update Clean status binary sensor - Endpoint 7 (Read-Only) */
    esp_zb_zcl_set_attribute_val(HA_ESP_CLEAN_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &state.clean_status, false);
    
    /* Update Mute switch state - Endpoint 8 */
    esp_zb_zcl_set_attribute_val(HA_ESP_MUTE_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &state.mute_on, false);
    
    /* Update error text in Basic cluster locationDescription attribute - Endpoint 1 */
    // Zigbee string format: first byte is length, followed by chars
    static char error_text_zigbee[65];  // Static to prevent stack corruption
    memset(error_text_zigbee, 0, sizeof(error_text_zigbee));  // Clear buffer
    
    size_t text_len = strlen(state.error_text);
    if (text_len > 64) text_len = 64;
    error_text_zigbee[0] = text_len;  // Length byte
    if (text_len > 0) {
        memcpy(&error_text_zigbee[1], state.error_text, text_len);
    }
    esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID,
                                 error_text_zigbee, false);
    
    /* Update Error Status binary sensor - Endpoint 9 */
    // Error status is ON when there's an error (non-empty error text)
    bool error_status_on = (text_len > 0);
    esp_zb_zcl_set_attribute_val(HA_ESP_ERROR_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                 &error_status_on, false);
    
    /* Log error text when error/warning is active */
    bool error_active = state.error || state.filter_dirty;
    if (error_active) {
        ESP_LOGW(TAG, "Error/Warning active: %s", state.error_text);
    }
    
    /* Update Fan Mode - Endpoint 1 */
    // Report ACW02 protocol values directly to Zigbee
    // Z2M converter will translate to custom names (quiet/low/low-med/etc)
    uint8_t zigbee_fan_mode = state.fan_speed;  // Pass through ACW02 value
    if (state.fan_speed == HVAC_FAN_TURBO) {
        zigbee_fan_mode = HVAC_FAN_SILENT;  // Map TURBO to SILENT for now
    }
    
    esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID,
                                 &zigbee_fan_mode, false);
    
    ESP_LOGI(TAG, "Updated Zigbee attributes: Mode=%d, LocalTemp=%.1f°C, TargetTemp=%d°C, Fan=%d, RunningMode=0x%02X", 
             system_mode, state.ambient_temp_c, state.target_temp_c, zigbee_fan_mode, running_mode);
    ESP_LOGI(TAG, "  Switches: Eco=%d, Night=%d, Display=%d, Purifier=%d, Clean=%d, Swing=%d, Mute=%d", 
             state.eco_mode, state.night_mode, state.display_on, state.purifier_on, 
             state.clean_status, state.swing_on, state.mute_on);
}

static void hvac_periodic_update(uint8_t param)
{
    static hvac_state_t previous_state = {0};
    static bool first_run = true;
    hvac_state_t current_state;
    
    /* Request fresh status from HVAC unit via UART */
    hvac_request_status();
    
    /* Get current state */
    esp_err_t ret = hvac_get_state(&current_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[POLL] Failed to get HVAC state");
        goto schedule_next;
    }
    
    /* On first run, always update to sync initial state */
    if (first_run) {
        ESP_LOGI(TAG, "[POLL] First run - syncing initial state to Zigbee");
        hvac_update_zigbee_attributes(0);
        memcpy(&previous_state, &current_state, sizeof(hvac_state_t));
        first_run = false;
        goto schedule_next;
    }
    
    /* Check if any state has changed */
    bool state_changed = false;
    
    if (current_state.power_on != previous_state.power_on) {
        ESP_LOGI(TAG, "[POLL] Power changed: %d -> %d", previous_state.power_on, current_state.power_on);
        state_changed = true;
    }
    if (current_state.mode != previous_state.mode) {
        ESP_LOGI(TAG, "[POLL] Mode changed: %d -> %d", previous_state.mode, current_state.mode);
        state_changed = true;
    }
    if (current_state.target_temp_c != previous_state.target_temp_c) {
        ESP_LOGI(TAG, "[POLL] Target temp changed: %d -> %d°C", previous_state.target_temp_c, current_state.target_temp_c);
        state_changed = true;
    }
    if (fabsf(current_state.ambient_temp_c - previous_state.ambient_temp_c) >= 0.5f) {
        ESP_LOGI(TAG, "[POLL] Ambient temp changed: %.1f -> %.1f°C", previous_state.ambient_temp_c, current_state.ambient_temp_c);
        state_changed = true;
    }
    if (current_state.fan_speed != previous_state.fan_speed) {
        ESP_LOGI(TAG, "[POLL] Fan speed changed: %d -> %d", previous_state.fan_speed, current_state.fan_speed);
        state_changed = true;
    }
    if (current_state.eco_mode != previous_state.eco_mode) {
        ESP_LOGI(TAG, "[POLL] Eco mode changed: %d -> %d", previous_state.eco_mode, current_state.eco_mode);
        state_changed = true;
    }
    if (current_state.swing_on != previous_state.swing_on) {
        ESP_LOGI(TAG, "[POLL] Swing changed: %d -> %d", previous_state.swing_on, current_state.swing_on);
        state_changed = true;
    }
    if (current_state.display_on != previous_state.display_on) {
        ESP_LOGI(TAG, "[POLL] Display changed: %d -> %d", previous_state.display_on, current_state.display_on);
        state_changed = true;
    }
    if (current_state.night_mode != previous_state.night_mode) {
        ESP_LOGI(TAG, "[POLL] Night mode changed: %d -> %d", previous_state.night_mode, current_state.night_mode);
        state_changed = true;
    }
    if (current_state.purifier_on != previous_state.purifier_on) {
        ESP_LOGI(TAG, "[POLL] Purifier changed: %d -> %d", previous_state.purifier_on, current_state.purifier_on);
        state_changed = true;
    }
    if (current_state.clean_status != previous_state.clean_status) {
        ESP_LOGI(TAG, "[POLL] Clean status changed: %d -> %d", previous_state.clean_status, current_state.clean_status);
        state_changed = true;
    }
    if (current_state.mute_on != previous_state.mute_on) {
        ESP_LOGI(TAG, "[POLL] Mute changed: %d -> %d", previous_state.mute_on, current_state.mute_on);
        state_changed = true;
    }
    if (strcmp(current_state.error_text, previous_state.error_text) != 0) {
        ESP_LOGI(TAG, "[POLL] Error text changed: '%s' -> '%s'", previous_state.error_text, current_state.error_text);
        state_changed = true;
    }
    
    /* Only update Zigbee if state changed - let REPORTING handle automatic updates */
    if (state_changed) {
        ESP_LOGI(TAG, "[POLL] State changed - updating Zigbee attributes (REPORTING will auto-send)");
        hvac_update_zigbee_attributes(0);
        memcpy(&previous_state, &current_state, sizeof(hvac_state_t));
    } else {
        ESP_LOGD(TAG, "[POLL] No state changes detected - skipping Zigbee update");
    }
    
schedule_next:
    /* Send keepalive to HVAC (required to maintain UART connection) */
    hvac_send_keepalive();
    
    /* Log Zigbee TX power every hour for monitoring */
    static uint8_t log_counter = 0;
    if (++log_counter >= 120) {  // Log every ~1 hour (120 * 30 seconds)
        int8_t tx_power = 0;
        esp_zb_get_tx_power(&tx_power);
        ESP_LOGI(TAG, "[RF] Zigbee TX power: %d dBm", tx_power);
        log_counter = 0;
    }
    
    /* Schedule next poll in 30 seconds - event-driven logic prevents unnecessary Zigbee traffic */
    esp_zb_scheduler_alarm((esp_zb_callback_t)hvac_periodic_update, 0, HVAC_UPDATE_INTERVAL_MS);
}

static void esp_zb_task(void *pvParameters)
{
    ESP_LOGI(TAG, "[START] Starting Zigbee task...");
    
    /* Initialize Zigbee stack as Router */
    ESP_LOGI(TAG, "[INIT] Initializing Zigbee stack as Router...");
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,
        .nwk_cfg = {
            .zczr_cfg = {
                .max_children = 10,
            },
        },
    };
    /* Configure antenna switch for XIAO ESP32-C6 if enabled in board.h */
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#if ENABLE_XIAO_EXTERNAL_ANT
    /* Drive FM8625H switch: GPIO3 = LOW, GPIO14 = HIGH to select external antenna */
    gpio_reset_pin(XIAO_ANT_GPIO_SEL0);
    gpio_set_direction(XIAO_ANT_GPIO_SEL0, GPIO_MODE_OUTPUT);
    gpio_set_level(XIAO_ANT_GPIO_SEL0, 0);

    gpio_reset_pin(XIAO_ANT_GPIO_SEL1);
    gpio_set_direction(XIAO_ANT_GPIO_SEL1, GPIO_MODE_OUTPUT);
    gpio_set_level(XIAO_ANT_GPIO_SEL1, 1);
    ESP_LOGW(TAG, "RF switch set for XIAO ESP32-C6: external antenna enabled");
#else
    gpio_reset_pin(XIAO_ANT_GPIO_SEL0);
    gpio_set_direction(XIAO_ANT_GPIO_SEL0, GPIO_MODE_OUTPUT);
    gpio_set_level(XIAO_ANT_GPIO_SEL0, 1);

    gpio_reset_pin(XIAO_ANT_GPIO_SEL1);
    gpio_set_direction(XIAO_ANT_GPIO_SEL1, GPIO_MODE_OUTPUT);
    gpio_set_level(XIAO_ANT_GPIO_SEL1, 0);
    ESP_LOGW(TAG, "RF switch set for XIAO ESP32-C6: onboard antenna enabled");
#endif
#endif

    esp_zb_init(&zb_nwk_cfg);
    ESP_LOGI(TAG, "[OK] Zigbee stack initialized");

    /* Log current Zigbee TX power and optionally apply a default from sdkconfig */
    {
    int8_t cur_power = 0;
    esp_zb_get_tx_power(&cur_power);
    ESP_LOGI(TAG, "Current Zigbee TX power: %d dBm", cur_power);

#if defined(CONFIG_ZB_DEFAULT_TX_POWER)
    /* If CONFIG_ZB_DEFAULT_TX_POWER is set in sdkconfig, apply it and show result */
    int8_t desired = CONFIG_ZB_DEFAULT_TX_POWER;
    ESP_LOGI(TAG, "CONFIG_ZB_DEFAULT_TX_POWER is set to %d dBm - applying...", desired);
    esp_zb_set_tx_power(desired);
    int8_t new_power = 0;
    esp_zb_get_tx_power(&new_power);
    ESP_LOGI(TAG, "New Zigbee TX power: %d dBm", new_power);
#else
    ESP_LOGD(TAG, "No default Zigbee TX power configured (CONFIG_ZB_DEFAULT_TX_POWER not set)");
#endif
    }
    
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
        .power_source = 0x01,  // 0x01 = Mains (single phase), 0x03 = Battery
    };
    esp_zb_attribute_list_t *esp_zb_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    
    /* Add optional Basic cluster attributes that Z2M expects */
    int ver_major = 1, ver_minor = 0, ver_patch = 0;
    sscanf(FW_VERSION, "%d.%d.%d", &ver_major, &ver_minor, &ver_patch);
    // Encode version as (major << 4) | minor for Zigbee compatibility
    uint8_t app_version = (ver_major << 4) | ver_minor;  // e.g., 1.1 = 0x11
    uint8_t stack_version = (ZB_STACK_VERSION << 4) | 0; // e.g., 3.0 = 0x30
    
    // Use CMake-injected version and date for Zigbee attributes
    char date_code[17];      // 16 chars max for Zigbee date code
    char sw_build_id[17];    // 16 chars max for Zigbee sw_build_id
    uint8_t hw_version = 1;  //> To add in CMAKE also%

    fill_zcl_string(date_code, sizeof(date_code), FW_DATE_CODE);
    fill_zcl_string(sw_build_id, sizeof(sw_build_id), FW_VERSION);
    
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app_version);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, (void*)date_code);
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, (void*)sw_build_id);
    
    /* Add locationDescription for error text (standard Basic cluster attribute 0x0010) */
    static char location_desc[65] = "\x00";  // Static buffer, empty initially (length = 0)
    esp_zb_basic_cluster_add_attr(esp_zb_basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID, location_desc);
    
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_hvac_clusters, esp_zb_basic_cluster, 
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  [OK] Basic cluster added with extended attributes");
    
    /* Create Thermostat cluster with REPORTING flag for persistence */
    ESP_LOGI(TAG, "  [+] Adding Thermostat cluster (0x0201) with REPORTING flag...");
    
    /* Create empty thermostat cluster */
    esp_zb_attribute_list_t *esp_zb_thermostat_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT);
    
    /* Add mandatory attributes with REPORTING flag */
    int16_t local_temp = 25 * 100;  // 25°C default
    int16_t cooling_setpoint = 24 * 100;  // 24°C default
    int16_t heating_setpoint = 22 * 100;  // 22°C default
    uint8_t control_sequence = 0x04;  // Cooling and heating
    uint8_t system_mode = 0x00;  // Off
    
    /* Local temperature - with REPORTING flag for automatic reports */
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_thermostat_cluster, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                            ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_S16,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &local_temp));
    
    /* Cooling setpoint - with REPORTING flag */
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_thermostat_cluster, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                            ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_S16,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &cooling_setpoint));
    
    /* Heating setpoint - with REPORTING flag */
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_thermostat_cluster, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                            ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_S16,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &heating_setpoint));
    
    /* System mode - with REPORTING flag */
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_thermostat_cluster, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                            ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &system_mode));
    
    /* Control sequence of operation - required mandatory attribute */
    ESP_ERROR_CHECK(esp_zb_thermostat_cluster_add_attr(esp_zb_thermostat_cluster,
                                                        ESP_ZB_ZCL_ATTR_THERMOSTAT_CONTROL_SEQUENCE_OF_OPERATION_ID,
                                                        &control_sequence));
    
    /* Add running_mode attribute (shows current operating mode) */
    uint8_t running_mode = 0x00;  // Initial state: idle
    esp_zb_thermostat_cluster_add_attr(esp_zb_thermostat_cluster, 
                                       ESP_ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID,
                                       &running_mode);
    
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_thermostat_cluster(esp_zb_hvac_clusters, esp_zb_thermostat_cluster,
                                                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    ESP_LOGI(TAG, "  [OK] Thermostat cluster added with REPORTING flag");
    
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
    
    /* Add OTA cluster for firmware updates */
    ESP_LOGI(TAG, "  [+] Adding OTA cluster (0x0019)...");
    esp_zb_ota_cluster_cfg_t ota_cluster_cfg = {
        .ota_upgrade_file_version = OTA_FILE_VERSION,     // From CMake
        .ota_upgrade_manufacturer = OTA_MANUFACTURER,     // From CMake
        .ota_upgrade_image_type = OTA_IMAGE_TYPE,         // From CMake
        .ota_upgrade_downloaded_file_ver = 0xFFFFFFFF,    // No pending update
    };
    esp_zb_attribute_list_t *esp_zb_ota_cluster = esp_zb_ota_cluster_create(&ota_cluster_cfg);

    /* Add OTA cluster attributes for Zigbee2MQTT OTA version display */
    uint32_t current_file_version = ota_cluster_cfg.ota_upgrade_file_version; // e.g. 0x01000000 for v1.0.0.0
    // Set OTA cluster version fields to match running firmware
    //uint8_t ota_app_version = ver_major;
    //uint8_t ota_stack_version = ver_minor;
    //esp_zb_ota_cluster_add_attr(esp_zb_ota_cluster, 0x0000, &ota_app_version); // currentZigbeeStackVersion (if not stack-managed)
    //esp_zb_ota_cluster_add_attr(esp_zb_ota_cluster, 0x0001, &ota_stack_version); // currentZigbeeStackBuild (if not stack-managed)
    esp_zb_ota_cluster_add_attr(esp_zb_ota_cluster, 0x0003, &current_file_version); // currentFileVersion

    /* Note: Attribute 0x0002 (currentZigbeeStackVersion) is managed by the Zigbee stack and cannot be added manually */

    /* Add client-specific OTA attributes */
    esp_zb_zcl_ota_upgrade_client_variable_t client_vars = {
        .timer_query = ESP_ZB_ZCL_OTA_UPGRADE_QUERY_TIMER_COUNT_DEF,
        .hw_version = 0x0101,
        .max_data_size = 223,
    };
    esp_zb_ota_cluster_add_attr(esp_zb_ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID, &client_vars);

    uint16_t server_addr = 0xffff;
    esp_zb_ota_cluster_add_attr(esp_zb_ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID, &server_addr);

    uint8_t server_ep = 0xff;
    esp_zb_ota_cluster_add_attr(esp_zb_ota_cluster, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, &server_ep);

    ESP_ERROR_CHECK(esp_zb_cluster_list_add_ota_cluster(esp_zb_hvac_clusters, esp_zb_ota_cluster,
                                                       ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
    ESP_LOGI(TAG, "  [OK] OTA cluster added (FW version: 0x%08lX)", ota_cluster_cfg.ota_upgrade_file_version);
    
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
    
    /* Create Eco Mode Switch - Endpoint 2 with REPORTING flag */
    ESP_LOGI(TAG, "[ECO] Creating Eco Mode switch endpoint %d with REPORTING flag...", HA_ESP_ECO_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_eco_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_eco_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_eco_clusters, esp_zb_eco_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag */
    bool eco_on_off_val = false;
    esp_zb_attribute_list_t *esp_zb_eco_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_eco_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &eco_on_off_val));
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
    
    /* Create Swing Switch - Endpoint 3 with REPORTING flag */
    ESP_LOGI(TAG, "[SWING] Creating Swing switch endpoint %d with REPORTING flag...", HA_ESP_SWING_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_swing_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_swing_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_swing_clusters, esp_zb_swing_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag */
    bool swing_on_off_val = false;
    esp_zb_attribute_list_t *esp_zb_swing_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_swing_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &swing_on_off_val));
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
    
    /* Create Display Switch - Endpoint 4 with REPORTING flag */
    ESP_LOGI(TAG, "[DISP] Creating Display switch endpoint %d with REPORTING flag...", HA_ESP_DISPLAY_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_display_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_display_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_display_clusters, esp_zb_display_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag */
    bool display_on_off_val = true;  // Display defaults to ON
    esp_zb_attribute_list_t *esp_zb_display_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_display_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &display_on_off_val));
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
    
    /* Create Night Mode Switch - Endpoint 5 with REPORTING flag */
    ESP_LOGI(TAG, "[NIGHT] Creating Night Mode switch endpoint %d with REPORTING flag...", HA_ESP_NIGHT_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_night_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_night_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_night_clusters, esp_zb_night_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag */
    bool night_on_off_val = false;
    esp_zb_attribute_list_t *esp_zb_night_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_night_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &night_on_off_val));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_night_clusters, esp_zb_night_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t night_endpoint_config = {
        .endpoint = HA_ESP_NIGHT_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_night_clusters, night_endpoint_config);
    ESP_LOGI(TAG, "[OK] Night Mode switch endpoint %d added", HA_ESP_NIGHT_ENDPOINT);
    
    /* Create Purifier Switch - Endpoint 6 with REPORTING flag */
    ESP_LOGI(TAG, "[PURIF] Creating Purifier switch endpoint %d with REPORTING flag...", HA_ESP_PURIFIER_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_purifier_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_purifier_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_purifier_clusters, esp_zb_purifier_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag */
    bool purifier_on_off_val = false;
    esp_zb_attribute_list_t *esp_zb_purifier_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_purifier_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &purifier_on_off_val));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_purifier_clusters, esp_zb_purifier_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t purifier_endpoint_config = {
        .endpoint = HA_ESP_PURIFIER_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_purifier_clusters, purifier_endpoint_config);
    ESP_LOGI(TAG, "[OK] Purifier switch endpoint %d added", HA_ESP_PURIFIER_ENDPOINT);
    
    /* Create Clean Status Binary Sensor - Endpoint 7 with REPORTING flag (Read-Only) */
    ESP_LOGI(TAG, "[CLEAN] Creating Clean status binary sensor endpoint %d with REPORTING flag...", HA_ESP_CLEAN_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_clean_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_clean_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_clean_clusters, esp_zb_clean_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag (Read-Only for binary sensor) */
    bool clean_on_off_val = false;  // Clean status defaults to false (no cleaning needed)
    esp_zb_attribute_list_t *esp_zb_clean_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_clean_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &clean_on_off_val));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_clean_clusters, esp_zb_clean_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t clean_endpoint_config = {
        .endpoint = HA_ESP_CLEAN_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,  // Binary sensor as on/off device
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_clean_clusters, clean_endpoint_config);
    ESP_LOGI(TAG, "[OK] Clean status binary sensor endpoint %d added", HA_ESP_CLEAN_ENDPOINT);
    
    /* Create Mute Switch - Endpoint 8 with REPORTING flag */
    ESP_LOGI(TAG, "[MUTE] Creating Mute switch endpoint %d with REPORTING flag...", HA_ESP_MUTE_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_mute_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_mute_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_mute_clusters, esp_zb_mute_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag */
    bool mute_on_off_val = false;  // Mute defaults to OFF
    esp_zb_attribute_list_t *esp_zb_mute_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_mute_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &mute_on_off_val));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_mute_clusters, esp_zb_mute_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t mute_endpoint_config = {
        .endpoint = HA_ESP_MUTE_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_mute_clusters, mute_endpoint_config);
    ESP_LOGI(TAG, "[OK] Mute switch endpoint %d added", HA_ESP_MUTE_ENDPOINT);
    
    /* Create Error Status Binary Sensor - Endpoint 9 with REPORTING flag (Read-Only) */
    ESP_LOGI(TAG, "[ERROR] Creating Error status binary sensor endpoint %d with REPORTING flag...", HA_ESP_ERROR_ENDPOINT);
    esp_zb_cluster_list_t *esp_zb_error_clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_attribute_list_t *esp_zb_error_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_error_clusters, esp_zb_error_basic_cluster,
                                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create On/Off cluster with REPORTING flag (Read-Only for binary sensor) */
    bool error_on_off_val = false;  // Error status defaults to false (no error)
    esp_zb_attribute_list_t *esp_zb_error_on_off_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF);
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(esp_zb_error_on_off_cluster, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                                            ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
                                            &error_on_off_val));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_on_off_cluster(esp_zb_error_clusters, esp_zb_error_on_off_cluster,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    esp_zb_endpoint_config_t error_endpoint_config = {
        .endpoint = HA_ESP_ERROR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,  // Binary sensor as on/off device
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_error_clusters, error_endpoint_config);
    ESP_LOGI(TAG, "[OK] Error status binary sensor endpoint %d added", HA_ESP_ERROR_ENDPOINT);
    
    /* Add manufacturer info */
    ESP_LOGI(TAG, "[INFO] Adding manufacturer info (Espressif, %s)...", CONFIG_IDF_TARGET);
    
    // Create separate manufacturer info structs for each endpoint to avoid memory corruption
    zcl_basic_manufacturer_info_t info_hvac = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_HVAC_ENDPOINT, &info_hvac);
    
    zcl_basic_manufacturer_info_t info_eco = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_ECO_ENDPOINT, &info_eco);
    
    zcl_basic_manufacturer_info_t info_swing = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_SWING_ENDPOINT, &info_swing);
    
    zcl_basic_manufacturer_info_t info_display = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_DISPLAY_ENDPOINT, &info_display);
    
    zcl_basic_manufacturer_info_t info_night = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_NIGHT_ENDPOINT, &info_night);
    
    zcl_basic_manufacturer_info_t info_purifier = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_PURIFIER_ENDPOINT, &info_purifier);
    
    zcl_basic_manufacturer_info_t info_clean = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_CLEAN_ENDPOINT, &info_clean);
    
    zcl_basic_manufacturer_info_t info_mute = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_MUTE_ENDPOINT, &info_mute);
    
    zcl_basic_manufacturer_info_t info_error = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_ERROR_ENDPOINT, &info_error);
    
    ESP_LOGI(TAG, "[OK] Manufacturer info added to all endpoints");
    
    /* Register device */
    ESP_LOGI(TAG, "[REG] Registering Zigbee device...");
    esp_zb_device_register(esp_zb_ep_list);
    ESP_LOGI(TAG, "[OK] Device registered");
    
    /* Debug: Verify REPORTING flag is set on thermostat attributes */
    ESP_LOGI(TAG, "🔍 Verifying REPORTING flag on thermostat attributes...");
    esp_zb_zcl_attr_t *attr;
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Local Temperature: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Cooling Setpoint: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Heating Setpoint: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID);
    if (attr) {
        ESP_LOGI(TAG, "  System Mode: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    /* Debug: Verify REPORTING flag on all on/off switch endpoints */
    ESP_LOGI(TAG, "🔍 Verifying REPORTING flag on on/off switch endpoints...");
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_ECO_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Eco Mode: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_SWING_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Swing: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_DISPLAY_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Display: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_NIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Night Mode: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_PURIFIER_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Purifier: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_CLEAN_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Clean Status: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_MUTE_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Mute: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    attr = esp_zb_zcl_get_attribute(HA_ESP_ERROR_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                     ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    if (attr) {
        ESP_LOGI(TAG, "  Error Status: access=0x%02x %s", attr->access,
                 (attr->access & ESP_ZB_ZCL_ATTR_ACCESS_REPORTING) ? "✅ REPORTING" : "❌ NO REPORTING");
    }
    
    ESP_LOGI(TAG, "[REG] Registering action handler...");
    esp_zb_core_action_handler_register(zb_action_handler);
    ESP_LOGI(TAG, "[OK] Action handler registered");
    
    /* Initialize and register OTA */
    ESP_LOGI(TAG, "[OTA] Initializing OTA functionality...");
    esp_err_t ota_ret = esp_zb_ota_init();
    if (ota_ret == ESP_OK) {
        esp_zb_ota_register_callbacks();
        ESP_LOGI(TAG, "[OK] OTA initialized and ready for updates");
    } else {
        ESP_LOGW(TAG, "[WARN] OTA initialization failed, updates disabled");
    }
    
    ESP_LOGI(TAG, "[CFG] Setting Zigbee channel mask: 0x%08lX", (unsigned long)ESP_ZB_PRIMARY_CHANNEL_MASK);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    
    /* Enable OTA logging for debugging */
    ESP_LOGI(TAG, "[TRACE] Enabling OTA subsystem tracing for debugging");
    esp_zb_set_trace_level_mask(ESP_ZB_TRACE_LEVEL_DEBUG, ESP_ZB_TRACE_SUBSYSTEM_OTA);
    
    ESP_LOGI(TAG, "[START] Starting Zigbee stack...");
    ESP_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "[OK] Zigbee stack started successfully");
    
    /* Start main Zigbee stack loop */
    esp_zb_stack_main_loop();
}

/* OTA Validation Logic */

static const char *OTA_VALIDATION_TAG = "OTA_VALIDATION";

typedef struct {
    bool hw_init_ok;
    bool zigbee_init_ok;
    bool zigbee_connected;
    uint32_t validation_start_time;
} validation_state_t;

static validation_state_t validation = {0};
#define VALIDATION_TIMEOUT_MS 180000  // 3 minutes - allows time for Zigbee network rejoin
#define ZIGBEE_CONNECT_TIMEOUT_MS 60000
static TimerHandle_t validation_timer = NULL;

static void check_validation_complete(void);

static void validation_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(OTA_VALIDATION_TAG, "Validation timer expired, checking status...");
    if (validation.hw_init_ok && validation.zigbee_init_ok && validation.zigbee_connected) {
        ESP_LOGI(OTA_VALIDATION_TAG, "All validation checks passed!");
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(OTA_VALIDATION_TAG, "New firmware marked as valid - rollback cancelled");
        } else {
            ESP_LOGE(OTA_VALIDATION_TAG, "Failed to mark firmware as valid: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(OTA_VALIDATION_TAG, "Validation checks incomplete:");
        ESP_LOGW(OTA_VALIDATION_TAG, "  - Hardware initialization: %s", validation.hw_init_ok ? "OK" : "FAILED");
        ESP_LOGW(OTA_VALIDATION_TAG, "  - Zigbee stack initialization: %s", validation.zigbee_init_ok ? "OK" : "FAILED");
        ESP_LOGW(OTA_VALIDATION_TAG, "  - Zigbee network connection: %s", validation.zigbee_connected ? "OK" : "FAILED");
        ESP_LOGW(OTA_VALIDATION_TAG, "Firmware will NOT be marked as valid - device may rollback on next boot");
    }
}

void ota_validation_start(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    ESP_LOGI(OTA_VALIDATION_TAG, "Running partition: %s at offset 0x%lx", running->label, running->address);
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        ESP_LOGI(OTA_VALIDATION_TAG, "OTA state: %d", ota_state);
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(OTA_VALIDATION_TAG, "First boot after OTA update - starting validation...");
            validation.hw_init_ok = false;
            validation.zigbee_init_ok = false;
            validation.zigbee_connected = false;
            validation.validation_start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            validation_timer = xTimerCreate(
                "validation_timer",
                pdMS_TO_TICKS(VALIDATION_TIMEOUT_MS),
                pdFALSE,
                NULL,
                validation_timer_callback
            );
            if (validation_timer != NULL) {
                if (xTimerStart(validation_timer, 0) == pdPASS) {
                    ESP_LOGI(OTA_VALIDATION_TAG, "Validation timer started");
                } else {
                    ESP_LOGE(OTA_VALIDATION_TAG, "Failed to start validation timer!");
                    xTimerDelete(validation_timer, 0);
                    validation_timer = NULL;
                }
            } else {
                ESP_LOGE(OTA_VALIDATION_TAG, "Failed to create validation timer!");
            }
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            ESP_LOGI(OTA_VALIDATION_TAG, "Running valid firmware - no validation needed");
        } else if (ota_state == ESP_OTA_IMG_INVALID) {
            ESP_LOGW(OTA_VALIDATION_TAG, "Running invalid firmware!");
        } else if (ota_state == ESP_OTA_IMG_ABORTED) {
            ESP_LOGW(OTA_VALIDATION_TAG, "Previous OTA was aborted");
        }
    } else {
        ESP_LOGE(OTA_VALIDATION_TAG, "Failed to get OTA state");
    }
}

void ota_validation_hw_init_ok(void)
{
    validation.hw_init_ok = true;
    ESP_LOGI(OTA_VALIDATION_TAG, "Hardware initialization validated");
    check_validation_complete();
}
void ota_validation_zigbee_init_ok(void)
{
    validation.zigbee_init_ok = true;
    ESP_LOGI(OTA_VALIDATION_TAG, "Zigbee stack initialization validated");
    check_validation_complete();
}
void ota_validation_zigbee_connected(void)
{
    // Only update validation state if timer is active (we're in validation mode)
    if (validation_timer == NULL) {
        return;
    }
    
    validation.zigbee_connected = true;
    ESP_LOGI(OTA_VALIDATION_TAG, "Zigbee network connection validated");
    check_validation_complete();
}
static void check_validation_complete(void)
{
    // Validation timer should exist during validation - if not, we're not in validation mode
    if (validation_timer == NULL) {
        ESP_LOGD(OTA_VALIDATION_TAG, "Not in validation mode, ignoring check");
        return;
    }
    
    if (validation.hw_init_ok && validation.zigbee_init_ok && validation.zigbee_connected) {
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - validation.validation_start_time;
        ESP_LOGI(OTA_VALIDATION_TAG, "All validation checks passed in %lu ms!", elapsed);
        
        // Stop and delete the timer
        if (validation_timer != NULL) {
            xTimerStop(validation_timer, 0);
            xTimerDelete(validation_timer, 0);
            validation_timer = NULL;
        }
        
        // Mark firmware as valid
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(OTA_VALIDATION_TAG, "New firmware marked as valid - rollback cancelled");
        } else {
            ESP_LOGE(OTA_VALIDATION_TAG, "Failed to mark firmware as valid: %s", esp_err_to_name(err));
        }
    }
}
void ota_validation_mark_invalid(void)
{
    ESP_LOGE(OTA_VALIDATION_TAG, "Marking firmware as invalid - rollback will occur!");
    if (validation_timer != NULL) {
        xTimerStop(validation_timer, 0);
        xTimerDelete(validation_timer, 0);
        validation_timer = NULL;
    }
    esp_ota_mark_app_invalid_rollback_and_reboot();
}
void app_main(void)
{
    ESP_LOGI(OTA_VALIDATION_TAG, "=== Application Starting ===");
    esp_app_desc_t app_desc;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
        ESP_LOGI(OTA_VALIDATION_TAG, "Firmware version: %s", app_desc.version);
        ESP_LOGI(OTA_VALIDATION_TAG, "Compile time: %s %s", app_desc.date, app_desc.time);
    }
    ota_validation_start();

    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    // Hardware init
    ESP_LOGI(OTA_VALIDATION_TAG, "Initializing hardware...");
    bool hw_ok = true; // Replace with actual check
    if (hw_ok) {
        ota_validation_hw_init_ok();
    } else {
        ESP_LOGE(OTA_VALIDATION_TAG, "Hardware initialization failed!");
        ota_validation_mark_invalid();
        return;
    }

    // Zigbee stack init
    ESP_LOGI(OTA_VALIDATION_TAG, "Initializing Zigbee stack...");
    bool zigbee_init_ok = true; // Replace with actual check
    if (zigbee_init_ok) {
        ota_validation_zigbee_init_ok();
    } else {
        ESP_LOGE(OTA_VALIDATION_TAG, "Zigbee initialization failed!");
        ota_validation_mark_invalid();
        return;
    }

    // Start Zigbee and wait for connection
    // When Zigbee connects successfully, call:
    // ota_validation_zigbee_connected();

    ESP_LOGI(OTA_VALIDATION_TAG, "=== Application Started ===");

    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
