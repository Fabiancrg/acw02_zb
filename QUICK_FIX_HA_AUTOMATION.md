# Quick Fix: Home Assistant Polling Automation

Since Z2M's `onEvent` timer mechanism doesn't work reliably, use Home Assistant to trigger polling instead.

## Solution: HA Automation

Add this to your Home Assistant `configuration.yaml` or `automations.yaml`:

```yaml
automation:
  - id: 'acw02_poll_running_state'
    alias: 'ACW02 - Poll Running Mode Every 30 Seconds'
    description: 'Automatically poll unreportable runningMode attribute from ACW02'
    mode: single
    trigger:
      - platform: time_pattern
        seconds: '/30'  # Every 30 seconds
    action:
      - service: mqtt.publish
        data:
          topic: 'zigbee2mqtt/ACW02_Living_Room/get'  # Replace with your device friendly_name
          payload: |
            {
              "running_state_ep1": "",
              "error_text": ""
            }
```

## Customization

### Change Device Name
Replace `ACW02_Living_Room` with your actual Z2M friendly_name:
```yaml
topic: 'zigbee2mqtt/YOUR_DEVICE_NAME_HERE/get'
```

### Change Poll Interval
Poll every 15 seconds instead of 30:
```yaml
seconds: '/15'
```

Poll every minute:
```yaml
seconds: '/60'
```

### Poll Multiple Devices
```yaml
automation:
  - id: 'acw02_poll_all'
    alias: 'ACW02 - Poll All Units'
    trigger:
      - platform: time_pattern
        seconds: '/30'
    action:
      - service: mqtt.publish
        data:
          topic: 'zigbee2mqtt/ACW02_Living_Room/get'
          payload: '{"running_state_ep1":""}'
      - service: mqtt.publish
        data:
          topic: 'zigbee2mqtt/ACW02_Bedroom/get'
          payload: '{"running_state_ep1":""}'
```

## What This Does

1. **Every 30 seconds**, Home Assistant sends an MQTT message to Z2M
2. Z2M receives the `/get` request for `running_state_ep1`
3. Z2M **reads** the `runningMode` attribute from your ESP32 device
4. ESP32 responds with current value (idle/heat/cool/fan_only)
5. Z2M's converter processes the response
6. Z2M publishes updated MQTT state
7. Home Assistant sees the update

## Testing

1. Add automation to HA
2. Restart HA or reload automations
3. Set AC to HEAT mode
4. Watch Home Assistant - within 30 seconds, `running_state_ep1` should show "heat"
5. Set AC to OFF
6. Within 30 seconds, `running_state_ep1` should show "idle"

## Z2M Logs

If you enable debug logging in Z2M (`log_level: debug`), you'll see:

```
[17:00:00] info:  z2m:mqtt: MQTT publish: topic 'zigbee2mqtt/ACW02_Living_Room/get', payload '{"running_state_ep1":""}'
[17:00:00] debug: zh:controller: Read 'hvacThermostat'['runningMode'] from 'ACW02_Living_Room'
[17:00:01] debug: zh:controller: Received 'readResponse' from 'ACW02_Living_Room' with data '{"runningMode":4}'
[17:00:01] info:  z2m:mqtt: MQTT publish: topic 'zigbee2mqtt/ACW02_Living_Room', payload '{"running_state_ep1":"heat",...}'
```

## Why This Works

- Manual dev console reads work perfectly ✅
- This automation IS a manual read triggered automatically
- Bypasses Z2M's broken `onEvent` timer mechanism  
- Reliable and proven approach
- No firmware changes needed
- No converter changes needed

## Final Result

Your `running_state_ep1` will update automatically every 30 seconds! 🎉
