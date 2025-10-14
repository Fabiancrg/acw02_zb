# Automated Polling for Unreportable Zigbee Attributes

## The Problem

The ESP-Zigbee stack marks certain Zigbee Cluster Library attributes as **unreportable**:
- `runningMode` (Thermostat cluster) - Current operating state
- `systemMode` (Thermostat cluster) - Target mode setting
- `occupiedHeatingSetpoint` / `occupiedCoolingSetpoint` - Temperature setpoints
- `locationDescription` (Basic cluster) - Error text string
- `fanMode` (Fan Control cluster) - Fan speed setting

These attributes cannot use Zigbee's automatic reporting mechanism (configureReporting fails with `UNREPORTABLE_ATTRIBUTE`). The coordinator must actively **read** them to get current values.

## Solution Implemented

### Three-Tier Polling Strategy

The updated `zigbee2mqtt_converter.js` implements a comprehensive polling strategy:

#### 1. Event-Based Polling (Immediate)
Polls attributes whenever:
- **Device announces** - Device joins/rejoins network
- **Message received** - Piggyback on any device activity
- **Z2M stop** - Read final state before shutdown

```javascript
if (type === 'deviceAnnounce' || type === 'stop' || type === 'message') {
    await pollAttributes();
}
```

#### 2. Periodic Polling (Background)
- **Default interval**: 30 seconds
- **Configurable**: Set via device options in Z2M
- **Ensures fresh data** even when device is idle

```javascript
if (type === 'start') {
    globalThis.acw02PollTimer = setInterval(async () => {
        await pollAttributes();
    }, pollInterval * 1000);
}
```

#### 3. On-Demand Reading
Z2M's built-in "Read" feature allows manual polling anytime via the UI.

## Configuration Options

### Option 1: Use Default Settings (Recommended)
The converter automatically polls every 30 seconds. No configuration needed.

**Advantages:**
- Works immediately after reloading converter
- Balances freshness vs. network traffic
- 30-second delay is acceptable for HVAC control

### Option 2: Adjust Poll Interval
If you want faster or slower updates, you can configure the interval in Z2M's `configuration.yaml`:

```yaml
devices:
  '0x1051dbfffe1c7958':
    friendly_name: 'ACW02_HVAC'
    options:
      state_poll_interval: 15  # Poll every 15 seconds (faster)
      # Or: state_poll_interval: 60  # Poll every 60 seconds (slower, less traffic)
      # Or: state_poll_interval: 0   # Disable periodic polling (event-based only)
```

**Recommended intervals:**
- **15 seconds** - Fast updates, slightly more network traffic
- **30 seconds** (default) - Good balance
- **60 seconds** - Slower updates, minimal network traffic
- **0** - Disable timer polling (relies on event-based polling only)

### Option 3: Home Assistant Automation
You can also create HA automations to refresh specific attributes on triggers:

```yaml
automation:
  - alias: "Poll HVAC state when mode changes"
    trigger:
      - platform: state
        entity_id: climate.acw02_hvac_ep1
        attribute: hvac_mode
    action:
      - service: mqtt.publish
        data:
          topic: "zigbee2mqtt/ACW02_HVAC/get"
          payload: '{"running_state_ep1": ""}'
```

## What Gets Polled

Every poll cycle reads:

### From Thermostat Cluster (`hvacThermostat`)
- `runningMode` → `running_state_ep1` (idle/heat/cool/fan_only)
- `systemMode` → `system_mode_ep1` (off/auto/cool/heat/dry/fan_only)
- `occupiedHeatingSetpoint` → `occupied_heating_setpoint_ep1`
- `occupiedCoolingSetpoint` → `occupied_cooling_setpoint_ep1`

### From Basic Cluster (`genBasic`)
- `locationDesc` → `error_text_ep1` (error messages or empty string)

## Network Impact

**Per Poll Cycle:**
- 2 read requests (one for thermostat, one for basic cluster)
- ~100-200 bytes total network traffic
- Negligible impact on Zigbee network

**At 30-second interval:**
- 120 reads per hour
- ~12-24 KB/hour
- Acceptable for battery-powered coordinator
- Minimal for powered devices

## Verification

After reloading the converter, verify polling is working:

1. **Check Z2M logs** for read operations:
   ```
   debug: zh:controller:endpoint: ZCL command read hvacThermostat runningMode
   debug: zh:controller:endpoint: ZCL command read genBasic locationDesc
   ```

2. **Monitor state updates** in Z2M frontend - attributes should update every 30 seconds

3. **Trigger a change** on the AC:
   - Change mode (heat/cool/off)
   - `running_state_ep1` should update within 30 seconds
   - If device sends any other message, update happens immediately

## Troubleshooting

### Attributes Still Showing Null

**Check converter is loaded:**
```bash
# In Z2M logs, look for:
info: Successfully loaded external converter for 'acw02-z'
```

**Force read manually:**
In Z2M UI: Device page → "Exposes" tab → Click "Read" on any attribute

**Check for errors:**
```bash
# Look for read failures in logs:
error: Failed to read hvacThermostat
```

### High Network Traffic

If polling causes issues:
1. Increase interval to 60+ seconds
2. Disable periodic polling (set to 0)
3. Rely on event-based polling only

### Delayed Updates

If 30 seconds is too slow:
1. Decrease interval to 15 seconds
2. Remember: event-based polling triggers on any message
3. Device is already sending temperature reports every hour

## Alternative: Home Assistant Template Sensors

For more advanced use cases, create HA template sensors that refresh on other triggers:

```yaml
template:
  - trigger:
      - platform: time_pattern
        seconds: "/15"  # Every 15 seconds
      - platform: state
        entity_id: climate.acw02_hvac_ep1
    action:
      - service: mqtt.publish
        data:
          topic: "zigbee2mqtt/ACW02_HVAC/get"
          payload: '{"running_state_ep1": "", "error_text_ep1": ""}'
    sensor:
      - name: "HVAC Running State (Fresh)"
        state: "{{ state_attr('climate.acw02_hvac_ep1', 'running_state_ep1') }}"
```

## Summary

✅ **Automatic periodic polling** implemented in converter (default: 30s)  
✅ **Event-based polling** on device activity (instant updates)  
✅ **Configurable interval** via Z2M device options  
✅ **Minimal network impact** (~12 KB/hour at 30s interval)  
✅ **No Home Assistant configuration required** (works out of the box)  

The polling mechanism is transparent and automatic. Just reload the converter and attributes will stay current!
