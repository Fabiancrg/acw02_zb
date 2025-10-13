# New Endpoints Added - Summary

## Overview
Added 4 new endpoints to the ACW02 Zigbee HVAC controller:
- **Endpoint 5**: Night Mode (Sleep mode)
- **Endpoint 6**: Purifier (Air ionizer/purifier)
- **Endpoint 7**: Clean Status (Filter cleaning indicator - READ-ONLY)
- **Endpoint 8**: Mute (Silent commands - no beep)

## ACW02 Protocol Mapping

### Frame Byte 15 (Options byte):
```
Bit 0 (0x01): ECO mode       → Endpoint 2 ✅
Bit 1 (0x02): NIGHT mode     → Endpoint 5 ✅ NEW
Bit 4 (0x10): CLEAN status   → Endpoint 7 ✅ NEW (read-only from AC)
Bit 6 (0x40): PURIFIER mode  → Endpoint 6 ✅ NEW
Bit 7 (0x80): DISPLAY on/off → Endpoint 4 ✅
```

### Frame Byte 16 (Mute byte):
```
Bit 0 (0x01): MUTE → Endpoint 8 ✅ NEW
```

## Changes Made

### 1. Header Files
**`esp_zb_light.h`:**
- Added endpoint definitions:
  - `HA_ESP_NIGHT_ENDPOINT` = 5
  - `HA_ESP_PURIFIER_ENDPOINT` = 6
  - `HA_ESP_CLEAN_ENDPOINT` = 7
  - `HA_ESP_MUTE_ENDPOINT` = 8

**`hvac_driver.h`:**
- Added state fields to `hvac_state_t`:
  - `bool night_mode`
  - `bool purifier_on`
  - `bool clean_status` (read-only)
  - `bool mute_on`
- Added function declarations:
  - `esp_err_t hvac_set_night_mode(bool night_on)`
  - `esp_err_t hvac_set_purifier(bool purifier_on)`
  - `esp_err_t hvac_set_mute(bool mute_on)`
  - `bool hvac_get_clean_status(void)`

### 2. HVAC Driver (`hvac_driver.c`)
- Initialized new state fields in `current_state`
- Updated frame building (byte 15 & 16):
  ```c
  // Byte 15: Options
  if (current_state.night_mode) options |= 0x02;
  if (current_state.purifier_on) options |= 0x40;
  
  // Byte 16: Mute
  frame[16] = current_state.mute_on ? 0x01 : 0x00;
  ```
- Updated frame decoding (reading from AC):
  ```c
  current_state.night_mode = (flags & 0x02) != 0;
  current_state.clean_status = (flags & 0x10) != 0;
  current_state.purifier_on = (flags & 0x40) != 0;
  ```
- Added implementation functions:
  - `hvac_set_night_mode()`
  - `hvac_set_purifier()`
  - `hvac_set_mute()`
  - `hvac_get_clean_status()`
- Enhanced logging to show all new options

### 3. Zigbee Application (`esp_zb_hvac.c`)
- Created 4 new On/Off cluster endpoints:
  - Endpoint 5: Night Mode switch
  - Endpoint 6: Purifier switch
  - Endpoint 7: Clean status binary sensor
  - Endpoint 8: Mute switch
- Added manufacturer info to all new endpoints
- Added command handlers in `zb_attribute_handler()`:
  - Night mode: Calls `hvac_set_night_mode()`
  - Purifier: Calls `hvac_set_purifier()`
  - Clean status: Read-only, no commands accepted
  - Mute: Calls `hvac_set_mute()`
- Updated `hvac_update_zigbee_attributes()` to sync all new endpoints
- Enhanced logging to show all switch states

### 4. Zigbee2MQTT Converter (`zigbee2mqtt_converter.js`)
- Added `tzLocal` converters (to device):
  - `night_mode`
  - `purifier`
  - `mute`
  - (clean_status is read-only, no toZigbee needed)
- Added `fzLocal` converters (from device):
  - `night_mode` (endpoint 5)
  - `purifier` (endpoint 6)
  - `clean_status` (endpoint 7)
  - `mute` (endpoint 8)
- Added exposes:
  - Night mode with description
  - Purifier with description
  - Clean status (read-only: `exposes.access.STATE_GET`)
  - Mute with description
- Updated endpoint mapping to include ep5-ep8
- Added binding and reporting configuration for all 4 new endpoints

## Usage in Zigbee2MQTT

After flashing and pairing, you'll see these controls in Z2M:

1. **Night Mode** (`night_mode`): ON/OFF
   - Sleep mode with adjusted temperature/fan settings
   - Endpoint 5

2. **Purifier** (`purifier`): ON/OFF
   - Air ionizer/purifier feature
   - Endpoint 6

3. **Clean Status** (`clean_status`): ON/OFF (READ-ONLY)
   - Shows when filter needs cleaning
   - Automatically set by AC
   - Endpoint 7

4. **Mute** (`mute`): ON/OFF
   - Silent commands (AC won't beep)
   - Endpoint 8

## Testing Checklist

- [ ] Build project successfully
- [ ] Flash to ESP32-C6
- [ ] Device joins Zigbee network
- [ ] All 8 endpoints visible in Z2M
- [ ] Night mode switch works
- [ ] Purifier switch works
- [ ] Clean status updates (when AC reports it)
- [ ] Mute switch works (commands don't beep on AC)
- [ ] Verify frame bytes 15 & 16 are correct in logs

## Notes

- **Clean Status** is READ-ONLY: The AC sets this when the filter needs cleaning. You cannot manually trigger it.
- **Night Mode**: Works best in COOL, DRY, or HEAT modes (not in AUTO or FAN modes per ACW02 protocol)
- **Purifier**: Only available if your ACW02 model has the air ionizer feature
- **Mute**: Commands sent with mute=ON won't make the AC beep

## Frame Structure Reference

**24-byte Command Frame:**
```
[0-1]   Header: 0x7A 0x7A
[2-7]   Header: 0x21 0xD5 0x18 0x00 0x00 0xA1
[8-11]  Reserved: 0x00
[12]    Power/Mode/Fan: (fan<<4) | (power<<3) | mode
[13]    Temperature: encoded value (+ 0x40 for SILENT)
[14]    Swing: (horizontal<<4) | vertical
[15]    Options: eco|night|clean|purifier|display
[16]    Mute: 0x01 if muted
[17-21] Reserved: 0x00
[22-23] CRC16: MSB, LSB
```

**34-byte Status Frame (from AC):**
```
[0-1]   Header: 0x7A 0x7A
[2-3]   Type: 0xD5 0x21 (status response)
[10-11] Ambient temperature: integer, decimal
[13]    Power/Mode/Fan
[14]    Temperature
[15]    Swing
[16]    Options: eco|night|clean|purifier|display
[32-33] CRC16
```
