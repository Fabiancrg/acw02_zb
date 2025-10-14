# Fix for Null running_state_ep1 and error_text_ep1

## Problem

Both `running_state_ep1` and `error_text_ep1` were showing as `null` in Zigbee2MQTT despite the firmware correctly setting these attributes.

## Root Cause

The ESP-Zigbee stack marks certain attributes as **unreportable** (cannot use automatic reporting):

1. **`runningMode`** (Thermostat cluster, 0x001E) - Shows current AC operating state
2. **`locationDescription`** (Basic cluster, 0x0010) - Contains error text string

When Z2M tries to configure automatic reporting for these attributes, the device responds with `UNREPORTABLE_ATTRIBUTE` status.

## How It Works

### Reportable Attributes (Working)
- `localTemp` - Temperature sensor attribute
- Automatically sends updates to Z2M when value changes
- Z2M receives updates without polling

### Unreportable Attributes (Need Polling)
- `runningMode` - MUST be read by Z2M, not auto-reported
- `locationDescription` - MUST be read by Z2M, not auto-reported  
- Firmware sets values with `esp_zb_zcl_set_attribute_val()`
- Values are available when Z2M reads them

## Solution

### Firmware Side (Already Implemented)
```c
// Set running_mode - value is stored but not auto-reported
esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, 
                             ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT,
                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                             ESP_ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID,
                             &running_mode, false);

// Set error text in locationDescription - value is stored but not auto-reported
esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, 
                             ESP_ZB_ZCL_CLUSTER_ID_BASIC,
                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                             ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID,
                             error_text_zigbee, false);
```

### Z2M Converter Side (Updated)
Added polling in `onEvent` handler to read attributes whenever any message is received:

```javascript
onEvent: async (type, data, device, options) => {
    const endpoint1 = device.getEndpoint(1);
    if (!endpoint1) return;
    
    // Poll on device announce, stop, or any message from the device
    const shouldPoll = type === 'deviceAnnounce' || type === 'stop' || type === 'message';
    
    if (shouldPoll) {
        // Read thermostat attributes
        await endpoint1.read('hvacThermostat', ['runningMode', 'systemMode', 
                                                'occupiedHeatingSetpoint', 
                                                'occupiedCoolingSetpoint']);
        
        // Read error text from Basic cluster
        await endpoint1.read('genBasic', ['locationDesc']);
    }
},
```

## Testing

1. **Copy updated converter** to Z2M external converters directory
2. **Reload Z2M** or restart Zigbee2MQTT
3. **Verify attributes populate**:
   - `running_state_ep1`: Should show idle/heat/cool/fan_only
   - `error_text_ep1`: Should show error messages or empty string
4. **Trigger error** on AC to see error decoding in action

## Expected Behavior After Fix

- `running_state_ep1` will update based on AC state:
  - `idle` - AC is off or in auto/dry mode
  - `heat` - AC is actively heating
  - `cool` - AC is actively cooling
  - `fan_only` - AC is in fan-only mode

- `error_text_ep1` will show:
  - Empty string when no error
  - Decoded error message (e.g., "FAULT 0xE1: Indoor temp sensor fault")

## Notes

- This is a limitation of the ESP-Zigbee stack, not a bug in our code
- Other ESP-Zigbee devices likely have the same issue with these attributes
- Polling on every message event ensures timely updates without excessive overhead
- The firmware logs confirm values are being set correctly: "Setting running_mode=0x04"
