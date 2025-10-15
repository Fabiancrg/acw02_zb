# Polling Solution: meta.poll API

## Problem Analysis

After extensive research in the Koenkk/zigbee-herdsman-converters GitHub repository, I discovered the root cause of the polling issues:

### What Doesn't Work in External Converters:
1. ❌ **`modernExtend.poll()`** - Only available for internal converters (official repository)
2. ❌ **`tuya.modernExtend.electricityMeasurementPoll()`** - Tuya wrapper, also internal-only
3. ❌ **Module-level `setInterval` timers** - Don't work reliably in Z2M's external converter context
4. ❌ **Custom `pollTimers` object with `setupPolling/teardownPolling`** - Timers don't fire consistently

### What SHOULD Work in External Converters:
✅ **`meta.poll` object** - Native Z2M API for external converters

## Research Findings

### Internal Converter Pattern (Tuya TS011F_plug_3):
```javascript
// From src/lib/tuya.ts lines 2069-2087
modernExtend.poll({
    key: "measurement",
    option: exposes.options.measurement_poll_interval(),
    defaultIntervalSeconds: 60,
    poll: async (device) => {
        const endpoint = device.getEndpoint(1);
        await endpoint.read("haElectricalMeasurement", ["rmsVoltage", "rmsCurrent", "activePower"]);
        await endpoint.read("seMetering", ["currentSummDelivered"]);
    }
})
```

**This is for INTERNAL converters only** - requires access to `modernExtend` module.

### External Converter Pattern (meta.poll):
```javascript
// Our implementation
meta: {
    multiEndpoint: true,
    poll: {
        interval: (device) => device.options?.state_poll_interval ?? 30,
        read: async (device) => {
            const endpoint1 = device.getEndpoint(1);
            await endpoint1.read('hvacThermostat', ['runningMode', 'systemMode']);
            await endpoint1.read('genBasic', ['locationDesc']);
        },
    },
},
```

This is the **correct API for external converters**.

## Changes Made

### Removed (Lines 1-21):
```javascript
// ❌ REMOVED: Module-level polling infrastructure
const pollTimers = {};

const setupPolling = (device, intervalSeconds, readFn) => {
    const key = `poll_${device.ieeeAddr}`;
    if (pollTimers[key]) clearInterval(pollTimers[key]);
    if (intervalSeconds > 0) {
        pollTimers[key] = setInterval(() => {
            readFn(device).catch(() => {});
        }, intervalSeconds * 1000);
    }
};

const teardownPolling = (device) => {
    const key = `poll_${device.ieeeAddr}`;
    if (pollTimers[key]) {
        clearInterval(pollTimers[key]);
        delete pollTimers[key];
    }
};
```

**Reason**: Custom timers don't work reliably in Z2M's external converter context.

### Simplified onEvent (Lines 461-479):
```javascript
// ✅ SIMPLIFIED: Only immediate polls on events
onEvent: async (type, data, device, options) => {
    // Perform an immediate poll on device announce or message to refresh state
    if (['deviceAnnounce', 'message'].includes(type)) {
        const endpoint1 = device.getEndpoint(1);
        if (endpoint1) {
            try {
                await endpoint1.read('hvacThermostat', [
                    'runningMode', 'systemMode',
                    'occupiedHeatingSetpoint', 'occupiedCoolingSetpoint',
                ]);
            } catch (error) {
                // Silently ignore errors on immediate poll
            }
            try {
                await endpoint1.read('genBasic', ['locationDesc']);
            } catch (error) {
                // Silently ignore errors
            }
        }
    }
},
```

**Purpose**: Provides immediate refresh when device sends messages or rejoins network.

### Kept meta.poll (Lines 286-318):
```javascript
// ✅ KEPT: Native Z2M polling API
meta: {
    multiEndpoint: true,
    poll: {
        // Poll interval in seconds (default 30, overridable via device options)
        interval: (device) => device.options?.state_poll_interval ?? 30,
        read: async (device) => {
            const endpoint1 = device.getEndpoint(1);
            if (!endpoint1) return;
            
            // Read unreportable thermostat attributes
            try {
                await endpoint1.read('hvacThermostat', [
                    'runningMode',
                    'systemMode',
                    'occupiedHeatingSetpoint',
                    'occupiedCoolingSetpoint',
                ]);
            } catch (error) {
                // Silently ignore read errors (device may be offline or busy)
            }
            
            // Read error text from locationDescription
            try {
                await endpoint1.read('genBasic', ['locationDesc']);
            } catch (error) {
                // Silently ignore read errors
            }
        },
    },
},
```

**This is the core polling mechanism** - relies on Z2M's built-in polling engine.

## How It Works

### 1. Automatic Polling (meta.poll):
- **Z2M's polling engine** calls `meta.poll.read()` every `interval` seconds
- **Interval calculation**: `device.options?.state_poll_interval ?? 30`
  - Defaults to 30 seconds
  - Can be overridden in Z2M device settings: `state_poll_interval: 60`
- **Attributes polled**:
  - `runningMode` (0x001E) - Shows heat/cool/idle/fan state
  - `systemMode` (0x001C) - Current system mode
  - `occupiedHeatingSetpoint` - Target temp when heating
  - `occupiedCoolingSetpoint` - Target temp when cooling
  - `locationDesc` (0x0010) - Error text string

### 2. Event-Based Immediate Poll (onEvent):
- **Triggers**: When device sends ANY Zigbee message or rejoins network
  - `'deviceAnnounce'` - Device reconnects/restarts
  - `'message'` - Device sends attribute report or command
- **Purpose**: Provides instant state refresh without waiting for poll timer
- **Example flow**:
  1. User changes AC mode via IR remote → Firmware sends systemMode report
  2. Z2M receives 'message' event → onEvent triggers
  3. Immediate read of runningMode → UI updates instantly
  4. Next scheduled poll happens in <30 seconds anyway

## Configuration Option

Device option exposed in Z2M UI:

```javascript
options: [
    {
        name: 'state_poll_interval',
        type: 'number',
        description: 'Interval (in seconds) to poll unreportable attributes (runningMode, error_text). Default: 30 seconds. Set to 0 to disable polling.',
        default: 30,
    },
],
```

### Usage:
In Z2M UI: **Devices → ACW02-ZB → Settings (specific) → Add**
```yaml
state_poll_interval: 60  # Poll every 60 seconds instead of 30
```

Or disable polling entirely:
```yaml
state_poll_interval: 0   # Disable automatic polling
```

## Testing Plan

### 1. Restart Z2M
```bash
sudo systemctl restart zigbee2mqtt
```

### 2. Check Z2M logs for poll messages:
```bash
journalctl -u zigbee2mqtt -f | grep -i poll
```

**Expected output every 30 seconds:**
```
[2024-XX-XX XX:XX:XX] info: Polling 'ACW02-ZB' (0x00124b001234abcd)
```

### 3. Test polling workflow:
1. **Initial state**: AC off, running_state_ep1 = "N/A"
2. **Turn on AC** via IR remote → Set to HEAT mode
3. **Wait <30 seconds** → Check Z2M UI
4. **Expected**: `running_state_ep1` = "heat"

### 4. Test immediate poll:
1. **Change systemMode** via Z2M → Set to "cool"
2. **Check immediately** (don't wait 30 sec)
3. **Expected**: `running_state_ep1` updates to "cooling" within 1-2 seconds
   - Reason: onEvent immediate poll triggered by systemMode write

### 5. Test error diagnostics:
1. **Trigger AC fault** (e.g., remove power to outdoor unit)
2. **Wait for error** to appear on indoor unit display
3. **Wait <30 seconds** → Check Z2M UI
4. **Expected**: `error_text_ep1` = "FAULT 0xXX: Description"

## Troubleshooting

### If polling still doesn't work:

1. **Check Z2M version**:
```bash
cat /opt/zigbee2mqtt/package.json | grep version
```
Ensure version ≥ 1.30.0 (meta.poll support added in mid-2023)

2. **Enable debug logging**:
Edit `/opt/zigbee2mqtt/data/configuration.yaml`:
```yaml
advanced:
  log_level: debug
```

3. **Check for poll errors**:
```bash
journalctl -u zigbee2mqtt -f | grep -E "(poll|0x00124b|ACW02)"
```

4. **Manual test**:
In Z2M UI → **Devices → ACW02-ZB → Dev console**:
```json
Cluster: hvacThermostat
Attribute: runningMode
Type: Read
```
If manual reads work but automatic polling doesn't, the issue is in Z2M's polling engine.

### If meta.poll is not supported:

**Alternative**: Copy converter to Koenkk repo fork and use `modernExtend.poll()`:
```javascript
const m = require('../lib/modernExtend');

extend: [
    m.poll({
        key: 'thermostat',
        option: exposes.options.measurement_poll_interval(),
        defaultIntervalSeconds: 30,
        poll: async (device) => {
            await device.getEndpoint(1).read('hvacThermostat', ['runningMode']);
            await device.getEndpoint(1).read('genBasic', ['locationDesc']);
        }
    })
],
```

Then submit PR to Koenkk repo for official inclusion.

## Expected Behavior After Fix

### Automatic Updates:
- **Every 30 seconds**: All unreportable attributes refresh
  - `running_state_ep1`: Updates based on AC operational state
  - `error_text_ep1`: Updates if fault occurs
  - Setpoints: Refresh to catch manual AC changes

### Instant Updates:
- **When you change settings via Z2M**: Immediate poll on 'message' event
- **When AC changes internally**: Immediate poll if firmware sends report
- **When device reconnects**: Immediate poll on 'deviceAnnounce'

### User Experience:
1. **Turn on AC** via IR remote (heat mode)
2. **Within 30 seconds**: Z2M UI shows `running_state_ep1: "heat"`
3. **Change to cool** via Z2M UI
4. **Within 2 seconds**: UI updates to `running_state_ep1: "cooling"`
5. **Trigger error** on AC unit
6. **Within 30 seconds**: UI shows `error_text_ep1: "FAULT 0x04: Low voltage protection"`

## Summary

The solution is to **rely entirely on Z2M's native `meta.poll` API** instead of attempting custom timer management in external converters.

**Key changes:**
- ❌ Removed custom pollTimers infrastructure
- ❌ Removed setInterval-based polling in onEvent
- ✅ Kept meta.poll for automatic polling (30s interval)
- ✅ Kept immediate poll in onEvent for responsive updates
- ✅ Simplified code from 522 lines → 496 lines

**Next step**: Test with Zigbee2MQTT restart to verify polling now works correctly.
