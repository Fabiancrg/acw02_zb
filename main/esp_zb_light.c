/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier:  LicenseRef-Included
 *
 * Zigbee HA_on_off_light Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_light.h"
#include "driver/gpio.h"
#include "bme280_app.h"
#include "i2c_bus.h"
#include "nvs.h"

/* Define multistate input cluster constants if not available */
#ifndef ESP_ZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT
#define ESP_ZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT                0x0012U
#endif
#ifndef ESP_ZB_ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE_ID
#define ESP_ZB_ZCL_ATTR_MULTISTATE_INPUT_PRESENT_VALUE_ID     0x0055U
#endif

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

static const char *TAG = "ESP_ZB_ON_OFF_LIGHT";

/* Rain gauge configuration */
#define RAIN_GAUGE_GPIO         18              // GPIO pin for rain gauge reed switch
#define RAIN_MM_PER_PULSE       0.36f           // mm of rain per bucket tip
#define RAIN_GAUGE_TAG          "RAIN_GAUGE"
#define RAIN_NVS_NAMESPACE      "rain_gauge"
#define RAIN_NVS_KEY            "total_mm"

/* Rain gauge variables */
static QueueHandle_t rain_gauge_evt_queue = NULL;
static float total_rainfall_mm = 0.0f;
static uint32_t rain_pulse_count = 0;
static const char *RAIN_TAG = "RAIN_GAUGE";
static bool rain_gauge_enabled = false;  // Only enable when connected to network
static bool rain_gauge_isr_installed = false;  // Track ISR installation state
static float last_reported_rainfall_mm = 0.0f;  // Track last reported value for 1mm threshold
static TickType_t last_report_time = 0;  // Track last report time for hourly reporting

/* Button action tracking (no state needed for action-based buttons) */

/********************* Define functions **************************/
static void builtin_button_callback(button_action_t action);
static void external_button_callback(button_action_t action);
static void external_button_zigbee_update(uint8_t param);
static void builtin_button_zigbee_update(uint8_t param);
static void factory_reset_device(uint8_t param);
static void bme280_read_and_report(uint8_t param);
static void rain_gauge_init(void);
static void rain_gauge_isr_handler(void *arg);
static void rain_gauge_task(void *arg);
static void rain_gauge_zigbee_update(uint8_t param);
static esp_err_t rain_gauge_load_total(void);
static esp_err_t rain_gauge_save_total(void);
static void rain_gauge_enable_isr(void);
static void rain_gauge_disable_isr(void);
static bool rain_gauge_should_report(void);
static void rain_gauge_hourly_check(uint8_t param);
static esp_err_t deferred_driver_init(void)
{
    light_driver_init(LIGHT_DEFAULT_OFF);
    
    /* Initialize builtin button with callback */
    if (!builtin_button_driver_init(builtin_button_callback)) {
        ESP_LOGE(TAG, "Failed to initialize builtin button driver");
        return ESP_FAIL;
    }
    
    /* Initialize external button with callback */
    if (!external_button_driver_init(external_button_callback)) {
        ESP_LOGE(TAG, "Failed to initialize external button driver");
        return ESP_FAIL;
    }
    
    /* Initialize I2C and BME280 sensor */
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_6,      // ESP32-C6 default I2C SDA
        .scl_io_num = GPIO_NUM_7,      // ESP32-C6 default I2C SCL  
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,    // 100 KHz
    };
    
    i2c_bus_handle_t i2c_bus = i2c_bus_create(I2C_NUM_0, &i2c_cfg);
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C bus");
        return ESP_FAIL;
    }
    
    esp_err_t ret = bme280_app_init(i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BME280 sensor: %s", esp_err_to_name(ret));
        // Don't fail completely if BME280 is not connected
        ESP_LOGW(TAG, "Continuing without BME280 sensor");
    } else {
        ESP_LOGI(TAG, "BME280 sensor initialized successfully");
    }
    
    /* Initialize rain gauge */
    rain_gauge_init();
    
    return ESP_OK;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p       = signal_struct->p_app_signal;
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
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
            }
        } else {
            /* commissioning failed */
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
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
            
            /* Enable rain gauge now that we're connected */
            rain_gauge_enable_isr();
            ESP_LOGI(RAIN_TAG, "Rain gauge enabled - device connected to Zigbee network");
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            /* Disable rain gauge when not connected */
            rain_gauge_disable_isr();
            ESP_LOGW(RAIN_TAG, "Rain gauge disabled - not connected to network");
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;

    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)",
                        message->info.status);
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", message->info.dst_endpoint, message->info.cluster,
             message->attribute.id, message->attribute.data.size);
    if (message->info.dst_endpoint == HA_ESP_LIGHT1_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light 1 sets to %s", light_state ? "On" : "Off");
                light_driver_set_power(light_state);
            }
        }
    }
    if (message->info.dst_endpoint == HA_ESP_LIGHT2_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light 2 sets to %s", light_state ? "On" : "Off");
                light_driver_set_gpio_power(light_state);
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
        {
            esp_zb_zcl_report_attr_message_t *report_attr_message = (esp_zb_zcl_report_attr_message_t *)message;
            
            // Simplified logging - only show key information
            if (report_attr_message->cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
                bool state = report_attr_message->attribute.data.value ? 
                           *(bool*)report_attr_message->attribute.data.value : false;
                ESP_LOGI(TAG, "📡 LED%d: %s", report_attr_message->dst_endpoint, state ? "ON" : "OFF");
            } else if (report_attr_message->cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT) {
                float analog_value = report_attr_message->attribute.data.value ? 
                                    *(float*)report_attr_message->attribute.data.value : 0.0f;
                
                if (report_attr_message->attribute.id == ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID) {
                    /* Check which endpoint this is from */
                    if (report_attr_message->dst_endpoint == HA_ESP_BUTTON_ENDPOINT) {
                        /* Decode: action = value / 1000, counter = value % 1000 */
                        const char* action_names[] = {"none", "single", "double", "hold", "release_after_hold"};
                        int action_idx = (int)analog_value / 1000;
                        int counter = (int)analog_value % 1000;
                        
                        if (action_idx >= 1 && action_idx < 5) {
                            ESP_LOGI(TAG, "📡 Button: %s (#%d)", action_names[action_idx], counter);
                        }
                    } else if (report_attr_message->dst_endpoint == HA_ESP_RAIN_GAUGE_ENDPOINT) {
                        /* This is rain gauge total rainfall */
                        ESP_LOGI(TAG, "📡 Rain: %.2f mm", analog_value);
                    }
                }
            } else if (report_attr_message->cluster == ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT) {
                int16_t temp_raw = report_attr_message->attribute.data.value ? 
                                 *(int16_t*)report_attr_message->attribute.data.value : 0;
                ESP_LOGI(TAG, "📡 Temp: %.1f°C", temp_raw / 100.0f);
            } else if (report_attr_message->cluster == ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) {
                uint16_t hum_raw = report_attr_message->attribute.data.value ? 
                                 *(uint16_t*)report_attr_message->attribute.data.value : 0;
                ESP_LOGI(TAG, "📡 Humidity: %.1f%%", hum_raw / 100.0f);
            } else if (report_attr_message->cluster == ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT) {
                int16_t pressure_raw = report_attr_message->attribute.data.value ? 
                                     *(int16_t*)report_attr_message->attribute.data.value : 0;
                ESP_LOGI(TAG, "📡 Pressure: %.1f hPa", pressure_raw / 10.0f);
            } else {
                // Minimal logging for other clusters
                ESP_LOGD(TAG, "📤 Report: EP%d, cluster 0x%x", 
                         report_attr_message->dst_endpoint, report_attr_message->cluster);
            }
        }
        break;
    case 0x1005:  // Command response callback - normal Zigbee operation (attribute update acknowledgments)
        ESP_LOGD(TAG, "Zigbee command response received (0x1005) - normal operation");
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        ESP_LOGW(TAG, "  ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID = 0x%x", ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID);
        ESP_LOGW(TAG, "  ESP_ZB_CORE_REPORT_ATTR_CB_ID = 0x%x", ESP_ZB_CORE_REPORT_ATTR_CB_ID);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    
    /* Create endpoint list */
    esp_zb_ep_list_t *esp_zb_ep_list = esp_zb_ep_list_create();
    
    /* Create first light endpoint (LED strip) */
    esp_zb_on_off_light_cfg_t light1_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_cluster_list_t *esp_zb_light1_clusters = esp_zb_on_off_light_clusters_create(&light1_cfg);
    
    /* Add optional attributes to Basic cluster for endpoint 1 */
    esp_zb_attribute_list_t *basic1_cluster = esp_zb_cluster_list_get_cluster(esp_zb_light1_clusters, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (basic1_cluster) {
        uint8_t app_version = 1;
        uint8_t stack_version = 1; 
        uint8_t hw_version = 1;
        char date_code[] = "\x08""20251011";     // Length-prefixed: 8 bytes + "20251011"
        char sw_build_id[] = "\x06""v5.5.1";    // Length-prefixed: 6 bytes + "v5.5.1"
        
        esp_zb_basic_cluster_add_attr(basic1_cluster, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app_version);
        esp_zb_basic_cluster_add_attr(basic1_cluster, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
        esp_zb_basic_cluster_add_attr(basic1_cluster, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);
        esp_zb_basic_cluster_add_attr(basic1_cluster, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);
        esp_zb_basic_cluster_add_attr(basic1_cluster, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw_build_id);
    }
    
    esp_zb_endpoint_config_t endpoint1_config = {
        .endpoint = HA_ESP_LIGHT1_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_light1_clusters, endpoint1_config);
    
    /* Create second light endpoint (GPIO LED) */
    esp_zb_on_off_light_cfg_t light2_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_cluster_list_t *esp_zb_light2_clusters = esp_zb_on_off_light_clusters_create(&light2_cfg);
    
    /* Add optional attributes to Basic cluster for endpoint 2 */
    esp_zb_attribute_list_t *basic2_cluster = esp_zb_cluster_list_get_cluster(esp_zb_light2_clusters, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    if (basic2_cluster) {
        uint8_t app_version = 1;
        uint8_t stack_version = 1; 
        uint8_t hw_version = 1;
        char date_code[] = "\x08""20251011";     // Length-prefixed: 8 bytes + "20251011"
        char sw_build_id[] = "\x06""v5.5.1";    // Length-prefixed: 6 bytes + "v5.5.1"
        
        esp_zb_basic_cluster_add_attr(basic2_cluster, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app_version);
        esp_zb_basic_cluster_add_attr(basic2_cluster, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
        esp_zb_basic_cluster_add_attr(basic2_cluster, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);
        esp_zb_basic_cluster_add_attr(basic2_cluster, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, date_code);
        esp_zb_basic_cluster_add_attr(basic2_cluster, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw_build_id);
    }
    
    esp_zb_endpoint_config_t endpoint2_config = {
        .endpoint = HA_ESP_LIGHT2_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_light2_clusters, endpoint2_config);
    
    /* Create button sensor endpoint (Binary Input) */
    esp_zb_cluster_list_t *esp_zb_button_clusters = esp_zb_zcl_cluster_list_create();
    
    /* Create Basic cluster for button endpoint */
    esp_zb_basic_cluster_cfg_t basic_button_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *esp_zb_basic_button_cluster = esp_zb_basic_cluster_create(&basic_button_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_button_clusters, esp_zb_basic_button_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create Analog Input cluster for button (multistate not available) */
    esp_zb_analog_input_cluster_cfg_t button_analog_cfg = {
        .present_value = 0,  // Will be set to encoded action + counter
    };
    esp_zb_attribute_list_t *esp_zb_button_analog_cluster = esp_zb_analog_input_cluster_create(&button_analog_cfg);
    
    /* Add description attribute */
    char button_description[] = "\x0D""Button Action";  // Length-prefixed: 13 bytes + "Button Action"
    esp_zb_analog_input_cluster_add_attr(esp_zb_button_analog_cluster, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, button_description);
    
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_analog_input_cluster(esp_zb_button_clusters, esp_zb_button_analog_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Add Identify cluster for button endpoint */
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(esp_zb_button_clusters, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    esp_zb_endpoint_config_t endpoint_button_config = {
        .endpoint = HA_ESP_BUTTON_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_button_clusters, endpoint_button_config);

    /* Create BME280 environmental sensor endpoint */
    esp_zb_cluster_list_t *esp_zb_bme280_clusters = esp_zb_zcl_cluster_list_create();
    
    /* Create Basic cluster for BME280 endpoint */
    esp_zb_basic_cluster_cfg_t basic_bme280_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *esp_zb_basic_bme280_cluster = esp_zb_basic_cluster_create(&basic_bme280_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_bme280_clusters, esp_zb_basic_bme280_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create Temperature measurement cluster */
    esp_zb_temperature_meas_cluster_cfg_t temp_meas_cfg = {
        .measured_value = 0x8000,  // Invalid/unknown temperature value
        .min_value = -40 * 100,     // -40°C in centidegrees
        .max_value = 85 * 100,      // 85°C in centidegrees (BME280 range)
    };
    esp_zb_attribute_list_t *esp_zb_temperature_cluster = esp_zb_temperature_meas_cluster_create(&temp_meas_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_temperature_meas_cluster(esp_zb_bme280_clusters, esp_zb_temperature_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create Humidity measurement cluster */
    esp_zb_humidity_meas_cluster_cfg_t hum_meas_cfg = {
        .measured_value = 0xFFFF,   // Invalid/unknown humidity value
        .min_value = 0 * 100,       // 0% in centipercent
        .max_value = 100 * 100,     // 100% in centipercent
    };
    esp_zb_attribute_list_t *esp_zb_humidity_cluster = esp_zb_humidity_meas_cluster_create(&hum_meas_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_humidity_meas_cluster(esp_zb_bme280_clusters, esp_zb_humidity_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create Pressure measurement cluster */
    esp_zb_pressure_meas_cluster_cfg_t pressure_meas_cfg = {
        .measured_value = ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_UNKNOWN,  // Invalid/unknown pressure value initially
    };
    esp_zb_attribute_list_t *esp_zb_pressure_cluster = esp_zb_pressure_meas_cluster_create(&pressure_meas_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_pressure_meas_cluster(esp_zb_bme280_clusters, esp_zb_pressure_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Add Identify cluster for BME280 endpoint */
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(esp_zb_bme280_clusters, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    esp_zb_endpoint_config_t endpoint_bme280_config = {
        .endpoint = HA_ESP_BME280_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_bme280_clusters, endpoint_bme280_config);

    /* Create rain gauge sensor endpoint */
    esp_zb_cluster_list_t *esp_zb_rain_clusters = esp_zb_zcl_cluster_list_create();
    
    /* Create Basic cluster for rain gauge endpoint */
    esp_zb_basic_cluster_cfg_t basic_rain_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *esp_zb_basic_rain_cluster = esp_zb_basic_cluster_create(&basic_rain_cfg);
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(esp_zb_rain_clusters, esp_zb_basic_rain_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Create Analog Input cluster for rain gauge (reports rainfall in mm) */
    esp_zb_analog_input_cluster_cfg_t rain_analog_cfg = {
        .present_value = 0.0f,  // Will be set to total rainfall in mm
    };
    esp_zb_attribute_list_t *esp_zb_rain_analog_cluster = esp_zb_analog_input_cluster_create(&rain_analog_cfg);
    
    /* Add description attribute */
    char rain_description[] = "\x0E""Rainfall Total";  // Length-prefixed: 14 bytes + "Rainfall Total"
    esp_zb_analog_input_cluster_add_attr(esp_zb_rain_analog_cluster, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, rain_description);
    
    /* Add engineering units attribute (mm) */
    uint16_t engineering_units = 0;  // 0 = dimensionless, could use custom units
    esp_zb_analog_input_cluster_add_attr(esp_zb_rain_analog_cluster, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_ENGINEERING_UNITS_ID, &engineering_units);
    
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_analog_input_cluster(esp_zb_rain_clusters, esp_zb_rain_analog_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    /* Add Identify cluster for rain gauge endpoint */
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(esp_zb_rain_clusters, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
    
    esp_zb_endpoint_config_t endpoint_rain_config = {
        .endpoint = HA_ESP_RAIN_GAUGE_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0
    };
    esp_zb_ep_list_add_ep(esp_zb_ep_list, esp_zb_rain_clusters, endpoint_rain_config);

    /* Add manufacturer info to all endpoints */
    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_LIGHT1_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_LIGHT2_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_BUTTON_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_BME280_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(esp_zb_ep_list, HA_ESP_RAIN_GAUGE_ENDPOINT, &info);

    esp_zb_device_register(esp_zb_ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    
    /* Start BME280 periodic reading (initial delay 5 seconds) */
    esp_zb_scheduler_alarm((esp_zb_callback_t)bme280_read_and_report, 0, 5000);
    
    /* Note: Button monitoring is now interrupt-based, no polling task needed */
    
    /* Start main Zigbee stack loop */
    esp_zb_stack_main_loop();
}



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

/* Deferred function for safe Zigbee calls */
static void builtin_button_zigbee_update(uint8_t param)
{
    /* Read current LED state directly from hardware driver */
    bool current_led_state = light_driver_get_power();
    
    /* Toggle the state */
    bool new_led_state = !current_led_state;
    ESP_LOGI(TAG, "Toggling LED strip from %s to %s", current_led_state ? "ON" : "OFF", new_led_state ? "ON" : "OFF");
    
    /* Update the hardware immediately */
    light_driver_set_power(new_led_state);
    
    /* Check if device is connected to network for Zigbee reporting */
    uint16_t short_addr = esp_zb_get_short_address();
    if (short_addr == 0xFFFF || short_addr == 0x0000) {
        ESP_LOGW(TAG, "⚠️ Device not connected to Zigbee network (addr: 0x%04x), LED toggled locally only", short_addr);
        return;
    }
    
    /* Add small delay to ensure Zigbee stack is ready */
    vTaskDelay(pdMS_TO_TICKS(10));
    
    /* Update the Zigbee attribute and force reporting */
    esp_err_t ret = esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT1_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, 
                               ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, 
                               &new_led_state, true);  // Force immediate reporting
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ LED strip toggled to %s and reported to Zigbee", new_led_state ? "ON" : "OFF");
    } else {
        /* Try without forced reporting as fallback (0x88 is common when forced reporting not available) */
        ESP_LOGD(TAG, "Forced reporting failed (0x%x), trying without reporting...", ret);
        ret = esp_zb_zcl_set_attribute_val(HA_ESP_LIGHT1_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, 
                                   ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, 
                                   &new_led_state, false);  // No forced reporting
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ LED strip toggled to %s (Zigbee attribute updated)", new_led_state ? "ON" : "OFF");
        } else {
            ESP_LOGE(TAG, "❌ Zigbee attribute update failed: 0x%x", ret);
        }
    }
}

static void builtin_button_callback(button_action_t action)
{
    const char* action_names[] = {
        "none", "single", "double", "hold", "release_after_hold"
    };
    
    if (action != BUTTON_ACTION_NONE && action < (sizeof(action_names)/sizeof(action_names[0]))) {
        ESP_LOGI(TAG, "🔘 Builtin button action detected: %s", action_names[action]);
        
        if (action == BUTTON_ACTION_HOLD) {
            /* Long press detected - Factory reset */
            ESP_LOGW(TAG, "🔄 Factory reset triggered by long press - device will restart and rejoin network");
            esp_zb_scheduler_alarm((esp_zb_callback_t)factory_reset_device, 0, 100);
        } else if (action == BUTTON_ACTION_SINGLE) {
            /* Short press - Toggle LED strip */
            ESP_LOGI(TAG, "💡 Short press - scheduling LED strip toggle");
            esp_zb_scheduler_alarm((esp_zb_callback_t)builtin_button_zigbee_update, 0, 1);
        }
    }
}

/* Deferred function for safe Zigbee calls for external button */
static void external_button_zigbee_update(uint8_t param)
{
    button_action_t action = (button_action_t)param;
    const char* action_names[] = {
        "none", "single", "double", "hold", "release_after_hold"
    };
    
    if (action != BUTTON_ACTION_NONE && action < (sizeof(action_names)/sizeof(action_names[0]))) {
        static uint32_t button_counter = 0;
        button_counter++;
        
        /* Encode: (action * 1000) + (counter % 1000) to ensure unique values */
        float encoded_value = (float)((action * 1000) + (button_counter % 1000));
        
        esp_err_t ret = esp_zb_zcl_set_attribute_val(HA_ESP_BUTTON_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, 
                                   ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, 
                                   &encoded_value, true);  // Force immediate reporting
        
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ Button action %s sent (encoded: %.0f) - press #%u", 
                     action_names[action], encoded_value, button_counter);
        } else {
            ESP_LOGE(TAG, "❌ Failed to send button action: %s", esp_err_to_name(ret));
        }
    }
}

static void external_button_callback(button_action_t action)
{
    const char* action_names[] = {
        "none", "single", "double", "hold", "release_after_hold"
    };
    
    if (action != BUTTON_ACTION_NONE && action < (sizeof(action_names)/sizeof(action_names[0]))) {
        ESP_LOGI(TAG, "🔘 External button action detected: %s - scheduling Zigbee update", action_names[action]);
        
        /* Schedule the Zigbee update to run in the Zigbee task context */
        esp_zb_scheduler_alarm((esp_zb_callback_t)external_button_zigbee_update, (uint8_t)action, 1);
    }
}

/* BME280 sensor reading and reporting functions */
static void bme280_read_and_report(uint8_t param)
{
    float temperature, humidity, pressure;
    esp_err_t ret;
    
    // Read temperature
    ret = bme280_app_read_temperature(&temperature);
    if (ret == ESP_OK) {
        // Convert to centidegrees (Zigbee temperature unit: 0.01°C)
        int16_t temp_centidegrees = (int16_t)(temperature * 100);
        
        ret = esp_zb_zcl_set_attribute_val(HA_ESP_BME280_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT, 
                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID, 
                                          &temp_centidegrees, false);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "🌡️ Temperature: %.2f°C reported to Zigbee", temperature);
        } else {
            ESP_LOGE(TAG, "Failed to report temperature: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Failed to read temperature: %s", esp_err_to_name(ret));
    }
    
    // Read humidity  
    ret = bme280_app_read_humidity(&humidity);
    if (ret == ESP_OK) {
        // Convert to centipercent (Zigbee humidity unit: 0.01%)
        uint16_t hum_centipercent = (uint16_t)(humidity * 100);
        
        ret = esp_zb_zcl_set_attribute_val(HA_ESP_BME280_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT, 
                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_REL_HUMIDITY_MEASUREMENT_VALUE_ID, 
                                          &hum_centipercent, false);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "💧 Humidity: %.2f%% reported to Zigbee", humidity);
        } else {
            ESP_LOGE(TAG, "Failed to report humidity: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Failed to read humidity: %s", esp_err_to_name(ret));
    }
    
    // Read pressure
    ret = bme280_app_read_pressure(&pressure);
    if (ret == ESP_OK) {
        // Convert pressure from hPa to 0.1 kPa (Zigbee pressure unit)
        // 1 hPa = 0.1 kPa, so pressure in hPa * 10 = pressure in 0.1 kPa
        int16_t pressure_zigbee = (int16_t)(pressure * 10);
        
        ret = esp_zb_zcl_set_attribute_val(HA_ESP_BME280_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT, 
                                          ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_PRESSURE_MEASUREMENT_VALUE_ID, 
                                          &pressure_zigbee, false);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "🌪️  Pressure: %.2f hPa (raw: %d x0.1kPa) reported to Zigbee", pressure, pressure_zigbee);
        } else {
            ESP_LOGE(TAG, "Failed to report pressure: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGE(TAG, "Failed to read pressure: %s", esp_err_to_name(ret));
    }
    
    // Schedule next reading (every 30 seconds)
    esp_zb_scheduler_alarm((esp_zb_callback_t)bme280_read_and_report, 0, 30000);
}

/* Rain gauge implementation */
static esp_err_t rain_gauge_load_total(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(RAIN_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGI(RAIN_TAG, "No previous rainfall data found, starting from 0.0mm");
        total_rainfall_mm = 0.0f;
        return ESP_OK;
    }
    
    size_t required_size = sizeof(float);
    ret = nvs_get_blob(nvs_handle, RAIN_NVS_KEY, &total_rainfall_mm, &required_size);
    if (ret != ESP_OK) {
        ESP_LOGI(RAIN_TAG, "No previous rainfall data found, starting from 0.0mm");
        total_rainfall_mm = 0.0f;
    } else {
        ESP_LOGI(RAIN_TAG, "Loaded previous rainfall total: %.2f mm", total_rainfall_mm);
    }
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

static esp_err_t rain_gauge_save_total(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(RAIN_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(RAIN_TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_blob(nvs_handle, RAIN_NVS_KEY, &total_rainfall_mm, sizeof(float));
    if (ret != ESP_OK) {
        ESP_LOGE(RAIN_TAG, "Failed to save rainfall total: %s", esp_err_to_name(ret));
    } else {
        ret = nvs_commit(nvs_handle);
        if (ret == ESP_OK) {
            ESP_LOGD(RAIN_TAG, "Saved rainfall total: %.2f mm", total_rainfall_mm);
        }
    }
    
    nvs_close(nvs_handle);
    return ret;
}

static void IRAM_ATTR rain_gauge_isr_handler(void *arg)
{
    uint32_t gpio_num = RAIN_GAUGE_GPIO;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(rain_gauge_evt_queue, &gpio_num, &xHigherPriorityTaskWoken);
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void rain_gauge_task(void *arg)
{
    uint32_t io_num;
    TickType_t last_pulse_time = 0;
    const TickType_t DEBOUNCE_TIME = pdMS_TO_TICKS(200); // 200ms debounce - more than adequate for rain gauge
    const TickType_t BOUNCE_SETTLE_TIME = pdMS_TO_TICKS(1000); // 1 second to completely eliminate bounce issues
    
    ESP_LOGI(RAIN_TAG, "Rain gauge task started, waiting for events...");
    
    for (;;) {
        if (xQueueReceive(rain_gauge_evt_queue, &io_num, portMAX_DELAY)) {
            ESP_LOGI(RAIN_TAG, "🔍 Rain gauge interrupt received on GPIO%u (enabled: %s)", io_num, rain_gauge_enabled ? "YES" : "NO");
            
            // Only process if rain gauge is enabled (connected to network)
            if (!rain_gauge_enabled) {
                ESP_LOGW(RAIN_TAG, "Rain gauge interrupt ignored - not connected to network");
                continue;
            }
            
            TickType_t current_time = xTaskGetTickCount();
            uint32_t time_diff_ms = pdTICKS_TO_MS(current_time - last_pulse_time);
            
            ESP_LOGI(RAIN_TAG, "🕐 Time since last pulse: %u ms (debounce: %u ms)", time_diff_ms, pdTICKS_TO_MS(DEBOUNCE_TIME));
            
            // Reasonable debounce check - 100ms minimum between pulses
            if ((current_time - last_pulse_time) > DEBOUNCE_TIME) {
                int pin_level = gpio_get_level(RAIN_GAUGE_GPIO);
                ESP_LOGI(RAIN_TAG, "🔌 GPIO%d level: %d", RAIN_GAUGE_GPIO, pin_level);
                
                // Enhanced verification: check pin is high and wait for signal stability
                if (pin_level == 1) {
                    vTaskDelay(pdMS_TO_TICKS(20)); // Wait 20ms for signal stability
                    
                    int pin_level_after = gpio_get_level(RAIN_GAUGE_GPIO);
                    ESP_LOGI(RAIN_TAG, "🔌 GPIO%d level after 20ms: %d", RAIN_GAUGE_GPIO, pin_level_after);
                    
                    // Verify signal is still stable - for rain gauge, we want rock-solid detection
                    if (pin_level_after == 1) {
                        // Temporarily disable ISR to prevent bounce interrupts
                        ESP_LOGI(RAIN_TAG, "🔇 Disabling ISR for %u ms to prevent bounce", pdTICKS_TO_MS(BOUNCE_SETTLE_TIME));
                        gpio_isr_handler_remove(RAIN_GAUGE_GPIO);
                        
                        last_pulse_time = current_time;
                        rain_pulse_count++;
                        total_rainfall_mm += RAIN_MM_PER_PULSE;
                        
                        ESP_LOGI(RAIN_TAG, "🌧️ Rain pulse #%u detected! Total: %.2f mm (+%.2f mm)", 
                                 rain_pulse_count, total_rainfall_mm, RAIN_MM_PER_PULSE);
                        
                        // Save to NVS every 10 pulses
                        if (rain_pulse_count % 10 == 0) {
                            rain_gauge_save_total();
                        }
                        
                        // Check if we should report to Zigbee (every hour or 1mm increment)
                        if (rain_gauge_should_report()) {
                            esp_zb_scheduler_alarm((esp_zb_callback_t)rain_gauge_zigbee_update, 0, 10);
                            last_reported_rainfall_mm = total_rainfall_mm;
                            last_report_time = current_time;
                        }
                        
                        // Wait for switch bounce to settle, then re-enable ISR
                        vTaskDelay(BOUNCE_SETTLE_TIME);
                        
                        // Re-enable ISR if still connected to network
                        if (rain_gauge_enabled) {
                            // Clear any pending interrupts before re-enabling ISR
                            gpio_intr_disable(RAIN_GAUGE_GPIO);
                            gpio_intr_enable(RAIN_GAUGE_GPIO);
                            
                            // Clear the interrupt queue of any bounce events that occurred during settle period
                            uint32_t dummy;
                            while (xQueueReceive(rain_gauge_evt_queue, &dummy, 0) == pdTRUE) {
                                // Drain any queued bounce events
                            }
                            
                            esp_err_t ret = gpio_isr_handler_add(RAIN_GAUGE_GPIO, rain_gauge_isr_handler, NULL);
                            if (ret == ESP_OK) {
                                ESP_LOGI(RAIN_TAG, "🔊 ISR re-enabled after bounce settle period (queue cleared)");
                            } else {
                                ESP_LOGE(RAIN_TAG, "❌ Failed to re-enable ISR: %s", esp_err_to_name(ret));
                            }
                        }
                    } else {
                        ESP_LOGW(RAIN_TAG, "❌ Rain pulse rejected - signal not stable (bounce detected)");
                    }
                } else {
                    ESP_LOGW(RAIN_TAG, "❌ Rain pulse rejected - pin low during interrupt (noise)");
                }
            } else {
                ESP_LOGW(RAIN_TAG, "⏱️ Rain pulse ignored - debounce protection (%ums not elapsed)", pdTICKS_TO_MS(DEBOUNCE_TIME));
            }
        }
    }
}

static void rain_gauge_enable_isr(void)
{
    ESP_LOGI(RAIN_TAG, "🔧 Enabling rain gauge ISR on GPIO%d (installed: %s)", RAIN_GAUGE_GPIO, rain_gauge_isr_installed ? "YES" : "NO");
    
    // Set enabled flag first
    rain_gauge_enabled = true;
    
    // Add ISR handler (will succeed even if already added)
    esp_err_t ret = gpio_isr_handler_add(RAIN_GAUGE_GPIO, rain_gauge_isr_handler, NULL);
    if (ret == ESP_OK) {
        rain_gauge_isr_installed = true;
        ESP_LOGI(RAIN_TAG, "✅ Rain gauge ISR enabled successfully on GPIO%d", RAIN_GAUGE_GPIO);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        // ISR already installed - this is fine
        rain_gauge_isr_installed = true;
        ESP_LOGI(RAIN_TAG, "✅ Rain gauge ISR already installed, now enabled");
    } else {
        ESP_LOGE(RAIN_TAG, "❌ Failed to enable rain gauge ISR: %s", esp_err_to_name(ret));
    }
    
    // Test GPIO level at enable time
    int current_level = gpio_get_level(RAIN_GAUGE_GPIO);
    ESP_LOGI(RAIN_TAG, "🔌 Current GPIO%d level at enable: %d", RAIN_GAUGE_GPIO, current_level);
}

static void rain_gauge_disable_isr(void)
{
    if (rain_gauge_isr_installed) {
        esp_err_t ret = gpio_isr_handler_remove(RAIN_GAUGE_GPIO);
        if (ret == ESP_OK) {
            rain_gauge_isr_installed = false;
            rain_gauge_enabled = false;
            ESP_LOGI(RAIN_TAG, "Rain gauge ISR disabled and removed");
        } else {
            ESP_LOGW(RAIN_TAG, "Failed to remove rain gauge ISR: %s", esp_err_to_name(ret));
            // Still disable processing even if removal failed
            rain_gauge_enabled = false;
        }
    } else {
        rain_gauge_enabled = false;
        ESP_LOGI(RAIN_TAG, "Rain gauge ISR already disabled");
    }
}

static bool rain_gauge_should_report(void)
{
    TickType_t current_time = xTaskGetTickCount();
    const TickType_t HOUR_MS = pdMS_TO_TICKS(3600000); // 1 hour in milliseconds
    const float REPORT_THRESHOLD_MM = 1.0f; // Report every 1mm
    
    // Check if 1 hour has passed since last report
    bool hour_passed = (current_time - last_report_time) >= HOUR_MS;
    
    // Check if we've accumulated 1mm more rainfall
    bool threshold_reached = (total_rainfall_mm - last_reported_rainfall_mm) >= REPORT_THRESHOLD_MM;
    
    if (hour_passed) {
        ESP_LOGI(RAIN_TAG, "📊 Hourly report triggered (%.2f mm total)", total_rainfall_mm);
        return true;
    } else if (threshold_reached) {
        ESP_LOGI(RAIN_TAG, "📊 Rainfall threshold report triggered (+%.2f mm, %.2f mm total)", 
                 total_rainfall_mm - last_reported_rainfall_mm, total_rainfall_mm);
        return true;
    }
    
    return false;
}

static void rain_gauge_hourly_check(uint8_t param)
{
    // This function is called every hour to ensure reporting even with very light rain
    if (rain_gauge_enabled && (total_rainfall_mm != last_reported_rainfall_mm)) {
        ESP_LOGI(RAIN_TAG, "📊 Periodic hourly check - reporting current rainfall");
        esp_zb_scheduler_alarm((esp_zb_callback_t)rain_gauge_zigbee_update, 0, 10);
        last_reported_rainfall_mm = total_rainfall_mm;
        last_report_time = xTaskGetTickCount();
    }
    
    // Debug: Check GPIO state and ISR status
    int gpio_level = gpio_get_level(RAIN_GAUGE_GPIO);
    ESP_LOGI(RAIN_TAG, "🔍 Hourly debug - GPIO%d level: %d, ISR installed: %s, enabled: %s", 
             RAIN_GAUGE_GPIO, gpio_level, rain_gauge_isr_installed ? "YES" : "NO", rain_gauge_enabled ? "YES" : "NO");
    
    // Schedule next hourly check
    esp_zb_scheduler_alarm((esp_zb_callback_t)rain_gauge_hourly_check, 0, 3600000); // 1 hour
}

static void rain_gauge_zigbee_update(uint8_t param)
{
    esp_err_t ret = esp_zb_zcl_set_attribute_val(HA_ESP_RAIN_GAUGE_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
                                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
                                                 &total_rainfall_mm, true);  // Force reporting
    
    if (ret == ESP_OK) {
        ESP_LOGI(RAIN_TAG, "✅ Rainfall total %.2f mm reported to Zigbee", total_rainfall_mm);
    } else {
        ESP_LOGE(RAIN_TAG, "❌ Failed to report rainfall: %s", esp_err_to_name(ret));
    }
}

static void rain_gauge_init(void)
{
    ESP_LOGI(RAIN_TAG, "Initializing rain gauge on GPIO%d (disabled until network connection)", RAIN_GAUGE_GPIO);
    
    /* Start disabled - will be enabled when connected to network */
    rain_gauge_enabled = false;
    
    /* Load previous rainfall total from NVS */
    rain_gauge_load_total();
    
    /* Configure GPIO for rain gauge */
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,  // Interrupt on rising edge
        .pin_bit_mask = (1ULL << RAIN_GAUGE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0,   // No pull-up needed
        .pull_down_en = 1, // Enable internal pull-down for additional noise protection
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(RAIN_TAG, "Failed to configure GPIO%d: %s", RAIN_GAUGE_GPIO, esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(RAIN_TAG, "✅ GPIO%d configured successfully", RAIN_GAUGE_GPIO);
    
    // Check initial GPIO state
    int initial_level = gpio_get_level(RAIN_GAUGE_GPIO);
    ESP_LOGI(RAIN_TAG, "🔌 Initial GPIO%d level: %d (with pull-down)", RAIN_GAUGE_GPIO, initial_level);
    
    /* Create queue for GPIO events */
    rain_gauge_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!rain_gauge_evt_queue) {
        ESP_LOGE(RAIN_TAG, "Failed to create event queue");
        return;
    }
    
    /* Install GPIO ISR service if not already installed */
    esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(RAIN_TAG, "Failed to install ISR service: %s", esp_err_to_name(isr_ret));
        return;
    }
    
    /* Don't add ISR handler yet - will be added when network connects */
    ESP_LOGI(RAIN_TAG, "Rain gauge GPIO configured, ISR will be enabled when connected to network");
    
    /* Create rain gauge task */
    BaseType_t task_ret = xTaskCreate(rain_gauge_task, "rain_gauge_task", 2048, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(RAIN_TAG, "Failed to create rain gauge task");
        return;
    }
    
    /* Initialize reporting variables */
    last_reported_rainfall_mm = total_rainfall_mm;
    last_report_time = xTaskGetTickCount();
    
    ESP_LOGI(RAIN_TAG, "Rain gauge initialized successfully. Current total: %.2f mm", total_rainfall_mm);
    
    /* Report initial value to Zigbee after device joins network */
    esp_zb_scheduler_alarm((esp_zb_callback_t)rain_gauge_zigbee_update, 0, 10000); // 10 seconds delay
    
    /* Start hourly reporting timer */
    esp_zb_scheduler_alarm((esp_zb_callback_t)rain_gauge_hourly_check, 0, 3600000); // First check in 1 hour
    
    /* Add a more frequent debug check initially (every 30 seconds for debugging) */
    esp_zb_scheduler_alarm((esp_zb_callback_t)rain_gauge_hourly_check, 0, 30000); // First debug check in 30 seconds
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
