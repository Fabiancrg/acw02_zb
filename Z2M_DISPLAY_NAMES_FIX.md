# Zigbee2MQTT Converter - Display Names Fix

## Problem

When using `e.switch().withEndpoint('ep2').withDescription('Eco mode')`, Zigbee2MQTT shows:
```
State (Endpoint: ep2)
On/off state of the switch
```

The custom description is not displayed because `.withDescription()` doesn't change the property name, only adds metadata.

## Solution

Use `exposes.binary()` with custom property names instead of generic `e.switch()`:

### Before (Generic):
```javascript
e.switch().withEndpoint('ep2').withDescription('Eco mode'),
e.switch().withEndpoint('ep3').withDescription('Swing mode'),
e.switch().withEndpoint('ep4').withDescription('Display on/off'),
```

### After (Named Properties):
```javascript
exposes.binary('eco_mode', exposes.access.ALL, 'ON', 'OFF')
    .withDescription('Eco mode')
    .withEndpoint('ep2'),
exposes.binary('swing_mode', exposes.access.ALL, 'ON', 'OFF')
    .withDescription('Swing mode')
    .withEndpoint('ep3'),
exposes.binary('display', exposes.access.ALL, 'ON', 'OFF')
    .withDescription('Display on/off')
    .withEndpoint('ep4'),
```

## What This Changes

### In Z2M UI:
**Before:**
- State (Endpoint: ep2) ← Generic name
- State (Endpoint: ep3) ← Generic name
- State (Endpoint: ep4) ← Generic name

**After:**
- **Eco mode** ← Custom property name
- **Swing mode** ← Custom property name
- **Display** ← Custom property name

### In MQTT Topics:
**Before:**
```
zigbee2mqtt/ACW02-HVAC/ep2/state
zigbee2mqtt/ACW02-HVAC/ep3/state
zigbee2mqtt/ACW02-HVAC/ep4/state
```

**After:**
```
zigbee2mqtt/ACW02-HVAC/eco_mode
zigbee2mqtt/ACW02-HVAC/swing_mode
zigbee2mqtt/ACW02-HVAC/display
```

### In JSON Payload:
**Before:**
```json
{
  "state_ep2": "ON",
  "state_ep3": "OFF",
  "state_ep4": "ON"
}
```

**After:**
```json
{
  "eco_mode": "ON",
  "swing_mode": "OFF",
  "display": "ON"
}
```

## Updated Endpoint Mapping

Added alternative endpoint names for better compatibility:

```javascript
endpoint: (device) => {
    return {
        'ep1': 1,        // Main thermostat
        'ep2': 2,        // Eco mode switch
        'ep3': 3,        // Swing switch
        'ep4': 4,        // Display switch
        'eco_mode': 2,   // Alternative name
        'swing_mode': 3, // Alternative name
        'display': 4,    // Alternative name
    };
},
```

This allows Z2M to map both the generic endpoint names and the custom property names.

## How to Apply

1. **Copy updated converter** to Z2M data directory
2. **Restart Zigbee2MQTT**
3. **Re-pair device** OR click **"Reconfigure"** in Z2M UI for the device
4. **Refresh browser** to see updated UI

## Expected Result

In Zigbee2MQTT UI, you should now see:

```
Climate
  - System mode: [off | auto | cool | heat | dry | fan_only]
  - Current temperature: 25°C
  - Target temperature (heating): 20°C
  - Target temperature (cooling): 24°C
  - Running state: idle

Fan mode
  - [off | low | medium | high | on | auto | smart]

Eco mode
  - [ON | OFF]

Swing mode
  - [ON | OFF]

Display
  - [ON | OFF]
```

## Home Assistant Integration

If using Z2M with Home Assistant, the entities will be:

```yaml
climate.acw02_hvac_zb          # Main thermostat
select.acw02_hvac_zb_fan_mode  # Fan speed
switch.acw02_hvac_zb_eco_mode  # Eco mode switch
switch.acw02_hvac_zb_swing_mode # Swing switch
switch.acw02_hvac_zb_display   # Display switch
```

## Testing

### Control via Z2M UI:
1. Toggle **Eco mode** switch
2. Toggle **Swing mode** switch
3. Toggle **Display** switch

### Control via MQTT:
```bash
# Turn on eco mode
mosquitto_pub -t "zigbee2mqtt/ACW02-HVAC/set" -m '{"eco_mode":"ON"}'

# Turn off swing mode
mosquitto_pub -t "zigbee2mqtt/ACW02-HVAC/set" -m '{"swing_mode":"OFF"}'

# Turn on display
mosquitto_pub -t "zigbee2mqtt/ACW02-HVAC/set" -m '{"display":"ON"}'
```

### Check State via MQTT:
```bash
mosquitto_sub -t "zigbee2mqtt/ACW02-HVAC" -v
```

Expected output:
```json
{
  "system_mode": "cool",
  "local_temperature": 25,
  "occupied_cooling_setpoint": 24,
  "occupied_heating_setpoint": 20,
  "fan_mode": "auto",
  "eco_mode": "ON",
  "swing_mode": "OFF",
  "display": "ON"
}
```

## Summary

✅ Changed from generic `e.switch()` to named `exposes.binary()`
✅ Custom property names: `eco_mode`, `swing_mode`, `display`
✅ Better UI display in Zigbee2MQTT
✅ More intuitive MQTT topics
✅ Cleaner JSON payloads
✅ Compatible with Home Assistant auto-discovery

**The controls will now show with proper names instead of generic "State (Endpoint: ep2)"!**
