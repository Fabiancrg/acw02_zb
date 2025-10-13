/*
 * HVAC Driver Implementation
 * 
 * UART communication with ACW02 HVAC device
 */

#include "hvac_driver.h"
#include "esp_log.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

static const char *TAG = "HVAC_DRIVER";

/* Current HVAC state */
static hvac_state_t current_state = {
    .mode = HVAC_MODE_COOL,
    .power_on = false,
    .target_temp_c = 24,
    .ambient_temp_c = 25,
    .eco_mode = false,
    .display_on = true,
    .swing_on = false,
    .fan_speed = HVAC_FAN_AUTO,
    .filter_dirty = false,
    .error = false,
    .error_text = "No Error"
};

/* UART buffer */
static uint8_t rx_buffer[HVAC_UART_BUF_SIZE];
static size_t rx_buffer_len = 0;
static uint32_t last_rx_time = 0;

/* Keepalive frame */
static const uint8_t keepalive_frame[] = {
    0x7A, 0x7A, 0x21, 0xD5, 0x0C, 0x00, 0x00, 0xAB,
    0x0A, 0x0A, 0xFC, 0xF9
};

/* Get status frame */
static const uint8_t get_status_frame[] = {
    0x7A, 0x7A, 0x21, 0xD5, 0x0C, 0x00, 0x00, 0xA2,
    0x0A, 0x0A, 0xFE, 0x29
};

/* Fahrenheit encoding table for temperatures 61-88°F */
static const uint8_t fahrenheit_encoding_table[] = { 
    0x20, 0x21, 0x31, 0x22, 0x32, 0x23, 0x33, 0x24, 0x25,
    0x35, 0x26, 0x36, 0x27, 0x37, 0x28, 0x38, 0x29, 0x2A, 
    0x3A, 0x2B, 0x3B, 0x2C, 0x3C, 0x2D, 0x3D, 0x2E, 0x2F, 
    0x3F
};

/* Forward declarations */
static uint16_t hvac_crc16(const uint8_t *data, size_t len);
static uint8_t hvac_encode_temperature(uint8_t temp_c);
static esp_err_t hvac_send_frame(const uint8_t *data, size_t len);
static esp_err_t hvac_build_and_send_command(void);
static void hvac_decode_state(const uint8_t *frame, size_t len);
static void hvac_rx_task(void *arg);

/**
 * @brief Calculate CRC16 for HVAC frames
 */
static uint16_t hvac_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief Encode temperature to HVAC format
 */
static uint8_t hvac_encode_temperature(uint8_t temp_c)
{
    // Clamp temperature to valid range
    if (temp_c < 16) temp_c = 16;
    if (temp_c > 31) temp_c = 31;
    
    // Convert Celsius to Fahrenheit: F = C * 9/5 + 32
    uint8_t temp_f = (temp_c * 9 / 5) + 32;
    
    // Clamp to valid Fahrenheit range (61-88°F)
    if (temp_f < 61) temp_f = 61;
    if (temp_f > 88) temp_f = 88;
    
    // Look up encoding in table
    int index = temp_f - 61;
    if (index < 0 || index >= sizeof(fahrenheit_encoding_table)) {
        return fahrenheit_encoding_table[17]; // Default to ~78°F (25°C)
    }
    
    return fahrenheit_encoding_table[index];
}

/**
 * @brief Build HVAC command frame
 */
static esp_err_t hvac_build_and_send_command(void)
{
    uint8_t frame[28] = {0};
    
    // Frame header
    frame[0] = 0x7A;
    frame[1] = 0x7A;
    frame[2] = 0x21;
    frame[3] = 0xD5;
    frame[4] = 0x1C;  // Frame length indicator for 28-byte command frame
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0xA3;  // Command type
    
    // Power and mode
    if (current_state.power_on) {
        frame[8] = (uint8_t)current_state.mode;
    } else {
        frame[8] = 0x00;  // Power off
    }
    
    // Temperature encoding
    frame[9] = hvac_encode_temperature(current_state.target_temp_c);
    
    // Fan speed
    frame[10] = (uint8_t)current_state.fan_speed;
    
    // Swing position
    if (current_state.swing_on) {
        frame[11] = (uint8_t)HVAC_SWING_AUTO;
    } else {
        frame[11] = (uint8_t)HVAC_SWING_STOP;
    }
    
    // Options byte (eco, display, etc.)
    uint8_t options = 0x00;
    if (current_state.eco_mode) options |= 0x01;
    if (current_state.display_on) options |= 0x02;
    frame[12] = options;
    
    // Fill remaining bytes with default values
    for (int i = 13; i < 26; i++) {
        frame[i] = 0x0A;
    }
    
    // Calculate and append CRC
    uint16_t crc = hvac_crc16(frame, 26);
    frame[26] = (crc >> 8) & 0xFF;
    frame[27] = crc & 0xFF;
    
    return hvac_send_frame(frame, sizeof(frame));
}

/**
 * @brief Send frame via UART
 */
static esp_err_t hvac_send_frame(const uint8_t *data, size_t len)
{
    int written = uart_write_bytes(HVAC_UART_NUM, data, len);
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write to UART");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "TX [%d bytes]: %02X %02X %02X %02X %02X %02X %02X %02X...", 
             len, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
    
    return ESP_OK;
}

/**
 * @brief Decode received HVAC state frame
 */
static void hvac_decode_state(const uint8_t *frame, size_t len)
{
    if (len < 13) {
        ESP_LOGW(TAG, "Frame too short: %d bytes", len);
        return;
    }
    
    // Verify header
    if (frame[0] != 0x7A || frame[1] != 0x7A) {
        ESP_LOGW(TAG, "Invalid frame header");
        return;
    }
    
    // Verify CRC
    uint16_t expected_crc = (frame[len - 2] << 8) | frame[len - 1];
    uint16_t computed_crc = hvac_crc16(frame, len - 2);
    
    if (expected_crc != computed_crc) {
        ESP_LOGW(TAG, "CRC mismatch: expected 0x%04X, got 0x%04X", expected_crc, computed_crc);
        return;
    }
    
    ESP_LOGI(TAG, "RX [%d bytes]: Valid frame received", len);
    
    // Parse state information from longer frames (18, 28, or 34 bytes)
    if (len >= 18) {
        // Power state and mode
        uint8_t mode_byte = frame[8];
        if (mode_byte == 0x00) {
            current_state.power_on = false;
        } else {
            current_state.power_on = true;
            current_state.mode = (hvac_mode_t)mode_byte;
        }
        
        // Temperature (if available in response)
        if (len >= 28) {
            // Ambient temperature might be in frame[13] or similar
            // This depends on the actual HVAC protocol response format
            // For now, we'll extract what we can
            current_state.ambient_temp_c = 25; // Placeholder
        }
        
        ESP_LOGI(TAG, "Decoded state: Power=%s, Mode=%d, Temp=%d°C", 
                 current_state.power_on ? "ON" : "OFF",
                 current_state.mode,
                 current_state.target_temp_c);
    }
}

/**
 * @brief UART receive task
 */
static void hvac_rx_task(void *arg)
{
    static const size_t VALID_SIZES[] = {13, 18, 28, 34};
    static const size_t NUM_VALID_SIZES = sizeof(VALID_SIZES) / sizeof(VALID_SIZES[0]);
    
    while (1) {
        int len = uart_read_bytes(HVAC_UART_NUM, rx_buffer + rx_buffer_len, 
                                  HVAC_UART_BUF_SIZE - rx_buffer_len, 20 / portTICK_PERIOD_MS);
        
        if (len > 0) {
            rx_buffer_len += len;
            last_rx_time = xTaskGetTickCount();
        }
        
        // Process buffer if we have data and silence period
        if (rx_buffer_len > 0 && (xTaskGetTickCount() - last_rx_time) > pdMS_TO_TICKS(10)) {
            size_t offset = 0;
            
            while (offset < rx_buffer_len && rx_buffer_len - offset >= 13) {
                bool found = false;
                
                for (size_t i = 0; i < NUM_VALID_SIZES; i++) {
                    size_t frame_size = VALID_SIZES[i];
                    
                    if (offset + frame_size > rx_buffer_len) {
                        continue;
                    }
                    
                    // Check for valid frame header
                    if (rx_buffer[offset] == 0x7A && rx_buffer[offset + 1] == 0x7A) {
                        // Verify CRC
                        uint16_t expected_crc = (rx_buffer[offset + frame_size - 2] << 8) | 
                                                rx_buffer[offset + frame_size - 1];
                        uint16_t computed_crc = hvac_crc16(&rx_buffer[offset], frame_size - 2);
                        
                        if (expected_crc == computed_crc) {
                            // Valid frame found
                            hvac_decode_state(&rx_buffer[offset], frame_size);
                            offset += frame_size;
                            found = true;
                            break;
                        }
                    }
                }
                
                if (!found) {
                    offset++;
                }
            }
            
            // Remove processed bytes
            if (offset > 0) {
                memmove(rx_buffer, rx_buffer + offset, rx_buffer_len - offset);
                rx_buffer_len -= offset;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Initialize HVAC driver
 */
esp_err_t hvac_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing HVAC driver");
    
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = HVAC_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    esp_err_t ret = uart_param_config(HVAC_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters");
        return ret;
    }
    
    ret = uart_set_pin(HVAC_UART_NUM, HVAC_UART_TX_PIN, HVAC_UART_RX_PIN, 
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
        return ret;
    }
    
    ret = uart_driver_install(HVAC_UART_NUM, HVAC_UART_BUF_SIZE * 2, 
                             HVAC_UART_BUF_SIZE * 2, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver");
        return ret;
    }
    
    // Create RX task
    BaseType_t task_ret = xTaskCreate(hvac_rx_task, "hvac_rx", 3072, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create RX task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "HVAC driver initialized successfully");
    
    // Send initial keepalive
    vTaskDelay(pdMS_TO_TICKS(100));
    hvac_send_keepalive();
    
    return ESP_OK;
}

/**
 * @brief Get current HVAC state
 */
esp_err_t hvac_get_state(hvac_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(state, &current_state, sizeof(hvac_state_t));
    return ESP_OK;
}

/**
 * @brief Set HVAC power
 */
esp_err_t hvac_set_power(bool power_on)
{
    ESP_LOGI(TAG, "Setting power: %s", power_on ? "ON" : "OFF");
    current_state.power_on = power_on;
    return hvac_build_and_send_command();
}

/**
 * @brief Set HVAC mode
 */
esp_err_t hvac_set_mode(hvac_mode_t mode)
{
    ESP_LOGI(TAG, "Setting mode: %d", mode);
    current_state.mode = mode;
    if (mode != HVAC_MODE_OFF) {
        current_state.power_on = true;
    }
    return hvac_build_and_send_command();
}

/**
 * @brief Set target temperature
 */
esp_err_t hvac_set_temperature(uint8_t temp_c)
{
    if (temp_c < 16 || temp_c > 31) {
        ESP_LOGW(TAG, "Temperature out of range: %d°C (valid: 16-31)", temp_c);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Setting temperature: %d°C", temp_c);
    current_state.target_temp_c = temp_c;
    return hvac_build_and_send_command();
}

/**
 * @brief Set eco mode
 */
esp_err_t hvac_set_eco_mode(bool eco_on)
{
    ESP_LOGI(TAG, "Setting eco mode: %s", eco_on ? "ON" : "OFF");
    current_state.eco_mode = eco_on;
    
    // Eco mode only works in COOL mode
    if (eco_on && current_state.mode != HVAC_MODE_COOL) {
        ESP_LOGW(TAG, "Eco mode only available in COOL mode");
        return ESP_ERR_INVALID_STATE;
    }
    
    return hvac_build_and_send_command();
}

/**
 * @brief Set display state
 */
esp_err_t hvac_set_display(bool display_on)
{
    ESP_LOGI(TAG, "Setting display: %s", display_on ? "ON" : "OFF");
    current_state.display_on = display_on;
    return hvac_build_and_send_command();
}

/**
 * @brief Set swing mode
 */
esp_err_t hvac_set_swing(bool swing_on)
{
    ESP_LOGI(TAG, "Setting swing: %s", swing_on ? "ON" : "OFF");
    current_state.swing_on = swing_on;
    return hvac_build_and_send_command();
}

/**
 * @brief Set fan speed
 */
esp_err_t hvac_set_fan_speed(hvac_fan_t fan)
{
    ESP_LOGI(TAG, "Setting fan speed: %d", fan);
    current_state.fan_speed = fan;
    
    // In eco mode, fan is forced to AUTO
    if (current_state.eco_mode) {
        ESP_LOGW(TAG, "Fan speed ignored in eco mode (forced to AUTO)");
        current_state.fan_speed = HVAC_FAN_AUTO;
    }
    
    return hvac_build_and_send_command();
}

/**
 * @brief Request status from HVAC
 */
esp_err_t hvac_request_status(void)
{
    ESP_LOGI(TAG, "Requesting HVAC status");
    return hvac_send_frame(get_status_frame, sizeof(get_status_frame));
}

/**
 * @brief Send keepalive frame
 */
esp_err_t hvac_send_keepalive(void)
{
    ESP_LOGD(TAG, "Sending keepalive");
    return hvac_send_frame(keepalive_frame, sizeof(keepalive_frame));
}
