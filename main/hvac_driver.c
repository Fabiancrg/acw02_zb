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
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "HVAC_DRIVER";
static const char *NVS_NAMESPACE = "hvac_storage";

/* Error code mapping structure */
typedef struct {
    uint8_t code_high;  // High byte (ASCII character like 'E', 'P', 'U', etc.)
    uint8_t code_low;   // Low byte (ASCII digit like '0', '1', etc.)
    const char *description;
} error_code_entry_t;

/* Error code lookup table from AIRTON-LIST-ERROR-CODE.EN.pdf */
static const error_code_entry_t error_code_table[] = {
    {'C', 'L', "Filter cleaning reminder"},
    {'D', '0', "Compressor RMS phase current limit"},
    {'D', '1', "Low RMS machine current limit"},
    {'D', '2', "Lower gas discharge temperature limit"},
    {'D', '3', "Extreme anti-freeze limit"},
    {'D', '4', "Overload limit"},
    {'D', '5', "IPM power module temperature limit"},
    {'E', '0', "Protection against high discharge temperatures"},
    {'E', '1', "Overload protection"},
    {'E', '2', "Compressor overload protection"},
    {'E', '3', "Frost protection"},
    {'E', '7', "4-way valve malfunction"},
    {'E', '8', "Abnormal outdoor ambient temperature"},
    {'H', '0', "Compressor stalling or jamming"},
    {'H', '1', "Startup failure"},
    {'H', '2', "Compressor phase current peak protection"},
    {'H', '3', "Compressor phase current RMS protection"},
    {'H', '4', "IPM power module protection"},
    {'H', '5', "IPM overheat protection"},
    {'H', '6', "Compressor circuit phase detection error"},
    {'H', '7', "Compressor phase loss error"},
    {'H', '8', "Outdoor unit fan motor error"},
    {'H', '9', "Outdoor unit fan motor phase current detection circuit error"},
    {'L', '0', "Jumper error"},
    {'L', '1', "Indoor fan motor zero crossing detection circuit malfunction"},
    {'L', '2', "Indoor fan motor error"},
    {'L', '3', "Communication fault between indoor and outdoor unit"},
    {'L', '4', "Port selection error"},
    {'L', '5', "EEPROM error on indoor unit"},
    {'L', '6', "Communication fault between outdoor and indoor unit"},
    {'L', 'L', "Function test"},
    {'P', '0', "EEPROM error on outdoor unit"},
    {'P', '1', "Power on error"},
    {'P', '2', "AC current protection"},
    {'P', '3', "High voltage protection"},
    {'P', '4', "Low voltage protection"},
    {'P', '5', "DC DC line voltage drop protection"},
    {'P', '6', "Current detection circuit error"},
    {'P', '7', "Overcurrent protection"},
    {'P', '8', "PFC current detection circuit error"},
    {'P', '9', "PFC protection"},
    {'P', 'A', "IU and EU mismatch"},
    {'P', 'C', "Fashion Conflict"},
    {'U', '0', "Ambient temperature sensor (probe) open/closed circuit"},
    {'U', '1', "Pipe temperature sensor (probe) open/closed circuit"},
    {'U', '2', "Ambient temperature sensor (probe) open/closed circuit"},
    {'U', '3', "UE Discharge Sensor (Probe) Open/Closed Circuit"},
    {'U', '4', "UE pipe temperature sensor open/closed circuit (probe)"},
    {'U', '5', "IPM power module temperature sensor open/closed circuit"},
    {'U', '6', "Liquid pipe outlet temperature sensor open/closed circuit"},
    {'U', '7', "Gas pipe outlet temperature sensor open/closed circuit"},
    {'U', '8', "Discharge temperature sensor open/closed circuit"},
};

#define ERROR_CODE_TABLE_SIZE (sizeof(error_code_table) / sizeof(error_code_entry_t))

/* Decode error code to human-readable text */
static const char* hvac_decode_error_code(uint8_t code) {
    // Handle special case for filter cleaning (0x80)
    if (code == 0x80) {
        return "Filter cleaning reminder (CL)";
    }
    
    // Handle standard two-byte error codes
    // The code format appears to be: high nibble = letter, low nibble = digit
    // For example: E1 might be encoded differently
    
    // Try to decode assuming ASCII-like encoding
    uint8_t code_high = (code >> 4) & 0x0F;
    uint8_t code_low = code & 0x0F;
    
    // Convert nibbles to ASCII characters
    char high_char = 0, low_char = 0;
    
    // Map high nibble to letter (rough approximation - may need adjustment based on actual protocol)
    if (code_high >= 0x0C && code_high <= 0x0F) {
        high_char = 'C' + (code_high - 0x0C);  // C, D, E, F
    } else if (code_high >= 0x08 && code_high <= 0x0B) {
        high_char = 'H' + (code_high - 0x08);  // H, I, J, K
    } else if (code_high >= 0x04 && code_high <= 0x07) {
        high_char = 'L' + (code_high - 0x04);  // L, M, N, O
    } else if (code_high <= 0x03) {  // 0x00-0x03
        high_char = 'P' + (code_high - 0x00);  // P, Q, R, S
    }
    
    // Map low nibble to digit or letter
    if (code_low <= 9) {
        low_char = '0' + code_low;
    } else {
        low_char = 'A' + (code_low - 10);  // A=10, B=11, C=12, etc.
    }
    
    // Search in lookup table
    for (size_t i = 0; i < ERROR_CODE_TABLE_SIZE; i++) {
        if (error_code_table[i].code_high == high_char && 
            error_code_table[i].code_low == low_char) {
            return error_code_table[i].description;
        }
    }
    
    // If not found in table, return unknown with hex code
    static char unknown_buffer[32];
    snprintf(unknown_buffer, sizeof(unknown_buffer), "Unknown error code 0x%02X", code);
    return unknown_buffer;
}

/* Current HVAC state */
static hvac_state_t current_state = {
    .mode = HVAC_MODE_COOL,
    .power_on = false,
    .target_temp_c = 24,
    .ambient_temp_c = 25,
    .eco_mode = false,
    .night_mode = false,
    .display_on = true,
    .swing_on = false,
    .purifier_on = false,
    .clean_status = false,
    .mute_on = false,
    .fan_speed = HVAC_FAN_AUTO,
    .filter_dirty = false,
    .error = false,
    .error_text = ""  // Empty string when no error
};

/* UART buffer */
static uint8_t rx_buffer[HVAC_UART_BUF_SIZE];
static size_t rx_buffer_len = 0;
static uint32_t last_rx_time = 0;

/* Delayed NVS write to reduce flash wear */
static TimerHandle_t nvs_save_timer = NULL;
static bool nvs_save_pending = false;
#define NVS_SAVE_DELAY_MS  900000  // 900 seconds - write to flash once per 15 minutes max

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
// static const uint8_t fahrenheit_encoding_table[] = { 
//     0x20, 0x21, 0x31, 0x22, 0x32, 0x23, 0x33, 0x24, 0x25,
//     0x35, 0x26, 0x36, 0x27, 0x37, 0x28, 0x38, 0x29, 0x2A, 
//     0x3A, 0x2B, 0x3B, 0x2C, 0x3C, 0x2D, 0x3D, 0x2E, 0x2F, 
//     0x3F
// };

/* Forward declarations */
static uint16_t hvac_crc16(const uint8_t *data, size_t len);
static uint8_t hvac_encode_temperature(uint8_t temp_c);
static esp_err_t hvac_send_frame(const uint8_t *data, size_t len);
static esp_err_t hvac_build_and_send_command(void);
static void hvac_decode_state(const uint8_t *frame, size_t len);
static void hvac_rx_task(void *arg);
static esp_err_t hvac_save_settings_immediate(void);  // Actual NVS write
static void nvs_save_timer_callback(TimerHandle_t xTimer);  // Delayed write callback

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
 * 
 * IMPORTANT: The ACW02 appears to expect Celsius values directly, not Fahrenheit!
 * The encoding table was misleading - we should just send the Celsius value.
 */
static uint8_t hvac_encode_temperature(uint8_t temp_c)
{
    // Clamp temperature to valid range
    if (temp_c < 16) temp_c = 16;
    if (temp_c > 31) temp_c = 31;
    
    // Send Celsius value directly
    // The ACW02 protocol expects the temperature in Celsius
    return temp_c;
}

/**
 * @brief Build HVAC command frame
 * 
 * ACW02 Protocol Frame Structure (24 bytes):
 * [0-1]  Header: 0x7A 0x7A
 * [2-7]  Header: 0x21 0xD5 0x18 0x00 0x00 0xA1
 * [8-11] Reserved: 0x00
 * [12]   Power/Mode/Fan: (fan<<4) | (power<<3) | mode
 * [13]   Temperature: encoded value (+ 0x40 for SILENT fan)
 * [14]   Swing: (horizontal<<4) | vertical
 * [15]   Options: eco(0x01) | night(0x02) | clean(0x10) | purifier(0x40) | display(0x80)
 * [16]   Mute: 0x01 if muted
 * [17-21] Reserved: 0x00
 * [22-23] CRC16: MSB, LSB (computed over first 22 bytes)
 */
static esp_err_t hvac_build_and_send_command(void)
{
    uint8_t frame[24] = {0};
    
    // Frame header (bytes 0-7)
    frame[0] = 0x7A;
    frame[1] = 0x7A;
    frame[2] = 0x21;
    frame[3] = 0xD5;
    frame[4] = 0x18;  // Frame length indicator for 24-byte command frame
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0xA1;  // Command type for control frame
    
    // Bytes 8-11 are reserved (already zeroed)
    
    // Byte 12: Pack fan (4 bits), power (1 bit), mode (3 bits)
    uint8_t fan_nibble = ((uint8_t)current_state.fan_speed & 0x0F) << 4;
    uint8_t power_bit = (current_state.power_on ? 1 : 0) << 3;
    uint8_t mode_bits = (uint8_t)current_state.mode & 0x07;
    frame[12] = fan_nibble | power_bit | mode_bits;
    
    // Byte 13: Temperature encoding (with SILENT bit if needed)
    uint8_t temp_base = hvac_encode_temperature(current_state.target_temp_c);
    if (current_state.fan_speed == HVAC_FAN_SILENT) {
        frame[13] = temp_base + 0x40;  // Add SILENT bit
    } else {
        frame[13] = temp_base;
    }
    
    // Byte 14: Swing (horizontal in upper nibble, vertical in lower nibble)
    uint8_t swing_v = current_state.swing_on ? 0x07 : 0x00;  // 0x07 = auto swing
    uint8_t swing_h = 0x00;  // Horizontal swing (not used for now)
    frame[14] = (swing_h << 4) | swing_v;
    
    // Byte 15: Options byte
    uint8_t options = 0x00;
    if (current_state.eco_mode) options |= 0x01;      // Bit 0: ECO mode
    if (current_state.night_mode) options |= 0x02;    // Bit 1: NIGHT mode
    // clean mode: bit 0x10 (READ from AC, not sent TO AC)
    if (current_state.purifier_on) options |= 0x40;   // Bit 6: PURIFIER mode
    if (current_state.display_on) options |= 0x80;    // Bit 7: DISPLAY on/off
    frame[15] = options;
    
    // Byte 16: Mute
    frame[16] = current_state.mute_on ? 0x01 : 0x00;  // Bit 0: MUTE (silent command)
    
    // Bytes 17-21 are reserved (already zeroed)
    
    // Calculate CRC over first 22 bytes
    uint16_t crc = hvac_crc16(frame, 22);
    frame[22] = (crc >> 8) & 0xFF;  // CRC MSB
    frame[23] = crc & 0xFF;          // CRC LSB
    
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
    
    // Log full frame in hex for debugging
    char hex_str[256] = {0};
    char *ptr = hex_str;
    for (size_t i = 0; i < len && i < 64; i++) {
        ptr += sprintf(ptr, "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "TX [%d bytes]: %s", len, hex_str);
    
    return ESP_OK;
}

/**
 * @brief Decode received HVAC state frame
 * 
 * ACW02 responds with 34-byte status frames:
 * [0-1]  Header: 0x7A 0x7A
 * [2-3]  Type marker: 0xD5 0x21 (status response)
 * [10-11] Ambient temperature: integer, decimal
 * [13]   Power/Mode/Fan: (fan<<4) | (power<<3) | mode
 * [14]   Temperature: encoded value (bit 0x40 indicates SILENT fan)
 * [15]   Swing: (horizontal<<4) | vertical
 * [16]   Options: eco(0x01) | night(0x02) | from_remote(0x04) | display(0x08) | clean(0x10) | purifier(0x40) | display(0x80)
 * [32-33] CRC16
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
    
    // Log full frame in hex for debugging
    char hex_str[256] = {0};
    char *ptr = hex_str;
    for (size_t i = 0; i < len && i < 64; i++) {
        ptr += sprintf(ptr, "%02X ", frame[i]);
    }
    ESP_LOGI(TAG, "RX [%d bytes]: %s", len, hex_str);
    
    // Verify CRC
    uint16_t expected_crc = (frame[len - 2] << 8) | frame[len - 1];
    uint16_t computed_crc = hvac_crc16(frame, len - 2);
    
    if (expected_crc != computed_crc) {
        ESP_LOGW(TAG, "CRC mismatch: expected 0x%04X, got 0x%04X", expected_crc, computed_crc);
        return;
    }
    
    ESP_LOGI(TAG, "RX [%d bytes]: Valid frame received", len);
    
    // Handle 13-byte ACK frames from AC (acknowledgment of commands)
    // Frame structure: 7A 7A D1 21 0D 00 00 A4 0A 0A 00 CRC CRC
    if (len == 13 && frame[0] == 0x7A && frame[1] == 0x7A && frame[2] == 0xD1 && frame[3] == 0x21) {
        ESP_LOGD(TAG, "ACK frame received from AC (13 bytes)");
        // This is just an acknowledgment, no state to decode
        return;
    }
    
    // Handle 18-byte frames (if they exist)
    if (len == 18 && frame[0] == 0x7A && frame[1] == 0x7A) {
        ESP_LOGD(TAG, "18-byte frame received (keepalive/other)");
        // Handle if needed in the future
        return;
    }
    
    // Handle 28-byte warning/error frames
    if (len == 28 && frame[0] == 0x7A && frame[1] == 0x7A && frame[2] == 0xD5 && frame[3] == 0x21) {
        uint8_t warn = frame[10];
        uint8_t fault = frame[12];
        
        if (fault != 0x00) {
            // We only know that 0x04 = PC (Fashion Conflict)
            // All other fault codes are unknown
            current_state.error = true;
            if (fault == 0x04) {
                ESP_LOGE(TAG, "AC FAULT: code=0x%02X - PC: Fashion Conflict", fault);
                snprintf(current_state.error_text, sizeof(current_state.error_text), 
                         "FAULT 0x%02X: PC - Fashion Conflict", fault);
            } else {
                ESP_LOGE(TAG, "AC FAULT: code=0x%02X - Unknown error", fault);
                snprintf(current_state.error_text, sizeof(current_state.error_text), 
                         "Error, check error code on the display");
            }
        } else if (warn != 0x00) {
            const char *warn_desc = hvac_decode_error_code(warn);
            ESP_LOGW(TAG, "AC WARNING: code=0x%02X - %s", warn, warn_desc);
            snprintf(current_state.error_text, sizeof(current_state.error_text), 
                     "WARNING 0x%02X: %s", warn, warn_desc);
            if (warn == 0x80) {
                current_state.filter_dirty = true;
            }
        } else {
            current_state.filter_dirty = false;
            current_state.error = false;
            current_state.error_text[0] = '\0';  // Empty string when no error
        }
        return;
    }
    
    // Parse 34-byte status frames
    if (len != 34) {
        ESP_LOGW(TAG, "Unexpected frame length (expected 34 bytes, got %d)", len);
        return;
    }
    
    ESP_LOGI(TAG, "Parsing 34-byte status frame...");
    
    // Parse ambient temperature (bytes 10-11)
    uint8_t temp_int = frame[10];
    uint8_t temp_dec = frame[11];
    
    // Calculate with explicit types to avoid truncation
    current_state.ambient_temp_c = (float)temp_int + ((float)temp_dec / 10.0f);
    
    // Byte 13: Power, Mode, Fan
    uint8_t b13 = frame[13];
    current_state.power_on = (b13 & 0x08) != 0;
    current_state.mode = (hvac_mode_t)(b13 & 0x07);
    current_state.fan_speed = (hvac_fan_t)((b13 >> 4) & 0x0F);
    
    // Byte 14: Temperature (with SILENT bit)
    uint8_t temp_byte = frame[14];
    bool silent_bit = (temp_byte & 0x40) != 0;
    temp_byte &= 0x3F;  // Remove SILENT bit
    
    // Temperature is in Celsius (16-31°C range)
    // The ACW02 sends temperature values directly in Celsius
    if (temp_byte >= 16 && temp_byte <= 31) {
        current_state.target_temp_c = temp_byte;
    } else {
        // If out of expected range, clamp to valid range
        ESP_LOGW(TAG, "Unexpected temperature value: %d, clamping to range", temp_byte);
        current_state.target_temp_c = (temp_byte < 16) ? 16 : 31;
    }
    
    // Override fan if SILENT bit is set
    if (silent_bit) {
        current_state.fan_speed = HVAC_FAN_SILENT;
    }
    
    // Byte 15: Swing
    uint8_t swing_raw = frame[15];
    uint8_t swing_v = swing_raw & 0x0F;
    current_state.swing_on = (swing_v != 0);
    
    // Byte 16: Options
    uint8_t flags = frame[16];
    current_state.eco_mode = (flags & 0x01) != 0;       // Bit 0: ECO mode
    current_state.night_mode = (flags & 0x02) != 0;     // Bit 1: NIGHT mode
    current_state.clean_status = (flags & 0x10) != 0;   // Bit 4: CLEAN status (from AC)
    current_state.purifier_on = (flags & 0x40) != 0;    // Bit 6: PURIFIER mode
    current_state.display_on = (flags & 0x80) != 0;     // Bit 7: DISPLAY on/off
    
    ESP_LOGI(TAG, "Decoded state: Power=%s, Mode=%d, Fan=0x%02X, Temp=%d°C, Ambient=%.1f°C", 
             current_state.power_on ? "ON" : "OFF",
             current_state.mode,
             current_state.fan_speed,
             current_state.target_temp_c,
             current_state.ambient_temp_c);
    ESP_LOGI(TAG, "  Options: Eco=%s, Night=%s, Display=%s, Purifier=%s, Clean=%s, Swing=%s", 
             current_state.eco_mode ? "ON" : "OFF",
             current_state.night_mode ? "ON" : "OFF",
             current_state.display_on ? "ON" : "OFF",
             current_state.purifier_on ? "ON" : "OFF",
             current_state.clean_status ? "YES" : "NO",
             current_state.swing_on ? "ON" : "OFF");
}

/**
 * @brief UART receive task
 */
static void hvac_rx_task(void *arg)
{
    static const size_t VALID_SIZES[] = {13, 18, 28, 34};
    static const size_t NUM_VALID_SIZES = sizeof(VALID_SIZES) / sizeof(VALID_SIZES[0]);
    
    while (1) {
        // Safety check: ensure buffer has space
        size_t space_available = HVAC_UART_BUF_SIZE - rx_buffer_len;
        if (space_available < 34) {  // Need at least space for largest frame
            ESP_LOGW(TAG, "HVAC RX buffer nearly full, resetting");
            rx_buffer_len = 0;
            space_available = HVAC_UART_BUF_SIZE;
        }
        
        int len = uart_read_bytes(HVAC_UART_NUM, rx_buffer + rx_buffer_len, 
                                  space_available, 20 / portTICK_PERIOD_MS);
        
        if (len > 0) {
            rx_buffer_len += len;
            last_rx_time = xTaskGetTickCount();
        } else if (len < 0) {
            // UART error occurred
            ESP_LOGE(TAG, "HVAC UART read error: %d", len);
            uart_flush_input(HVAC_UART_NUM);
            rx_buffer_len = 0;
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
            
            // Additional safety: prevent buffer overflow
            if (rx_buffer_len > HVAC_UART_BUF_SIZE - 64) {
                ESP_LOGW(TAG, "HVAC RX buffer unexpectedly full (%zu bytes), resetting", rx_buffer_len);
                rx_buffer_len = 0;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Timer callback for delayed NVS save
 */
static void nvs_save_timer_callback(TimerHandle_t xTimer)
{
    if (nvs_save_pending) {
        ESP_LOGI(TAG, "Performing delayed NVS save...");
        hvac_save_settings_immediate();
        nvs_save_pending = false;
    }
}

/**
 * @brief Save HVAC settings to NVS immediately (internal function)
 */
static esp_err_t hvac_save_settings_immediate(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    // Save all settings
    nvs_set_u8(nvs_handle, "mode", (uint8_t)current_state.mode);
    nvs_set_u8(nvs_handle, "power", current_state.power_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "temp", current_state.target_temp_c);
    nvs_set_u8(nvs_handle, "fan", (uint8_t)current_state.fan_speed);
    nvs_set_u8(nvs_handle, "eco", current_state.eco_mode ? 1 : 0);
    nvs_set_u8(nvs_handle, "night", current_state.night_mode ? 1 : 0);
    nvs_set_u8(nvs_handle, "display", current_state.display_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "swing", current_state.swing_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "purifier", current_state.purifier_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "mute", current_state.mute_on ? 1 : 0);
    
    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Settings saved to NVS");
    }
    
    nvs_close(nvs_handle);
    return err;
}

/**
 * @brief Schedule delayed save to NVS (reduces flash wear)
 * 
 * This function marks settings as pending and starts/resets a timer.
 * The actual write to NVS happens after NVS_SAVE_DELAY_MS milliseconds
 * of no further changes, reducing flash wear significantly.
 */
static esp_err_t hvac_save_settings(void)
{
    // Mark that we have pending changes
    nvs_save_pending = true;
    
    // Create timer on first use
    if (nvs_save_timer == NULL) {
        nvs_save_timer = xTimerCreate(
            "nvs_save",
            pdMS_TO_TICKS(NVS_SAVE_DELAY_MS),
            pdFALSE,  // One-shot timer
            NULL,
            nvs_save_timer_callback
        );
        
        if (nvs_save_timer == NULL) {
            ESP_LOGE(TAG, "Failed to create NVS save timer, saving immediately");
            return hvac_save_settings_immediate();
        }
    }
    
    // Reset timer - this delays the write if settings keep changing
    if (xTimerReset(nvs_save_timer, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "Failed to reset NVS save timer, saving immediately");
        return hvac_save_settings_immediate();
    }
    
    ESP_LOGD(TAG, "NVS save scheduled in %d seconds", NVS_SAVE_DELAY_MS / 1000);
    return ESP_OK;
}

/**
 * @brief Load HVAC settings from NVS
 */
static esp_err_t hvac_load_settings(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved settings found, using defaults");
        } else {
            ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        }
        return err;
    }
    
    // Load all settings
    uint8_t val;
    
    if (nvs_get_u8(nvs_handle, "mode", &val) == ESP_OK) {
        current_state.mode = (hvac_mode_t)val;
    }
    if (nvs_get_u8(nvs_handle, "power", &val) == ESP_OK) {
        current_state.power_on = (val != 0);
    }
    if (nvs_get_u8(nvs_handle, "temp", &val) == ESP_OK) {
        current_state.target_temp_c = val;
    }
    if (nvs_get_u8(nvs_handle, "fan", &val) == ESP_OK) {
        current_state.fan_speed = (hvac_fan_t)val;
    }
    if (nvs_get_u8(nvs_handle, "eco", &val) == ESP_OK) {
        current_state.eco_mode = (val != 0);
    }
    if (nvs_get_u8(nvs_handle, "night", &val) == ESP_OK) {
        current_state.night_mode = (val != 0);
    }
    if (nvs_get_u8(nvs_handle, "display", &val) == ESP_OK) {
        current_state.display_on = (val != 0);
    }
    if (nvs_get_u8(nvs_handle, "swing", &val) == ESP_OK) {
        current_state.swing_on = (val != 0);
    }
    if (nvs_get_u8(nvs_handle, "purifier", &val) == ESP_OK) {
        current_state.purifier_on = (val != 0);
    }
    if (nvs_get_u8(nvs_handle, "mute", &val) == ESP_OK) {
        current_state.mute_on = (val != 0);
    }
    
    ESP_LOGI(TAG, "Settings loaded from NVS: Mode=%d, Power=%d, Temp=%d°C",
             current_state.mode, current_state.power_on, current_state.target_temp_c);
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

/**
 * @brief Initialize HVAC driver
 */
esp_err_t hvac_driver_init(void)
{
    ESP_LOGI(TAG, "[HVAC] Starting HVAC driver initialization");
    
    // Configure UART
    ESP_LOGI(TAG, "[HVAC] Configuring UART%d (TX=%d, RX=%d, baud=%d)", 
             HVAC_UART_NUM, HVAC_UART_TX_PIN, HVAC_UART_RX_PIN, HVAC_UART_BAUD_RATE);
    
    uart_config_t uart_config = {
        .baud_rate = HVAC_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_LOGI(TAG, "[HVAC] Setting UART parameters");
    esp_err_t ret = uart_param_config(HVAC_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Failed to configure UART parameters: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[OK] UART parameters configured");
    
    ESP_LOGI(TAG, "[HVAC] Setting UART pins");
    ret = uart_set_pin(HVAC_UART_NUM, HVAC_UART_TX_PIN, HVAC_UART_RX_PIN, 
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Failed to set UART pins: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[OK] UART pins configured");
    
    ESP_LOGI(TAG, "[HVAC] Installing UART driver");
    ret = uart_driver_install(HVAC_UART_NUM, HVAC_UART_BUF_SIZE * 2, 
                             HVAC_UART_BUF_SIZE * 2, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[ERROR] Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[OK] UART driver installed");
    
    // Create RX task
    ESP_LOGI(TAG, "[HVAC] Creating RX task");
    BaseType_t task_ret = xTaskCreate(hvac_rx_task, "hvac_rx", 3072, NULL, 5, NULL);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "[ERROR] Failed to create RX task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[OK] RX task created");
    
    // Load saved settings from NVS
    ESP_LOGI(TAG, "[HVAC] Loading saved settings from NVS");
    hvac_load_settings();
    
    // Flush any pending NVS saves from previous session (shouldn't be any, but safety)
    nvs_save_pending = false;
    
    ESP_LOGI(TAG, "[OK] HVAC driver initialized successfully");
    
    // Send initial keepalive
    ESP_LOGI(TAG, "[HVAC] Sending initial keepalive");
    vTaskDelay(pdMS_TO_TICKS(100));
    hvac_send_keepalive();
    ESP_LOGI(TAG, "[OK] Initial keepalive sent");
    
    // Apply loaded settings to HVAC
    ESP_LOGI(TAG, "[HVAC] Applying loaded settings to HVAC");
    hvac_build_and_send_command();
    
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
    hvac_save_settings();
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
    hvac_save_settings();
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
    hvac_save_settings();
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
    
    hvac_save_settings();
    return hvac_build_and_send_command();
}

/**
 * @brief Set display state
 */
esp_err_t hvac_set_display(bool display_on)
{
    ESP_LOGI(TAG, "Setting display: %s", display_on ? "ON" : "OFF");
    current_state.display_on = display_on;
    hvac_save_settings();
    return hvac_build_and_send_command();
}

/**
 * @brief Set swing mode
 */
esp_err_t hvac_set_swing(bool swing_on)
{
    ESP_LOGI(TAG, "Setting swing: %s", swing_on ? "ON" : "OFF");
    current_state.swing_on = swing_on;
    hvac_save_settings();
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
    
    hvac_save_settings();
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

/**
 * @brief Set night mode
 */
esp_err_t hvac_set_night_mode(bool night_on)
{
    ESP_LOGI(TAG, "Setting night mode: %s", night_on ? "ON" : "OFF");
    current_state.night_mode = night_on;
    hvac_save_settings();
    return hvac_build_and_send_command();
}

/**
 * @brief Set purifier mode
 */
esp_err_t hvac_set_purifier(bool purifier_on)
{
    ESP_LOGI(TAG, "Setting purifier: %s", purifier_on ? "ON" : "OFF");
    current_state.purifier_on = purifier_on;
    hvac_save_settings();
    return hvac_build_and_send_command();
}

/**
 * @brief Set mute mode
 */
esp_err_t hvac_set_mute(bool mute_on)
{
    ESP_LOGI(TAG, "Setting mute: %s", mute_on ? "ON" : "OFF");
    current_state.mute_on = mute_on;
    hvac_save_settings();
    return hvac_build_and_send_command();
}

/**
 * @brief Get clean status
 */
bool hvac_get_clean_status(void)
{
    return current_state.clean_status;
}
