/*
 * HVAC Driver Header
 * 
 * Handles UART communication with ACW02 HVAC device
 * Supports control of mode, temperature, eco, swing, display
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HVAC Mode enumeration */
typedef enum {
    HVAC_MODE_OFF = 0xFF,
    HVAC_MODE_AUTO = 0x00,
    HVAC_MODE_COOL = 0x01,
    HVAC_MODE_DRY = 0x02,
    HVAC_MODE_FAN = 0x03,
    HVAC_MODE_HEAT = 0x04
} hvac_mode_t;

/* HVAC Fan speed enumeration */
typedef enum {
    HVAC_FAN_AUTO = 0x00,
    HVAC_FAN_P20 = 0x01,
    HVAC_FAN_P40 = 0x02,
    HVAC_FAN_P60 = 0x03,
    HVAC_FAN_P80 = 0x04,
    HVAC_FAN_P100 = 0x05,
    HVAC_FAN_SILENT = 0x06,
    HVAC_FAN_TURBO = 0x0D
} hvac_fan_t;

/* HVAC Swing enumeration */
typedef enum {
    HVAC_SWING_STOP = 0x00,
    HVAC_SWING_AUTO = 0x01,
    HVAC_SWING_P1 = 0x02,
    HVAC_SWING_P2 = 0x03,
    HVAC_SWING_P3 = 0x04,
    HVAC_SWING_P4 = 0x05,
    HVAC_SWING_P5 = 0x06
} hvac_swing_t;

/* HVAC Horizontal swing enumeration (upper nibble of the swing byte) */
typedef enum {
    HVAC_SWING_H_STOP = 0x00,
    HVAC_SWING_H_AUTO_LEFT = 0x01,      // full left-right swing
    HVAC_SWING_H_P1 = 0x02,
    HVAC_SWING_H_P2 = 0x03,
    HVAC_SWING_H_P3 = 0x04,
    HVAC_SWING_H_P4 = 0x05,
    HVAC_SWING_H_P5 = 0x06,
    HVAC_SWING_H_RANGE_P1_P5 = 0x0C,
    HVAC_SWING_H_AUTO_MID_OUT = 0x0D
} hvac_swing_h_t;

/* HVAC State structure */
typedef struct {
    hvac_mode_t mode;
    bool power_on;
    uint8_t target_temp_c;  // Temperature in Celsius (16-31)
    float ambient_temp_c;   // Current room temperature (with decimal precision)
    bool eco_mode;
    bool night_mode;        // Night mode (sleep mode)
    bool display_on;
    bool swing_on;
    bool purifier_on;       // Air purifier/ionizer
    bool clean_status;      // Filter cleaning status (read-only from AC)
    bool mute_on;           // Mute (silent commands)
    hvac_fan_t fan_speed;
    bool error;
    char error_text[64];
    bool self_clean;        // Self-clean cycle requested or running
    uint8_t swing_v_pos;    // Last fixed vertical louvre position (1-5)
    hvac_swing_h_t swing_h; // Horizontal swing mode
} hvac_state_t;

/* UART Configuration */
#define HVAC_UART_NUM           UART_NUM_1
#define HVAC_UART_TX_PIN        18   // GPIO18 (D10 on XIAO ESP32-C6)
#define HVAC_UART_RX_PIN        20   // GPIO20 (D9 on XIAO ESP32-C6)
#define HVAC_UART_BAUD_RATE     9600
#define HVAC_UART_BUF_SIZE      1024

/* Frame constants */
#define HVAC_FRAME_HEADER_1     0x7A
#define HVAC_FRAME_HEADER_2     0x7A

/**
 * @brief Initialize HVAC UART driver
 * 
 * @return ESP_OK on success
 */
esp_err_t hvac_driver_init(void);

/**
 * @brief Get current HVAC state
 * 
 * @param state Pointer to state structure to fill
 * @return ESP_OK on success
 */
esp_err_t hvac_get_state(hvac_state_t *state);

/**
 * @brief Set HVAC power state
 * 
 * @param power_on true to turn on, false to turn off
 * @return ESP_OK on success
 */
esp_err_t hvac_set_power(bool power_on);

/**
 * @brief Set HVAC mode
 * 
 * @param mode Operating mode
 * @return ESP_OK on success
 */
esp_err_t hvac_set_mode(hvac_mode_t mode);

/**
 * @brief Set target temperature
 * 
 * @param temp_c Temperature in Celsius (16-31)
 * @return ESP_OK on success
 */
esp_err_t hvac_set_temperature(uint8_t temp_c);

/**
 * @brief Set eco mode
 * 
 * @param eco_on true to enable eco mode
 * @return ESP_OK on success
 */
esp_err_t hvac_set_eco_mode(bool eco_on);

/**
 * @brief Set display state
 * 
 * @param display_on true to turn on display
 * @return ESP_OK on success
 */
esp_err_t hvac_set_display(bool display_on);

/**
 * @brief Set swing mode
 * 
 * @param swing_on true to enable swing
 * @return ESP_OK on success
 */
esp_err_t hvac_set_swing(bool swing_on);

/**
 * @brief Move the vertical louvre to a fixed position (stops vertical swing)
 *
 * @param position 1-5
 * @return ESP_OK on success
 */
esp_err_t hvac_set_swing_position(uint8_t position);

/**
 * @brief Set horizontal swing mode
 *
 * @param mode Horizontal swing mode
 * @return ESP_OK on success
 */
esp_err_t hvac_set_swing_horizontal(hvac_swing_h_t mode);

/**
 * @brief Start or stop a self-clean cycle
 *
 * The AC only self-cleans while switched off in COOL or DRY mode, so starting
 * a cycle switches the AC off (and to COOL if needed) first. Any other command
 * cancels the cycle.
 *
 * @param on true to start, false to stop
 * @return ESP_OK on success
 */
esp_err_t hvac_set_self_clean(bool on);

/**
 * @brief Set night mode (sleep mode)
 * 
 * @param night_on true to enable night mode
 * @return ESP_OK on success
 */
esp_err_t hvac_set_night_mode(bool night_on);

/**
 * @brief Set purifier mode (air ionizer)
 * 
 * @param purifier_on true to enable purifier
 * @return ESP_OK on success
 */
esp_err_t hvac_set_purifier(bool purifier_on);

/**
 * @brief Set mute mode (silent commands)
 * 
 * @param mute_on true to mute beep sounds
 * @return ESP_OK on success
 */
esp_err_t hvac_set_mute(bool mute_on);

/**
 * @brief Get clean status (filter cleaning indicator)
 * 
 * @return true if filter needs cleaning
 */
bool hvac_get_clean_status(void);

/**
 * @brief Set fan speed
 * 
 * @param fan Fan speed setting
 * @return ESP_OK on success
 */
esp_err_t hvac_set_fan_speed(hvac_fan_t fan);

/**
 * @brief Request status from HVAC unit
 * 
 * @return ESP_OK on success
 */
esp_err_t hvac_request_status(void);

/**
 * @brief Send keepalive frame
 * 
 * @return ESP_OK on success
 */
esp_err_t hvac_send_keepalive(void);

/**
 * @brief State change callback function type
 * 
 * Called when UART receives a state change from the AC unit.
 * This allows immediate notification instead of waiting for polling.
 */
typedef void (*hvac_state_change_callback_t)(void);

/**
 * @brief Register callback for state changes
 * 
 * @param callback Function to call when state changes (NULL to unregister)
 */
void hvac_register_state_change_callback(hvac_state_change_callback_t callback);

#ifdef __cplusplus
}
#endif
