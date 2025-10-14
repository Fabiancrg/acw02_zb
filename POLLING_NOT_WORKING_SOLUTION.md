# Z2M Polling Issue - Timer Not Working

## Symptoms
Your testing revealed the REAL problem:
- Manual dev console reads of `runningMode` work perfectly ✅
- **Automatic polling does NOT work** ❌
- Timer-based polling never fires
- Values only update when manually read

## Root Cause
**Zigbee2MQTT's `onEvent` handler with `setInterval` is unreliable in external converters!**

The `onEvent` function runs in Z2M's Node.js context, but:
1. `globalThis` timers may not persist across Z2M internal restarts
2. External converter context might be sandboxed
3. Timer callbacks might not execute in the right async context
4. Z2M might clear external timers on reload

## Alternative Solutions

### Option 1: Home Assistant Automation (RECOMMENDED)
Since manual reads work, use Home Assistant to poll automatically:

```yaml
# configuration.yaml
automation:
  - alias: 'Poll ACW02 Running Mode'
    trigger:
      - platform: time_pattern
        seconds: '/30'  # Every 30 seconds
    action:
      - service: mqtt.publish
        data:
          topic: 'zigbee2mqtt/ACW02_Living_Room/get'
          payload: '{"running_state_ep1": ""}'
```

This tells Z2M to read the attribute every 30 seconds.

### Option 2: Z2M Device-Specific Options
In Z2M's `configuration.yaml`, enable polling per-device:

```yaml
devices:
  '0x00124b001234abcd':  # Replace with your device IEEE address
    friendly_name: 'ACW02_Living_Room'
    poll: true
    poll_interval: 30  # seconds
```

But this might not work for custom converters...

### Option 3: Node-RED Flow
Create a Node-RED flow that sends MQTT messages to poll:

```javascript
[Every 30 seconds] → [MQTT Out]
  Topic: zigbee2mqtt/ACW02_Living_Room/get
  Payload: {"running_state_ep1": ""}
```

### Option 4: Shell Script with Cron
Create a simple polling script:

```bash
#!/bin/bash
# poll_acw02.sh
while true; do
  mosquitto_pub -h localhost -t 'zigbee2mqtt/ACW02_Living_Room/get' -m '{"running_state_ep1":""}'
  sleep 30
done
```

Run with: `nohup ./poll_acw02.sh &`

### Option 5: Fix the Converter (Try This)
The issue might be that Z2M doesn't actually call `onEvent` with `'message'` for every message. Let me try using Z2M's `ota` update mechanism to trigger polls instead.

## Debugging Steps

### 1. Check if onEvent is Even Being Called
Add logging to the converter:

```javascript
onEvent: async (type, data, device, options) => {
    // Add this at the start
    console.log(`ACW02 onEvent: type=${type}, device=${device.ieeeAddr}`);
    
    // Rest of code...
}
```

Restart Z2M and watch logs. If you NEVER see this log line, `onEvent` isn't being called at all!

### 2. Check Z2M Logs for Timer Evidence
Look for any errors like:
- `Uncaught exception in onEvent`
- `setInterval is not defined`
- `globalThis is not defined`

### 3. Test with Simpler Timer
Try replacing the entire `onEvent` with just this:

```javascript
onEvent: async (type, data, device) => {
    console.log(`[ACW02] Event: ${type}`);
},
```

If you don't see logs, Z2M isn't calling `onEvent` at all.

## Why Manual Reads Work

When you use the dev console to read `hvacThermostat/runningMode`:
1. Z2M sends a Zigbee **Read Attributes Request**
2. ESP32 responds with **Read Attributes Response** containing current value
3. `fzLocal.thermostat_ep1` converter processes the response
4. MQTT message published with updated `running_state_ep1`

This proves:
✅ Firmware is working correctly
✅ Converter logic is correct  
✅ Z2M can read the attribute
❌ Automatic polling mechanism is broken

## Recommended Immediate Fix

Use **Home Assistant** automation to poll every 30 seconds:

```yaml
automation:
  - alias: 'ACW02 - Poll Running State'
    description: 'Periodically poll unreportable runningMode attribute'
    trigger:
      - platform: time_pattern
        seconds: '/30'
    action:
      - service: mqtt.publish
        data:
          topic: 'zigbee2mqtt/{{ trigger.platform }}/get'
          payload: |
            {
              "running_state_ep1": "",
              "error_text": ""
            }
    variables:
      # Replace with your actual device friendly_name from Z2M
      device_name: 'ACW02_Living_Room'
```

Or simpler version:

```yaml
automation:
  - alias: 'ACW02 Poll'
    trigger:
      - platform: time_pattern
        seconds: '/30'
    action:
      - service: mqtt.publish
        data:
          topic: 'zigbee2mqtt/ACW02_Living_Room/get'
          payload: '{"running_state_ep1":""}'
```

This will make Z2M read the attribute every 30 seconds, and your `fzLocal.thermostat_ep1` converter will process it!

## Testing the HA Automation

1. Add automation to Home Assistant
2. Restart Home Assistant
3. Enable Z2M debug logging: `log_level: debug`
4. Watch Z2M logs - you should see reads every 30 seconds:
   ```
   [17:00:00] debug: zh:controller: Read 'hvacThermostat'['runningMode'] from 'ACW02_Living_Room'
   [17:00:30] debug: zh:controller: Read 'hvacThermostat'['runningMode'] from 'ACW02_Living_Room'
   ```
5. Change AC mode - within 30 seconds, `running_state_ep1` should update

This is the **most reliable** solution since it bypasses the broken timer mechanism entirely!
