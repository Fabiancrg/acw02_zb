# Running Mode Converter Fix

## Problem
After removing `runningState` bitmap and simplifying to `runningMode` enum, Z2M converter failed to load:

```
error: z2m: Invalid external converter 'esp32c6_hvac.js' was ignored
(0, node_assert_1.default)(allowed.includes(m))
```

## Root Cause
Used `.withRunningMode()` in exposes section, but this method **doesn't exist** in Z2M's exposes API!

The valid climate methods are:
- `.withSystemMode()`  ✅ EXISTS
- `.withRunningState()` ✅ EXISTS  
- `.withRunningMode()`  ❌ DOES NOT EXIST

## Solution
Keep using `.withRunningState()` in exposes (standard Z2M API), but output property as `running_state_ep1`.

### What Changed
**Before (broken):**
```javascript
exposes: [
    e.climate()
        .withRunningMode(['idle', 'heat', 'cool', 'fan_only'])  // ❌ Invalid method
        .withEndpoint('ep1'),
]

// Converter output
result.running_mode_ep1 = modeMap[msg.data.runningMode] || 'idle';
```

**After (fixed):**
```javascript
exposes: [
    e.climate()
        .withRunningState(['idle', 'heat', 'cool', 'fan_only'])  // ✅ Valid method
        .withEndpoint('ep1'),
]

// Converter output (matches expose name)
result.running_state_ep1 = modeMap[msg.data.runningMode] || 'idle';
```

## Implementation Details

### Firmware Side (Unchanged)
- Still uses **runningMode (0x001E)** enum attribute
- Removed runningState (0x0029) bitmap
- Values: 0x00=idle, 0x03=cool, 0x04=heat, 0x07=fan

### Z2M Converter Side
1. **Exposes**: Uses `.withRunningState()` (Z2M's standard API)
2. **Converter**: Reads `runningMode` from Zigbee, outputs as `running_state_ep1`
3. **Polling**: Reads `runningMode` attribute (unreportable)
4. **MQTT**: Publishes `running_state_ep1` with values: idle/heat/cool/fan_only

### Property Name Mapping
```
Zigbee Attribute → Converter Logic → MQTT Property → UI Display
runningMode       → modeMap         → running_state_ep1 → "Running State"
(0x001E enum)       (0→idle, etc)     (Z2M convention)     (Home Assistant)
```

## Why This Works
Z2M's `.withRunningState()` is just metadata telling the UI what values are valid. The actual property name in MQTT can be anything (`running_state_ep1`), as long as the converter outputs it correctly.

The expose says "this climate device has a running_state property with values [idle, heat, cool, fan_only]", and our converter delivers exactly that by reading the Zigbee `runningMode` attribute.

## Testing
After this fix:
1. Z2M should load converter without errors ✅
2. `running_state_ep1` should appear in MQTT ✅
3. Values should be: idle/heat/cool/fan_only ✅
4. UI should show "Running State" label ✅
5. Polling should update every 30 seconds ✅

## Note on Naming
- **Firmware**: Uses `runningMode` (Zigbee spec attribute name)
- **MQTT**: Uses `running_state_ep1` (Z2M convention with endpoint suffix)
- **UI**: Shows "Running State" (Home Assistant label)

This is standard practice - the underlying Zigbee attribute name doesn't have to match the MQTT property name.
