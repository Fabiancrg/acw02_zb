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
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "HVAC_DRIVER";
static const char *NVS_NAMESPACE = "hvac_storage";

/* State change callback */
static hvac_state_change_callback_t state_change_callback = NULL;

/* Mutex protecting current_state across tasks */
static SemaphoreHandle_t state_mutex = NULL;

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
    .error = false,
    .error_text = ""  // Empty string when no error
};

/* UART buffer */
static uint8_t rx_buffer[HVAC_UART_BUF_SIZE];
static size_t rx_buffer_len = 0;
static TickType_t last_rx_time = 0;

/* Command transmit path (see hvac_tx_service). Timings follow the reference
 * ESPHome implementation (devildant/acw02_esphome). */
#define HVAC_CMD_FRAME_LEN       24
#define HVAC_TX_INTERVAL_MS      180    // minimum gap between two transmitted frames
#define HVAC_RX_SILENCE_MS       120    // line must be quiet this long before we transmit
#define HVAC_ACK_EVAL_MIN_MS     50     // a status frame sooner than this cannot be a reply
#define HVAC_ACK_RETRY_MS        300    // unconfirmed after this long -> re-send
#define HVAC_ACK_ABORT_MS        1200   // after the last retry, give up after this long
#define HVAC_CMD_MAX_RETRY       3
#define HVAC_RX_STALE_MS         500    // drop partial RX data older than this

typedef struct {
    bool active;
    uint8_t frame[HVAC_CMD_FRAME_LEN];
    hvac_state_t expected;      // state the AC should report once it accepted the frame
    uint8_t tries;              // retries already sent
    TickType_t sent_at;
} hvac_cmd_t;

/* All of the following are protected by state_mutex */
static hvac_cmd_t pending_cmd = {0};     // built, waiting for an idle line (latest wins)
static hvac_cmd_t inflight_cmd = {0};    // transmitted, waiting for the AC to confirm
static bool keepalive_pending = false;
static bool status_pending = false;
static bool force_notify = false;        // push state to Zigbee even if unchanged
static TickType_t last_tx_time = 0;

/* Protocol details the Zigbee layer does not see */
static uint8_t swing_raw = 0x00;         // swing byte as last reported: (horizontal<<4) | vertical
static bool use_fahrenheit = false;      // AC encodes its setpoint in Fahrenheit
static bool filter_dirty = false;        // "CL" warning from a 28-byte frame
static bool self_clean_active = false;   // self-clean bit from a status frame

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

/* Forward declarations */
static uint16_t hvac_crc16(const uint8_t *data, size_t len);
static uint8_t hvac_encode_temperature(uint8_t temp_c);
static esp_err_t hvac_send_frame(const uint8_t *data, size_t len);
static void hvac_queue_command_locked(void);
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

/* Fahrenheit encoding table for setpoints 61-88 F (index = F - 61). The AC
 * uses this encoding instead of "Celsius - 16" when its display unit is F. */
static const uint8_t fahrenheit_encoding_table[28] = {
    0x20, 0x21, 0x31, 0x22, 0x32, 0x23, 0x33, 0x24, 0x25,
    0x35, 0x26, 0x36, 0x27, 0x37, 0x28, 0x38, 0x29, 0x2A,
    0x3A, 0x2B, 0x3B, 0x2C, 0x3C, 0x2D, 0x3D, 0x2E, 0x2F,
    0x3F
};

/**
 * @brief Decode a Fahrenheit-encoded temperature byte
 *
 * @return the setpoint in F (61-88), or 0 if the byte is not Fahrenheit-encoded
 */
static uint8_t hvac_fahrenheit_from_byte(uint8_t temp_byte)
{
    for (size_t i = 0; i < sizeof(fahrenheit_encoding_table); i++) {
        if (fahrenheit_encoding_table[i] == temp_byte) {
            return (uint8_t)(61 + i);
        }
    }
    return 0;
}

/**
 * @brief Encode temperature to HVAC format
 *
 * Celsius (default): frame byte = temp_c - 16 (16-31 C -> 0x00-0x0F).
 * Fahrenheit (when the AC reports its setpoint in F): table lookup, so the
 * unit the user selected on the AC is preserved.
 */
static uint8_t hvac_encode_temperature(uint8_t temp_c)
{
    // Clamp temperature to valid range (16-31°C)
    if (temp_c < 16) temp_c = 16;
    if (temp_c > 31) temp_c = 31;

    if (use_fahrenheit) {
        int temp_f = (temp_c * 9 + 2) / 5 + 32;  // rounded C -> F
        if (temp_f < 61) temp_f = 61;
        if (temp_f > 88) temp_f = 88;
        return fahrenheit_encoding_table[temp_f - 61];
    }

    return temp_c - 16;
}

/**
 * @brief Build an HVAC command frame from a state
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
static void hvac_build_frame(const hvac_state_t *st, uint8_t frame[HVAC_CMD_FRAME_LEN])
{
    memset(frame, 0, HVAC_CMD_FRAME_LEN);

    // Frame header (bytes 0-7)
    frame[0] = 0x7A;
    frame[1] = 0x7A;
    frame[2] = 0x21;
    frame[3] = 0xD5;
    frame[4] = 0x18;  // Frame length indicator for 24-byte command frame
    frame[5] = 0x00;
    frame[6] = 0x00;
    frame[7] = 0xA1;  // Command type for control frame

    // Byte 12: Pack fan (4 bits), power (1 bit), mode (3 bits)
    uint8_t fan_nibble = ((uint8_t)st->fan_speed & 0x0F) << 4;
    uint8_t power_bit = (st->power_on ? 1 : 0) << 3;
    uint8_t mode_bits = (uint8_t)st->mode & 0x07;
    frame[12] = fan_nibble | power_bit | mode_bits;

    // Byte 13: Temperature encoding (with SILENT bit if needed)
    uint8_t temp_base = hvac_encode_temperature(st->target_temp_c);
    frame[13] = (st->fan_speed == HVAC_FAN_SILENT) ? (temp_base + 0x40) : temp_base;

    // Byte 14: Swing. Sent as last reported by the AC (horizontal nibble and
    // any fixed vertical position are preserved); hvac_set_swing() only
    // switches the vertical nibble between STOP (0x0) and SWING (0x1).
    frame[14] = swing_raw;

    // Byte 15: Options byte
    uint8_t options = 0x00;
    if (st->eco_mode) options |= 0x01;      // Bit 0: ECO mode
    if (st->night_mode) options |= 0x02;    // Bit 1: NIGHT mode
    // clean mode: bit 0x10 (READ from AC, not sent TO AC)
    if (st->purifier_on) options |= 0x40;   // Bit 6: PURIFIER mode
    if (st->display_on) options |= 0x80;    // Bit 7: DISPLAY on/off
    frame[15] = options;

    // Byte 16: Mute
    frame[16] = st->mute_on ? 0x01 : 0x00;  // Bit 0: MUTE (silent command)

    // Calculate CRC over first 22 bytes
    uint16_t crc = hvac_crc16(frame, 22);
    frame[22] = (crc >> 8) & 0xFF;  // CRC MSB
    frame[23] = crc & 0xFF;          // CRC LSB
}

/**
 * @brief Queue a command built from current_state (caller holds state_mutex)
 *
 * The frame is snapshotted now and transmitted by the HVAC task once the line
 * is idle. Only the latest command is kept: every frame carries the full
 * state, so a newer one supersedes anything still waiting or unconfirmed.
 * Until the AC confirms it, incoming status frames are not allowed to
 * overwrite current_state (see hvac_decode_state).
 */
static void hvac_queue_command_locked(void)
{
    hvac_build_frame(&current_state, pending_cmd.frame);
    pending_cmd.expected = current_state;
    pending_cmd.tries = 0;
    pending_cmd.active = true;
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
 * @brief Parse a 34-byte status frame into a state struct
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
 *
 * Only the fields carried by the frame are written to *st.
 */
static void hvac_parse_status(const uint8_t *frame, hvac_state_t *st, uint8_t *swing_byte,
                              bool *fahrenheit, bool *self_clean, bool *from_remote)
{
    // Ambient temperature (bytes 10-11)
    st->ambient_temp_c = (float)frame[10] + ((float)frame[11] / 10.0f);

    // Byte 13: Power, Mode, Fan
    uint8_t b13 = frame[13];
    st->power_on = (b13 & 0x08) != 0;
    st->mode = (hvac_mode_t)(b13 & 0x07);
    st->fan_speed = (hvac_fan_t)((b13 >> 4) & 0x0F);

    // Byte 14: Temperature (with SILENT bit in bit 6)
    uint8_t temp_byte = frame[14];
    bool silent_bit = (temp_byte & 0x40) != 0;
    temp_byte &= 0x3F;

    /* Celsius: 0-15 -> 16-31 C. Fahrenheit: table-encoded 61-88 F (the AC
     * display unit was switched to F); converted to Celsius for Zigbee. */
    uint8_t temp_f = hvac_fahrenheit_from_byte(temp_byte);
    if (temp_f != 0) {
        int temp_c = ((temp_f - 32) * 5 + 4) / 9;  // rounded F -> C
        if (temp_c < 16) temp_c = 16;
        if (temp_c > 31) temp_c = 31;
        st->target_temp_c = (uint8_t)temp_c;
        *fahrenheit = true;
    } else if (temp_byte <= 15) {
        st->target_temp_c = 16 + temp_byte;
        *fahrenheit = false;
    } else {
        ESP_LOGW(TAG, "Unexpected temperature byte 0x%02X - keeping previous setpoint", temp_byte);
        *fahrenheit = use_fahrenheit;
    }

    // SILENT is signalled by the temperature byte, not the fan nibble
    if (silent_bit) {
        st->fan_speed = HVAC_FAN_SILENT;
    }

    // Byte 15: Swing. Only vertical "SWING" (0x1) counts as on; 0x2-0x6 are
    // fixed louvre positions.
    *swing_byte = frame[15];
    st->swing_on = ((frame[15] & 0x0F) == HVAC_SWING_AUTO);

    // Byte 16: Options
    uint8_t flags = frame[16];
    st->eco_mode = (flags & 0x01) != 0;       // Bit 0: ECO mode
    st->night_mode = (flags & 0x02) != 0;     // Bit 1: NIGHT mode
    st->purifier_on = (flags & 0x40) != 0;    // Bit 6: PURIFIER mode
    st->display_on = (flags & 0x80) != 0;     // Bit 7: DISPLAY on/off

    // Bit 4 (self-clean) is only meaningful in COOL or DRY mode
    *self_clean = ((flags & 0x10) != 0) &&
                  (st->mode == HVAC_MODE_COOL || st->mode == HVAC_MODE_DRY);

    // Set when the reported state comes from the IR remote, not from us
    *from_remote = ((flags & 0x04) != 0) || (st->display_on && (flags & 0x08) != 0);
}

/**
 * @brief Does a reported state confirm the command we sent?
 */
static bool hvac_status_matches(const hvac_state_t *expected, const hvac_state_t *st)
{
    if (!expected->power_on) {
        // The AC clears options on its own when switched off - only power matters
        return !st->power_on;
    }
    if (!st->power_on || expected->mode != st->mode) {
        return false;
    }
    // In AUTO mode the AC picks its own setpoint
    if (expected->mode != HVAC_MODE_AUTO && expected->target_temp_c != st->target_temp_c) {
        return false;
    }
    return expected->fan_speed == st->fan_speed &&
           expected->swing_on == st->swing_on &&
           expected->eco_mode == st->eco_mode &&
           expected->night_mode == st->night_mode &&
           expected->purifier_on == st->purifier_on &&
           expected->display_on == st->display_on;
}

/**
 * @brief Has current_state changed since the Zigbee layer was last notified?
 *
 * Caller holds state_mutex. Records the new state when it returns true.
 */
static bool hvac_state_changed_locked(void)
{
    static hvac_state_t prev_state = {0};

    bool changed = force_notify ||
        prev_state.power_on != current_state.power_on ||
        prev_state.mode != current_state.mode ||
        prev_state.fan_speed != current_state.fan_speed ||
        prev_state.target_temp_c != current_state.target_temp_c ||
        prev_state.ambient_temp_c != current_state.ambient_temp_c ||
        prev_state.eco_mode != current_state.eco_mode ||
        prev_state.night_mode != current_state.night_mode ||
        prev_state.display_on != current_state.display_on ||
        prev_state.purifier_on != current_state.purifier_on ||
        prev_state.clean_status != current_state.clean_status ||
        prev_state.swing_on != current_state.swing_on ||
        prev_state.mute_on != current_state.mute_on ||
        prev_state.error != current_state.error ||
        strcmp(prev_state.error_text, current_state.error_text) != 0;

    if (changed) {
        prev_state = current_state;
        force_notify = false;
    }
    return changed;
}

/**
 * @brief Decode a received, CRC-valid HVAC frame
 */
static void hvac_decode_state(const uint8_t *frame, size_t len)
{
    if (len < 13) {
        ESP_LOGW(TAG, "Frame too short: %d bytes", len);
        return;
    }

    // Log full frame in hex for debugging
    char hex_str[256] = {0};
    char *ptr = hex_str;
    for (size_t i = 0; i < len && i < 64; i++) {
        ptr += sprintf(ptr, "%02X ", frame[i]);
    }
    ESP_LOGI(TAG, "RX [%d bytes]: %s", len, hex_str);

    // 13-byte ACK frames: 7A 7A D1 21 0D 00 00 A4 0A 0A 00 CRC CRC - no state
    if (len == 13) {
        ESP_LOGD(TAG, "ACK frame received from AC (13 bytes)");
        return;
    }

    // 18-byte frames (keepalive/other) - nothing to decode
    if (len == 18) {
        ESP_LOGD(TAG, "18-byte frame received (keepalive/other)");
        return;
    }

    bool notify = false;

    xSemaphoreTake(state_mutex, portMAX_DELAY);

    if (len == 28 && frame[2] == 0xD5 && frame[3] == 0x21) {
        // Warning/error frame
        uint8_t warn = frame[10];
        uint8_t fault = frame[12];

        if (fault != 0x00) {
            // We only know that 0x04 = PC (Fashion Conflict)
            current_state.error = true;
            filter_dirty = false;
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
            current_state.error = false;
            snprintf(current_state.error_text, sizeof(current_state.error_text),
                     "WARNING 0x%02X: %s", warn, warn_desc);
            filter_dirty = (warn == 0x80);  // Filter needs cleaning only for 0x80
        } else {
            filter_dirty = false;
            current_state.error = false;
            current_state.error_text[0] = '\0';  // Empty string when no error
        }
        current_state.clean_status = filter_dirty || self_clean_active;
        notify = hvac_state_changed_locked();

    } else if (len == 34) {
        hvac_state_t st = current_state;  // keeps the fields a status frame does not carry
        uint8_t swing_byte = 0;
        bool fahrenheit = false, self_clean = false, from_remote = false;

        hvac_parse_status(frame, &st, &swing_byte, &fahrenheit, &self_clean, &from_remote);

        ESP_LOGI(TAG, "Status: Power=%s, Mode=%d, Fan=0x%02X, Temp=%d°C%s, Ambient=%.1f°C, Origin=%s",
                 st.power_on ? "ON" : "OFF", st.mode, st.fan_speed, st.target_temp_c,
                 fahrenheit ? " (AC in °F)" : "", st.ambient_temp_c,
                 from_remote ? "remote" : "module");
        ESP_LOGI(TAG, "  Options: Eco=%s, Night=%s, Display=%s, Purifier=%s, SelfClean=%s, Swing=0x%02X",
                 st.eco_mode ? "ON" : "OFF", st.night_mode ? "ON" : "OFF",
                 st.display_on ? "ON" : "OFF", st.purifier_on ? "ON" : "OFF",
                 self_clean ? "YES" : "NO", swing_byte);

        /* While a command is waiting to be sent or to be confirmed, the AC's
         * report may still describe the state from before that command. Taking
         * it would silently undo what the user just asked for, so it is only
         * accepted once it confirms the command (or the command is abandoned,
         * see hvac_tx_service). */
        bool apply = true;

        if (pending_cmd.active) {
            ESP_LOGD(TAG, "Status ignored - a command is waiting to be sent");
            apply = false;
        } else if (inflight_cmd.active) {
            TickType_t dt = xTaskGetTickCount() - inflight_cmd.sent_at;
            if (dt < pdMS_TO_TICKS(HVAC_ACK_EVAL_MIN_MS)) {
                ESP_LOGD(TAG, "Status ignored - too early to be a reply to the command");
                apply = false;
            } else if (!from_remote && hvac_status_matches(&inflight_cmd.expected, &st)) {
                ESP_LOGI(TAG, "Command confirmed by AC after %lu ms (try %d)",
                         (unsigned long)pdTICKS_TO_MS(dt), inflight_cmd.tries + 1);
                inflight_cmd.active = false;
            } else {
                ESP_LOGW(TAG, "Status does not confirm the command yet (try %d/%d)",
                         inflight_cmd.tries + 1, HVAC_CMD_MAX_RETRY + 1);
                apply = false;
            }
        }

        if (apply) {
            current_state = st;
            swing_raw = swing_byte;
            use_fahrenheit = fahrenheit;
            self_clean_active = self_clean;
            current_state.clean_status = filter_dirty || self_clean_active;
        } else {
            // The room temperature is always safe to take
            current_state.ambient_temp_c = st.ambient_temp_c;
        }
        notify = hvac_state_changed_locked();

    } else {
        ESP_LOGW(TAG, "Unexpected frame length %d", len);
    }

    xSemaphoreGive(state_mutex);

    if (notify) {
        ESP_LOGI(TAG, "State change detected - notifying Zigbee");
        if (state_change_callback) {
            state_change_callback();
        }
    }
}

/**
 * @brief Transmit side of the HVAC task
 *
 * All UART writes happen here, one frame at a time, only when the line has
 * been quiet for HVAC_RX_SILENCE_MS and HVAC_TX_INTERVAL_MS has passed since
 * the previous frame. A command stays "in flight" until a status frame
 * confirms it; if none does within HVAC_ACK_RETRY_MS it is re-sent (muted, so
 * the AC does not beep again) up to HVAC_CMD_MAX_RETRY times, then abandoned
 * and the real AC state is fetched so Zigbee shows what the AC actually did.
 */
static void hvac_tx_service(void)
{
    TickType_t now = xTaskGetTickCount();
    bool retry_due = false;

    xSemaphoreTake(state_mutex, portMAX_DELAY);

    if (inflight_cmd.active && !pending_cmd.active) {
        TickType_t dt = now - inflight_cmd.sent_at;
        if (inflight_cmd.tries < HVAC_CMD_MAX_RETRY) {
            retry_due = (dt >= pdMS_TO_TICKS(HVAC_ACK_RETRY_MS));
        } else if (dt >= pdMS_TO_TICKS(HVAC_ACK_ABORT_MS)) {
            ESP_LOGE(TAG, "Command not confirmed after %d tries - giving up, resyncing with AC",
                     inflight_cmd.tries + 1);
            inflight_cmd.active = false;
            status_pending = true;   // fetch what the AC really has
            force_notify = true;     // and push it to Zigbee even if unchanged
        }
    }

    bool line_idle = (rx_buffer_len == 0) &&
                     (now - last_rx_time) >= pdMS_TO_TICKS(HVAC_RX_SILENCE_MS) &&
                     (now - last_tx_time) >= pdMS_TO_TICKS(HVAC_TX_INTERVAL_MS);

    if (line_idle) {
        if (pending_cmd.active) {
            // A newer command supersedes whatever was still unconfirmed
            inflight_cmd = pending_cmd;
            pending_cmd.active = false;
            inflight_cmd.sent_at = now;
            last_tx_time = now;
            hvac_send_frame(inflight_cmd.frame, HVAC_CMD_FRAME_LEN);
        } else if (retry_due) {
            // Mute the retry and fix the CRC
            inflight_cmd.frame[16] |= 0x01;
            uint16_t crc = hvac_crc16(inflight_cmd.frame, 22);
            inflight_cmd.frame[22] = (crc >> 8) & 0xFF;
            inflight_cmd.frame[23] = crc & 0xFF;
            inflight_cmd.tries++;
            inflight_cmd.sent_at = now;
            last_tx_time = now;
            ESP_LOGW(TAG, "Command not confirmed - retry %d/%d", inflight_cmd.tries, HVAC_CMD_MAX_RETRY);
            hvac_send_frame(inflight_cmd.frame, HVAC_CMD_FRAME_LEN);
        } else if (!inflight_cmd.active) {
            // Keepalive / status poll only when no command is being confirmed
            if (keepalive_pending) {
                keepalive_pending = false;
                last_tx_time = now;
                hvac_send_frame(keepalive_frame, sizeof(keepalive_frame));
            } else if (status_pending) {
                status_pending = false;
                last_tx_time = now;
                hvac_send_frame(get_status_frame, sizeof(get_status_frame));
            }
        }
    }

    xSemaphoreGive(state_mutex);
}

/**
 * @brief HVAC task: UART receive + paced transmit
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

            // Leftover bytes that never completed a frame would block the
            // transmitter forever (it waits for an empty RX buffer) - drop them.
            if (rx_buffer_len > 0 &&
                (xTaskGetTickCount() - last_rx_time) > pdMS_TO_TICKS(HVAC_RX_STALE_MS)) {
                ESP_LOGW(TAG, "Discarding %zu stale RX bytes", rx_buffer_len);
                rx_buffer_len = 0;
            }
        }

        hvac_tx_service();

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
    
    // Save all settings (from a snapshot taken under the state mutex)
    hvac_state_t s;
    hvac_get_state(&s);
    nvs_set_u8(nvs_handle, "mode", (uint8_t)s.mode);
    nvs_set_u8(nvs_handle, "power", s.power_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "temp", s.target_temp_c);
    nvs_set_u8(nvs_handle, "fan", (uint8_t)s.fan_speed);
    nvs_set_u8(nvs_handle, "eco", s.eco_mode ? 1 : 0);
    nvs_set_u8(nvs_handle, "night", s.night_mode ? 1 : 0);
    nvs_set_u8(nvs_handle, "display", s.display_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "swing", s.swing_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "purifier", s.purifier_on ? 1 : 0);
    nvs_set_u8(nvs_handle, "mute", s.mute_on ? 1 : 0);
    
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

    // Create state mutex
    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == NULL) {
        ESP_LOGE(TAG, "[ERROR] Failed to create state mutex");
        return ESP_FAIL;
    }

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
    BaseType_t task_ret = xTaskCreate(hvac_rx_task, "hvac_rx", 4096, NULL, 5, NULL);
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
    
    /* Do NOT push the NVS settings to the AC here. They only track changes made
     * over Zigbee (saved with a 15-minute delay), so they can be stale: sending
     * them would undo anything done with the IR remote, e.g. switch the AC back
     * on after a module reboot. The AC is the source of truth - ask it for its
     * state; the status frame overwrites the loaded values. */
    ESP_LOGI(TAG, "[HVAC] Requesting current state from HVAC");
    hvac_request_status();
    
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

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    memcpy(state, &current_state, sizeof(hvac_state_t));
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

/**
 * @brief Set HVAC power
 */
esp_err_t hvac_set_power(bool power_on)
{
    ESP_LOGI(TAG, "Setting power: %s", power_on ? "ON" : "OFF");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.power_on = power_on;
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Set HVAC mode
 */
esp_err_t hvac_set_mode(hvac_mode_t mode)
{
    ESP_LOGI(TAG, "Setting mode: %d", mode);
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.mode = mode;
    if (mode != HVAC_MODE_OFF) {
        current_state.power_on = true;
    }
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
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
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.target_temp_c = temp_c;
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Set eco mode
 */
esp_err_t hvac_set_eco_mode(bool eco_on)
{
    ESP_LOGI(TAG, "Setting eco mode: %s", eco_on ? "ON" : "OFF");

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    // Eco mode only works in COOL mode
    if (eco_on && current_state.mode != HVAC_MODE_COOL) {
        // The Zigbee attribute was already written to ON by the stack: fetch
        // the AC state and push it back so the switch does not stay wrong.
        status_pending = true;
        force_notify = true;
        xSemaphoreGive(state_mutex);
        ESP_LOGW(TAG, "Eco mode only available in COOL mode");
        return ESP_ERR_INVALID_STATE;
    }
    current_state.eco_mode = eco_on;
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Set display state
 */
esp_err_t hvac_set_display(bool display_on)
{
    ESP_LOGI(TAG, "Setting display: %s", display_on ? "ON" : "OFF");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.display_on = display_on;
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Set swing mode
 */
esp_err_t hvac_set_swing(bool swing_on)
{
    ESP_LOGI(TAG, "Setting swing: %s", swing_on ? "ON" : "OFF");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.swing_on = swing_on;
    // Vertical nibble: SWING (0x1) when on. When off, stop the swing but keep
    // a fixed louvre position (0x2-0x6) set from the remote. The horizontal
    // nibble is left as the AC reported it.
    if (swing_on) {
        swing_raw = (swing_raw & 0xF0) | HVAC_SWING_AUTO;
    } else if ((swing_raw & 0x0F) == HVAC_SWING_AUTO) {
        swing_raw = (swing_raw & 0xF0) | HVAC_SWING_STOP;
    }
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Set fan speed
 */
esp_err_t hvac_set_fan_speed(hvac_fan_t fan)
{
    ESP_LOGI(TAG, "Setting fan speed: %d", fan);
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.fan_speed = fan;
    // In eco mode, fan is forced to AUTO
    if (current_state.eco_mode) {
        ESP_LOGW(TAG, "Fan speed ignored in eco mode (forced to AUTO)");
        current_state.fan_speed = HVAC_FAN_AUTO;
        force_notify = true;  // correct the Zigbee attribute the stack already wrote
    }
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Request status from HVAC
 */
esp_err_t hvac_request_status(void)
{
    /* Queued: the HVAC task transmits it when the line is idle */
    ESP_LOGD(TAG, "Requesting HVAC status");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    status_pending = true;
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

/**
 * @brief Send keepalive frame
 */
esp_err_t hvac_send_keepalive(void)
{
    /* Queued: the HVAC task transmits it when the line is idle */
    ESP_LOGD(TAG, "Sending keepalive");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    keepalive_pending = true;
    xSemaphoreGive(state_mutex);
    return ESP_OK;
}

/**
 * @brief Set night mode
 */
esp_err_t hvac_set_night_mode(bool night_on)
{
    ESP_LOGI(TAG, "Setting night mode: %s", night_on ? "ON" : "OFF");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.night_mode = night_on;
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Set purifier mode
 */
esp_err_t hvac_set_purifier(bool purifier_on)
{
    ESP_LOGI(TAG, "Setting purifier: %s", purifier_on ? "ON" : "OFF");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.purifier_on = purifier_on;
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Set mute mode
 */
esp_err_t hvac_set_mute(bool mute_on)
{
    ESP_LOGI(TAG, "Setting mute: %s", mute_on ? "ON" : "OFF");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    current_state.mute_on = mute_on;
    hvac_queue_command_locked();
    xSemaphoreGive(state_mutex);
    hvac_save_settings();
    return ESP_OK;
}

/**
 * @brief Get clean status
 */
bool hvac_get_clean_status(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool status = current_state.clean_status;
    xSemaphoreGive(state_mutex);
    return status;
}

/**
 * @brief Register callback for state changes
 */
void hvac_register_state_change_callback(hvac_state_change_callback_t callback)
{
    state_change_callback = callback;
    if (callback) {
        ESP_LOGI(TAG, "State change callback registered");
    } else {
        ESP_LOGI(TAG, "State change callback unregistered");
    }
}
