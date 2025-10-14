# Error Diagnostics Endpoint Implementation

## Overview
Implemented **Endpoint 9** to expose AC error/warning diagnostics via Zigbee with human-readable error messages.

## Implementation Details

### Firmware Changes

#### 1. Error Code Lookup Table (`hvac_driver.c`)
- **53 error codes** from `doc/error_codes.csv` (AIRTON-LIST-ERROR-CODE.EN.pdf)
- Error code structure: `{code_high, code_low, description}`
- Examples:
  - `CL` = Filter cleaning reminder
  - `E1` = Overload protection
  - `L3` = Communication fault between indoor and outdoor unit
  - `P4` = Low voltage protection
  - `U0` = Ambient temperature sensor open/closed circuit

#### 2. Error Code Decoder Function
```c
static const char* hvac_decode_error_code(uint8_t code)
```
- Converts hex error codes to human-readable descriptions
- Special handling for filter cleaning (0x80)
- Returns "Unknown error code 0x??" if not found in table

#### 3. Enhanced 28-byte Error Frame Handler
- Reads fault code (byte 12) and warning code (byte 10)
- Populates `error_text` field with formatted messages:
  - `"FAULT 0x??: [Description]"` for faults
  - `"WARNING 0x??: [Description]"` for warnings
  - `"No Error"` when cleared
- Logs errors with descriptions to console

### Zigbee Endpoint Configuration

#### Endpoint 9: Error Diagnostics Binary Sensor
- **Cluster**: On/Off (Binary Sensor)
- **Device ID**: ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID
- **Profile**: Home Automation (HA)

**Attributes:**
1. **Standard On/Off** (0x0000): 
   - `true` = Error or warning active
   - `false` = No error
   
2. **Custom String Attribute** (0x8000):
   - Manufacturer-specific attribute
   - Contains error message text (up to 64 chars)
   - Format: Zigbee string (length byte + text)
   - Examples: "FAULT 0xE1: Overload protection"

### Zigbee Attribute Updates
In `hvac_update_zigbee_attributes()`:
```c
// Update error status: ON if error OR filter_dirty
bool error_active = state.error || state.filter_dirty;

// Update error text with proper Zigbee string format
uint8_t error_text_zigbee[65];  // length + 64 chars max
error_text_zigbee[0] = text_length;
memcpy(&error_text_zigbee[1], state.error_text, text_length);
```

### Zigbee2MQTT Integration

#### Converter Updates (`zigbee2mqtt_converter.js`)

**1. fromZigbee Converter:**
```javascript
error_status: {
    cluster: 'genOnOff',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.endpoint.ID === 9) {
            const result = {error_status: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            // Also decode custom attribute 0x8000 (32768 decimal)
            if (msg.data['32768'] !== undefined) {
                const errorTextBytes = msg.data['32768'];
                const textLength = errorTextBytes[0];
                const textData = errorTextBytes.slice(1, 1 + textLength);
                result.error_text = String.fromCharCode.apply(null, textData);
            }
            return result;
        }
    }
}
```

**2. Exposed Properties:**
- `error_status` (binary): ON/OFF - Read-only
- `error_text` (text): Error description - Read-only
- Both on endpoint `ep9`

**3. Configure/Reporting:**
- Endpoint 9 bound to coordinator
- On/Off cluster reporting enabled
- Automatic updates when error status changes

## Z2M UI Display

When error occurs, Z2M will show:
```
Error Status: ON
Error Text: "FAULT 0xE1: Overload protection"
```

When no error:
```
Error Status: OFF
Error Text: "No Error"
```

## Error Code Examples

| Code | Meaning | Location |
|------|---------|----------|
| CL | Filter cleaning reminder | Indoor unit |
| E0 | Protection against high discharge temperatures | Outdoor unit |
| E1 | Overload protection | Outdoor unit |
| E2 | Compressor overload protection | Outdoor unit |
| E3 | Frost protection | Outdoor unit |
| L3 | Communication fault between indoor/outdoor | Indoor unit |
| L5 | EEPROM error | Indoor unit |
| P0 | EEPROM error | Outdoor unit |
| P3 | High voltage protection | Outdoor unit |
| P4 | Low voltage protection | Outdoor unit |
| U0 | Ambient temperature sensor fault | Indoor unit |
| U1 | Pipe temperature sensor fault | Indoor unit |

Full list: See `doc/error_codes.csv`

## Benefits

1. **Real-time Error Monitoring**: Errors appear immediately in Z2M
2. **Human-Readable Messages**: No need to look up error codes manually
3. **Standardized Zigbee**: Uses standard Binary Sensor cluster
4. **Comprehensive Coverage**: All 53 AIRTON error codes supported
5. **Read-Only Protection**: Error status can't be accidentally changed via Z2M
6. **Automatic Updates**: Error status pushed to Z2M via reporting

## Testing

To test error diagnostics:
1. Flash firmware to device
2. Pair with Zigbee2MQTT
3. Reconfigure device in Z2M UI
4. Wait for AC to report an error condition (or simulate one)
5. Check Z2M for `error_status` and `error_text` properties on endpoint 9

## Files Modified

- `main/esp_zb_hvac.h` - Added HA_ESP_ERROR_ENDPOINT (9)
- `main/esp_zb_hvac.c` - Created endpoint 9, updated attributes
- `main/hvac_driver.c` - Added error code table and decoder
- `zigbee2mqtt_converter.js` - Added error_status converter and exposes
- `doc/error_codes.csv` - Error code reference (user-created)

## Next Steps

1. Build and flash firmware
2. Test with physical AC
3. Verify error messages appear in Z2M
4. Document any additional error codes if found
