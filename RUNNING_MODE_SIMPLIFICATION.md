# Running Mode Simplification

## Summary
Removed redundant `runningState` bitmap attribute, keeping only `runningMode` enum for simplicity.

## What Changed

### Testing Results
Both attributes were tested for reportability:
- **runningMode (0x001E)** - 8-bit enum: UNREPORTABLE ❌
- **runningState (0x0029)** - 16-bit bitmap: UNREPORTABLE ❌

Both worked via polling: `{"runningState":1,"runningMode":4}` → `"running_mode_ep1":"heat"` ✅

**Conclusion**: Since both require polling anyway, keep the simpler enum version.

## Changes Made

### Firmware (esp_zb_hvac.c)
1. **Removed runningState bitmap attribute** (lines 727-729)
   - No longer creating 0x0029 attribute in cluster
2. **Removed bitmap setting code** (lines 561-589)
   - Simplified to only set runningMode enum
3. **Kept runningMode enum** (0x001E)
   - Values: 0x00=idle, 0x03=cool, 0x04=heat, 0x07=fan

### Z2M Converter (zigbee2mqtt_converter.js)
1. **Updated converter logic**
   - Changed from `running_state_ep1` to `running_mode_ep1`
   - Removed bitmap decoding, kept enum mapping
2. **Updated exposes**
   - Changed `.withRunningState()` to `.withRunningMode()`
   - UI now shows "Running Mode" instead of "Running State"
3. **Simplified polling**
   - Removed 'runningState' from read array
   - Now reads: `['runningMode', 'systemMode', 'occupiedHeatingSetpoint', 'occupiedCoolingSetpoint']`
4. **Removed reporting configuration**
   - Deleted runningState configureReporting attempt
   - Comment clarifies runningMode is unreportable

## Current State

### Attribute Details
| Attribute | ID | Type | Values | Reportable? | How Updated |
|-----------|-----|------|--------|-------------|-------------|
| runningMode | 0x001E | enum8 | 0=idle, 3=cool, 4=heat, 7=fan | ❌ NO | Polling (30s) |
| systemMode | 0x001C | enum8 | 0=off, 1=auto, 3=cool, 4=heat... | ✅ YES | Automatic |
| localTemp | 0x0000 | int16 | Temperature × 100 | ✅ YES | Automatic |

### Polling Mechanism
**Event-Based Triggers:**
- Device announce (rejoins network)
- Stop event (cleanup)
- Any message event (state changes)

**Periodic Timer:**
- Default: 30 seconds
- Configurable via `state_poll_interval` option
- Reads all unreportable attributes

### MQTT Payload Example
```json
{
  "local_temperature_ep1": 25,
  "running_mode_ep1": "heat",
  "system_mode_ep1": "heat",
  "occupied_heating_setpoint_ep1": 23,
  "occupied_cooling_setpoint_ep1": 23,
  "fan_mode": "medium"
}
```

## Benefits of Simplification
1. **Cleaner Code**: Removed 30+ lines of bitmap handling
2. **Easier Debugging**: Single attribute to track instead of two
3. **Same Functionality**: Both required polling anyway
4. **Better UX**: "Running Mode" is clearer than "Running State"
5. **Zigbee Spec Aligned**: Enum is simpler and more standard

## Implementation Notes
- runningMode updated every HVAC status frame (every ~5 seconds from AC)
- Z2M polls every 30 seconds to sync with device
- Values map directly: AC mode → enum → Z2M state
- No data loss: enum covers all needed states (idle/heat/cool/fan)

## Files Modified
- `main/esp_zb_hvac.c` - Removed runningState attribute and bitmap logic
- `zigbee2mqtt_converter.js` - Simplified converter, polling, and exposes
- Updated UI label from "Running State" to "Running Mode"
