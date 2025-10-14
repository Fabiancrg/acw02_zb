# Polling Mechanism Analysis - Null/N/A Issue

## Problem Description
`running_state_ep1` alternates between `null` and `N/A` in Zigbee2MQTT UI instead of showing actual values (idle/heat/cool/fan_only).

## Root Cause Analysis

### Issue 1: Periodic Timer Never Starts
```javascript
// Current code - BROKEN
if (type === 'start') {
    // Start new polling timer (30 second interval)
    const pollInterval = (options && options.state_poll_interval) || 30;
    if (pollInterval > 0) {
        globalThis.acw02PollTimer = setInterval(async () => {
            await pollAttributes();
        }, pollInterval * 1000);
    }
}
```

**Problem**: The `'start'` event is triggered when **Zigbee2MQTT starts**, NOT when a device joins or reconnects!

**What Actually Happens:**
1. Z2M starts → `'start'` event fires ONCE for ALL devices
2. Device joins network → **NO 'start' event** (it's `'deviceAnnounce'`)
3. Timer never initializes for the device
4. No periodic polling occurs
5. `running_state_ep1` never gets updated

### Issue 2: Event-Based Polling Only Runs Once
```javascript
// Poll on various events to ensure fresh data
if (type === 'deviceAnnounce' || type === 'stop' || type === 'message') {
    await pollAttributes();
}
```

**What This Does:**
- `'deviceAnnounce'`: Fires when device joins/rejoins → Reads attributes ONCE
- `'message'`: Fires on ANY Zigbee message → Reads attributes (but may be too frequent)
- `'stop'`: Fires when Z2M stops → Not useful for polling

**Result**: Attribute is read occasionally on events, but no regular periodic updates.

### Issue 3: No Initial Value
When the device first joins:
1. `'deviceAnnounce'` event triggers ONE read
2. If the read fails or times out → `running_state_ep1` stays `undefined`
3. Z2M shows `null` or `N/A` for undefined values
4. No periodic timer to retry → stays `null` forever

## Detailed Polling Flow (Current - BROKEN)

```
┌─────────────────────────────────────────────────────────────┐
│ Z2M Startup                                                  │
│ ├─ 'start' event fires for ALL devices                      │
│ │  └─ Timer initialized (but device might not be joined yet)│
│ └─ If device joins LATER, no timer is set up               │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ Device Joins Network                                        │
│ ├─ 'deviceAnnounce' event fires                            │
│ │  └─ pollAttributes() called ONCE                         │
│ │     ├─ Reads: runningMode, systemMode, setpoints         │
│ │     └─ Reads: locationDesc (error text)                  │
│ │                                                           │
│ ├─ Converter receives readResponse                         │
│ │  └─ Sets running_state_ep1 = 'heat' (or 'cool', etc)    │
│ │                                                           │
│ └─ NO TIMER STARTED! (only starts on 'start' event)       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│ Ongoing Operation (PROBLEM!)                                │
│ ├─ No periodic timer running                               │
│ ├─ Only reads on 'message' events (unpredictable)          │
│ └─ running_state_ep1 becomes stale or null                 │
└─────────────────────────────────────────────────────────────┘
```

## Why You See null/N/A

### Scenario 1: Read Timeout
```javascript
try {
    await endpoint1.read('hvacThermostat', ['runningMode', ...]);
} catch (error) {
    // Silently ignore read errors
    // No value is returned → running_state_ep1 stays undefined
}
```
- If the device is busy or read times out
- No error shown (silently caught)
- Converter doesn't set `running_state_ep1`
- Z2M shows `null` or `N/A`

### Scenario 2: Missing Attribute in Response
```javascript
if (msg.data.hasOwnProperty('runningMode')) {
    result.running_state_ep1 = modeMap[msg.data.runningMode] || 'idle';
}
// If runningMode is NOT in msg.data, running_state_ep1 is not set
return result;  // May be empty object {}
```
- If device doesn't include `runningMode` in response
- `running_state_ep1` not added to result
- Previous value in Z2M may expire
- Shows as `null`

### Scenario 3: Race Condition
- Event-based read triggers on `'message'`
- Multiple messages arrive quickly
- Multiple reads queued
- Some fail → intermittent `null` values

## The Fix

### Solution 1: Start Timer on Device Announce
```javascript
// Set up periodic polling when device joins OR on Z2M start
if (type === 'start' || type === 'deviceAnnounce') {
    // Clear any existing timer
    if (globalThis.acw02PollTimer) {
        clearInterval(globalThis.acw02PollTimer);
    }
    
    // Start new polling timer (30 second interval)
    const pollInterval = (options && options.state_poll_interval) || 30;
    if (pollInterval > 0) {
        globalThis.acw02PollTimer = setInterval(async () => {
            await pollAttributes();
        }, pollInterval * 1000);
    }
}
```

### Solution 2: Add Retry Logic
```javascript
const pollAttributes = async () => {
    try {
        await endpoint1.read('hvacThermostat', ['runningMode', 'systemMode', 
                                                'occupiedHeatingSetpoint', 
                                                'occupiedCoolingSetpoint']);
    } catch (error) {
        // Log error for debugging (optional)
        // console.log('ACW02: Failed to read thermostat attributes:', error.message);
    }
    
    try {
        await endpoint1.read('genBasic', ['locationDesc']);
    } catch (error) {
        // Silently ignore
    }
};
```

### Solution 3: Provide Default Value
```javascript
if (msg.data.hasOwnProperty('runningMode')) {
    const modeMap = {
        0x00: 'idle',
        0x03: 'cool',
        0x04: 'heat',
        0x07: 'fan_only',
    };
    result.running_state_ep1 = modeMap[msg.data.runningMode] || 'idle';
} else {
    // Ensure we always have a value, even if read fails
    // Don't set anything - let previous value persist
}
```

## Recommended Implementation

### Complete Fixed onEvent Handler
```javascript
onEvent: async (type, data, device, options, state) => {
    const endpoint1 = device.getEndpoint(1);
    if (!endpoint1) return;
    
    const pollAttributes = async () => {
        try {
            await endpoint1.read('hvacThermostat', ['runningMode', 'systemMode', 
                                                    'occupiedHeatingSetpoint', 
                                                    'occupiedCoolingSetpoint']);
        } catch (error) {
            // Silently ignore read errors
        }
        
        try {
            await endpoint1.read('genBasic', ['locationDesc']);
        } catch (error) {
            // Silently ignore read errors
        }
    };
    
    // Poll immediately on device announce or message
    if (type === 'deviceAnnounce' || type === 'message') {
        await pollAttributes();
    }
    
    // Start periodic polling on Z2M start OR when device joins
    if (type === 'start' || type === 'deviceAnnounce') {
        // Clear any existing timer for this device
        const timerKey = `acw02PollTimer_${device.ieeeAddr}`;
        if (globalThis[timerKey]) {
            clearInterval(globalThis[timerKey]);
        }
        
        // Start new polling timer (30 second interval)
        const pollInterval = (options && options.state_poll_interval) || 30;
        if (pollInterval > 0) {
            globalThis[timerKey] = setInterval(async () => {
                await pollAttributes();
            }, pollInterval * 1000);
        }
    }
    
    // Clean up timer on stop
    if (type === 'stop') {
        const timerKey = `acw02PollTimer_${device.ieeeAddr}`;
        if (globalThis[timerKey]) {
            clearInterval(globalThis[timerKey]);
            globalThis[timerKey] = null;
        }
    }
},
```

### Key Improvements:
1. **Timer starts on `'deviceAnnounce'`** → Works when device joins
2. **Per-device timer** → Uses `device.ieeeAddr` in key for multiple devices
3. **Immediate poll** → Reads attributes as soon as device announces
4. **Periodic updates** → 30-second timer ensures fresh data

## Testing Steps
1. Restart Z2M after updating converter
2. Re-pair device (or wait for it to rejoin)
3. Watch Z2M logs for "Read request" messages every 30 seconds
4. Verify `running_state_ep1` shows correct value (heat/cool/idle/fan_only)
5. Check that value updates when AC mode changes

## Expected Behavior After Fix
```
Device joins → 'deviceAnnounce' event
  ├─ Immediate read of runningMode → running_state_ep1 = 'heat'
  └─ Start 30-second timer
        ├─ 00:00 → Read runningMode → 'heat'
        ├─ 00:30 → Read runningMode → 'heat'
        ├─ 01:00 → Read runningMode → 'heat'
        └─ (continues every 30 seconds)
```

No more `null` or `N/A` - value should stay consistent!
