# Polling Timer Fix - Detailed Explanation

## Problem Summary
`running_state_ep1` was showing **null/N/A** intermittently instead of actual values (idle/heat/cool/fan_only).

## Root Cause
The periodic polling timer **never started** after device join because:
- Timer only initialized on `'start'` event (Z2M startup)
- When device joined network, it triggered `'deviceAnnounce'` event
- **No timer setup on `'deviceAnnounce'`** → No periodic polling
- Result: Only one-time reads on events, no regular updates

## What Was Wrong

### Before (Broken Code):
```javascript
// Set up periodic polling (every 30 seconds by default)
if (type === 'start') {  // ❌ ONLY runs when Z2M starts!
    if (globalThis.acw02PollTimer) {
        clearInterval(globalThis.acw02PollTimer);
    }
    
    const pollInterval = (options && options.state_poll_interval) || 30;
    if (pollInterval > 0) {
        globalThis.acw02PollTimer = setInterval(async () => {
            await pollAttributes();
        }, pollInterval * 1000);
    }
}
```

### Execution Flow - BROKEN:
```
1. Zigbee2MQTT Starts
   └─ 'start' event fires for ALL devices
      └─ Timer initialized (but device may not be paired yet)

2. Device Joins Network Later
   └─ 'deviceAnnounce' event fires
      ├─ pollAttributes() called ONCE
      └─ NO TIMER STARTED ❌
      
3. Ongoing Operation
   └─ No periodic timer
   └─ Only sporadic reads on 'message' events
   └─ running_state_ep1 becomes null/stale
```

### Why Multiple Devices Failed:
```javascript
globalThis.acw02PollTimer  // ❌ Single global timer for ALL devices!
```
- If you had 2+ ACW02 devices, they would overwrite each other's timer
- Last device to join would cancel previous timers
- Only most recent device gets polled

## What Was Fixed

### After (Fixed Code):
```javascript
// Set up periodic polling when Z2M starts OR when device joins network
if (type === 'start' || type === 'deviceAnnounce') {  // ✅ Works for both scenarios!
    // Use device-specific timer key to support multiple devices
    const timerKey = `acw02PollTimer_${device.ieeeAddr}`;  // ✅ Per-device timer
    
    // Clear any existing timer for this device
    if (globalThis[timerKey]) {
        clearInterval(globalThis[timerKey]);
    }
    
    // Start new polling timer (default: 30 seconds, configurable)
    const pollInterval = (options && options.state_poll_interval) || 30;
    if (pollInterval > 0) {
        globalThis[timerKey] = setInterval(async () => {
            await pollAttributes();
        }, pollInterval * 1000);
    }
}
```

### Execution Flow - FIXED:
```
1. Zigbee2MQTT Starts
   └─ 'start' event fires for already-paired devices
      └─ Timer starts for each device

2. Device Joins Network
   └─ 'deviceAnnounce' event fires
      ├─ pollAttributes() called immediately → running_state_ep1 = 'heat'
      └─ Timer started ✅
      
3. Ongoing Operation (Every 30 seconds)
   └─ 00:00 → pollAttributes() → running_state_ep1 = 'heat'
   └─ 00:30 → pollAttributes() → running_state_ep1 = 'heat'
   └─ 01:00 → pollAttributes() → running_state_ep1 = 'cool' (AC mode changed)
   └─ 01:30 → pollAttributes() → running_state_ep1 = 'cool'
   └─ (continues forever)
```

## Key Improvements

### 1. Timer Starts on Device Join
```javascript
if (type === 'start' || type === 'deviceAnnounce') {
    // Starts timer whether device is:
    // - Already paired when Z2M starts ('start')
    // - Joins after Z2M is running ('deviceAnnounce')
}
```

### 2. Per-Device Timers
```javascript
const timerKey = `acw02PollTimer_${device.ieeeAddr}`;
// Example keys:
// - acw02PollTimer_0x00124b001234abcd (Device 1)
// - acw02PollTimer_0x00124b005678ef01 (Device 2)
// Each device gets its own independent timer
```

### 3. Immediate Poll on Join
```javascript
if (type === 'deviceAnnounce' || type === 'message') {
    await pollAttributes();  // Don't wait 30 seconds, poll NOW
}
```

### 4. Proper Cleanup
```javascript
if (type === 'stop') {
    const timerKey = `acw02PollTimer_${device.ieeeAddr}`;
    if (globalThis[timerKey]) {
        clearInterval(globalThis[timerKey]);
        globalThis[timerKey] = null;  // Free memory
    }
}
```

## What Gets Polled

Every 30 seconds (configurable), the converter reads:

### From Thermostat Cluster (hvacThermostat):
```javascript
await endpoint1.read('hvacThermostat', [
    'runningMode',              // → running_state_ep1 (heat/cool/idle/fan_only)
    'systemMode',               // → system_mode_ep1 (off/auto/heat/cool/dry/fan_only)
    'occupiedHeatingSetpoint',  // → occupied_heating_setpoint_ep1 (16-31°C)
    'occupiedCoolingSetpoint',  // → occupied_cooling_setpoint_ep1 (16-31°C)
]);
```

### From Basic Cluster (genBasic):
```javascript
await endpoint1.read('genBasic', [
    'locationDesc'  // → error_text (error message string or empty)
]);
```

## Expected Z2M Log Output

After the fix, you should see in Z2M logs:

```
[2025-10-14 17:00:00] info:  z2m: Device 'ACW02_Living_Room' announced
[2025-10-14 17:00:00] debug: zh:controller: Read 'hvacThermostat'['runningMode','systemMode',...] from 'ACW02_Living_Room'
[2025-10-14 17:00:01] debug: zh:controller: Received 'readResponse' from 'ACW02_Living_Room'
[2025-10-14 17:00:01] info:  z2m:mqtt: MQTT publish: 'zigbee2mqtt/ACW02_Living_Room' payload '{"running_state_ep1":"heat",...}'

[2025-10-14 17:00:30] debug: zh:controller: Read 'hvacThermostat'['runningMode',...] (periodic poll)
[2025-10-14 17:00:31] info:  z2m:mqtt: MQTT publish: 'zigbee2mqtt/ACW02_Living_Room' payload '{"running_state_ep1":"heat",...}'

[2025-10-14 17:01:00] debug: zh:controller: Read 'hvacThermostat'['runningMode',...] (periodic poll)
[2025-10-14 17:01:01] info:  z2m:mqtt: MQTT publish: 'zigbee2mqtt/ACW02_Living_Room' payload '{"running_state_ep1":"heat",...}'
```

Notice reads every 30 seconds!

## Testing Steps

1. **Copy updated converter** to Z2M data directory
2. **Restart Zigbee2MQTT** to reload converter
3. **Watch Z2M logs** (set log level to `debug` if needed)
4. **Verify initial poll** when device announces
5. **Count 30 seconds** and confirm periodic read appears
6. **Check MQTT payload** - `running_state_ep1` should show correct value
7. **Change AC mode** on indoor unit
8. **Wait max 30 seconds** - running_state_ep1 should update

## Configuration Option

You can change the poll interval in Z2M's `configuration.yaml`:

```yaml
devices:
  '0x00124b001234abcd':
    friendly_name: 'ACW02_Living_Room'
    state_poll_interval: 15  # Poll every 15 seconds instead of 30
```

Or disable periodic polling entirely (not recommended):
```yaml
    state_poll_interval: 0  # Only polls on events
```

## Multiple Devices Support

The fix supports multiple ACW02 devices:
- Each device gets its own timer: `acw02PollTimer_<IEEE_ADDR>`
- Timers run independently
- No interference between devices
- Each polls every 30 seconds on its own schedule

## Why This Matters

### Before Fix:
```
User changes AC mode from HEAT to COOL
  ↓
ESP32 updates runningMode = 0x03 (cool)
  ↓
No periodic polling...
  ↓
running_state_ep1 stays "heat" OR shows null/N/A
  ↓
Home Assistant shows wrong state
  ↓
Automations don't trigger ❌
```

### After Fix:
```
User changes AC mode from HEAT to COOL
  ↓
ESP32 updates runningMode = 0x03 (cool)
  ↓
Z2M polls every 30 seconds
  ↓
Within 30 seconds: running_state_ep1 updates to "cool"
  ↓
MQTT publishes updated state
  ↓
Home Assistant shows correct state
  ↓
Automations trigger correctly ✅
```

## Performance Impact

- **Zigbee Traffic**: Minimal (2 read requests every 30 seconds per device)
- **CPU Usage**: Negligible (async I/O, non-blocking)
- **Memory**: ~100 bytes per device (timer object)
- **Battery**: N/A (mains-powered device)

## Summary

✅ **Fixed**: Timer now starts on `'deviceAnnounce'` event  
✅ **Fixed**: Per-device timers using `device.ieeeAddr`  
✅ **Fixed**: Immediate poll on device join  
✅ **Result**: `running_state_ep1` updates reliably every 30 seconds  
✅ **Result**: No more null/N/A values  

The polling mechanism is now **robust and reliable**! 🎉
