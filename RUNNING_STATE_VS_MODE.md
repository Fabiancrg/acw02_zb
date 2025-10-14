# runningMode vs runningState - Zigbee Thermostat Attributes

## The Confusion

There are **TWO different attributes** in the Zigbee Thermostat Cluster (0x0201):

### 1. `runningMode` (Attribute 0x001E)
- **Type**: 8-bit Enumeration
- **Purpose**: Indicates the current **mode** the thermostat is running in
- **Values**:
  - `0x00` = Off/Idle
  - `0x03` = Cool
  - `0x04` = Heat
  - `0x07` = Fan Only
- **Reportable in ESP-Zigbee**: ❌ **NO** - Must be polled
- **What we were using**: This attribute only

### 2. `runningState` (Attribute 0x0029)
- **Type**: 16-bit Bitmap
- **Purpose**: Indicates what HVAC **components** are actively running
- **Bitmap Bits**:
  - Bit 0 = Heat stage 1 running
  - Bit 1 = Cool stage 1 running
  - Bit 2 = Fan stage 1 running
  - Bit 3 = Heat stage 2 running
  - Bit 4 = Cool stage 2 running
  - Bit 5 = Fan stage 2 running
  - Bit 6 = Fan stage 3 running
- **Reportable in ESP-Zigbee**: ❓ **UNKNOWN** - Testing required
- **What we're now trying**: Added this attribute to test reportability

## Why This Matters

The `runningState` bitmap attribute **might be reportable** in ESP-Zigbee, even though `runningMode` is not. This would eliminate the need for polling.

## Changes Made

### Firmware (esp_zb_hvac.c)

1. **Added `runningState` attribute** to thermostat cluster initialization:
```c
uint16_t running_state = 0x0000;
esp_zb_thermostat_cluster_add_attr(esp_zb_thermostat_cluster,
                                   0x0029,  // runningState bitmap
                                   &running_state);
```

2. **Set both attributes** when state changes:
```c
// Set runningMode (enum - definitely not reportable)
esp_zb_zcl_set_attribute_val(..., ESP_ZB_ZCL_ATTR_THERMOSTAT_RUNNING_MODE_ID, ...);

// Set runningState (bitmap - might be reportable!)
uint16_t running_state_bitmap = 0x0000;
if (state.mode == HVAC_MODE_HEAT) running_state_bitmap = 0x0001;  // Bit 0
if (state.mode == HVAC_MODE_COOL) running_state_bitmap = 0x0002;  // Bit 1
if (state.mode == HVAC_MODE_FAN) running_state_bitmap = 0x0004;   // Bit 2
esp_zb_zcl_set_attribute_val(..., 0x0029, &running_state_bitmap, ...);
```

### Z2M Converter (zigbee2mqtt_converter.js)

1. **Try to configure automatic reporting** for `runningState`:
```javascript
try {
    await endpoint1.configureReporting('hvacThermostat', [{
        attribute: 'runningState',  // 0x0029
        minimumReportInterval: 0,
        maximumReportInterval: 3600,
        reportableChange: 1,
    }]);
} catch (error) {
    // If it fails, we'll poll it instead
}
```

2. **Poll both attributes**:
```javascript
await endpoint1.read('hvacThermostat', ['runningState', 'runningMode', ...]);
```

3. **Prefer `runningState` over `runningMode`** in converter:
```javascript
if (msg.data.hasOwnProperty('runningState')) {
    // Use bitmap - bit 0=heat, bit 1=cool, bit 2=fan
    const bitmap = msg.data.runningState;
    if (bitmap & 0x0001) result.running_state_ep1 = 'heat';
    else if (bitmap & 0x0002) result.running_state_ep1 = 'cool';
    else if (bitmap & 0x0004) result.running_state_ep1 = 'fan_only';
    else result.running_state_ep1 = 'idle';
} else if (msg.data.hasOwnProperty('runningMode')) {
    // Fallback to enum if bitmap not available
    result.running_state_ep1 = modeMap[msg.data.runningMode];
}
```

## Testing Required

After rebuilding firmware and reloading Z2M converter:

1. **Check Z2M logs** during device configuration:
   - ✅ **SUCCESS**: No `UNREPORTABLE_ATTRIBUTE` error for `runningState`
   - ❌ **FAILURE**: `UNREPORTABLE_ATTRIBUTE` error (same as `runningMode`)

2. **Monitor attribute updates**:
   - ✅ **SUCCESS**: `running_state_ep1` updates automatically without polling
   - ❌ **FAILURE**: Still shows null until manually polled

3. **Check automatic reports**:
   - ✅ **SUCCESS**: Z2M logs show `attributeReport` for `runningState`
   - ❌ **FAILURE**: Only see `readResponse` (from polling)

## Expected Outcomes

### Best Case: `runningState` IS Reportable ✅
- No polling needed for running state
- Instant updates when AC mode changes
- Lower network traffic
- More responsive Home Assistant automation

### Worst Case: `runningState` is ALSO Unreportable ❌
- Falls back to polling mechanism (already implemented)
- No worse than before
- Still provides correct state via polling

## Summary

We've implemented **dual attribute support** to test if the `runningState` bitmap is reportable in ESP-Zigbee. The firmware now sets both attributes, and the Z2M converter tries automatic reporting first, falling back to polling if needed.

**Next Step**: Build firmware, flash, reload converter, and check Z2M logs to see if `runningState` reporting succeeds!
