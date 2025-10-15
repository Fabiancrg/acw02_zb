# Polling Workaround for External Converters

## Problem

**`meta.poll` does NOT work in external converters.** This is a Z2M limitation - the polling API is only available to internal converters in the Koenkk/zigbee-herdsman-converters repository.

From my GitHub research:
- ✅ **Internal converters**: Use `modernExtend.poll()` (imported as `m.poll()`)
- ❌ **External converters**: Cannot access `modernExtend` module
- ❌ **`meta.poll`**: Doesn't actually trigger in external converters

## Evidence from Logs

Looking at `join.log`:
- **Line 36**: Initial read during configure works: `data '{"locationDesc":"No Error"}'`
- **Line 32**: Initial poll in configure: `data '{"runningMode":0,"systemMode":0}'`
- **No subsequent automatic polls** - meta.poll never fires

## Solutions

### Option 1: Submit to Official Z2M Repository (Recommended)

Convert your external converter to an internal one and submit a PR to Koenkk's repo.

**Steps:**
1. Fork https://github.com/Koenkk/zigbee-herdsman-converters
2. Add your device to `src/devices/espressif.ts`
3. Use `m.poll()` for automatic polling:

```typescript
const m = require('../lib/modernExtend');

const definition: DefinitionWithExtend = {
    zigbeeModel: ['acw02-z'],
    model: 'ACW02-ZB',
    vendor: 'ESPRESSIF',
    description: 'ACW02 HVAC Thermostat Controller',
    
    extend: [
        m.poll({
            key: 'thermostat',
            option: exposes.options.measurement_poll_interval(),
            defaultIntervalSeconds: 30,
            poll: async (device) => {
                const ep = device.getEndpoint(1);
                await ep.read('hvacThermostat', ['runningMode', 'systemMode']);
                await ep.read('genBasic', ['locationDesc']);
            },
        }),
    ],
    
    fromZigbee: [
        // ... your converters
    ],
    
    // ... rest of definition
};
```

4. Submit PR
5. Wait for merge (usually 1-2 weeks)
6. Update Z2M to latest version
7. Remove external converter

**Pros:**
- ✅ Official Z2M support
- ✅ Automatic updates
- ✅ Reliable polling
- ✅ OTA updates possible

**Cons:**
- ⏱️ Takes time (PR review process)
- 🔧 Need to learn TypeScript basics

### Option 2: Home Assistant Automation (Quick Fix)

Use HA to send read commands every 30 seconds via MQTT.

**Create automation in Home Assistant:**

```yaml
automation:
  - alias: "Poll ACW02 HVAC Unreportable Attributes"
    trigger:
      - platform: time_pattern
        seconds: "/30"  # Every 30 seconds
    action:
      - service: mqtt.publish
        data:
          topic: "zigbee2mqtt/0x1051dbfffe1c7958/get"
          payload: >
            {
              "running_state": "",
              "system_mode": "",
              "error_text": "",
              "fan_mode": ""
            }
```

**Alternative with service call:**
```yaml
automation:
  - alias: "Poll ACW02 Running State"
    trigger:
      - platform: time_pattern
        seconds: "/30"
    action:
      - service: script.poll_hvac_state

script:
  poll_hvac_state:
    sequence:
      - service: mqtt.publish
        data:
          topic: "zigbee2mqtt/Office_HVAC/get"
          payload: '{"running_state":""}'
```

**Pros:**
- ✅ Works immediately
- ✅ No code changes needed
- ✅ Easy to adjust interval

**Cons:**
- ❌ Requires Home Assistant
- ❌ Extra automation to maintain
- ❌ Doesn't work if HA is down

### Option 3: Node-RED Flow

Use Node-RED to poll attributes.

**Flow:**
```
[Inject Node: Every 30s] → [MQTT Out]
```

**Inject Node Config:**
- Repeat: interval
- Every: 30 seconds

**MQTT Out Config:**
- Topic: `zigbee2mqtt/0x1051dbfffe1c7958/get`
- Payload: `{"running_state":"","system_mode":"","error_text":"","fan_mode":""}`

**Pros:**
- ✅ Works immediately
- ✅ Visual programming
- ✅ Can add complex logic

**Cons:**
- ❌ Requires Node-RED
- ❌ Another service to run

### Option 4: Standalone MQTT Client Script

Create a simple Python script that runs as a service.

**`poll_hvac.py`:**
```python
#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import time
import json

MQTT_BROKER = "localhost"
MQTT_PORT = 1883
DEVICE_TOPIC = "zigbee2mqtt/0x1051dbfffe1c7958/get"
POLL_INTERVAL = 30  # seconds

def poll_device(client):
    payload = {
        "running_state": "",
        "system_mode": "",
        "error_text": "",
        "fan_mode": ""
    }
    client.publish(DEVICE_TOPIC, json.dumps(payload))
    print(f"Polled device at {time.strftime('%H:%M:%S')}")

def main():
    client = mqtt.Client()
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_start()
    
    try:
        while True:
            poll_device(client)
            time.sleep(POLL_INTERVAL)
    except KeyboardInterrupt:
        print("Stopping polling...")
    finally:
        client.loop_stop()
        client.disconnect()

if __name__ == "__main__":
    main()
```

**Run as systemd service:**
```bash
sudo nano /etc/systemd/system/hvac-poll.service
```

```ini
[Unit]
Description=HVAC Zigbee Polling Service
After=network.target mosquitto.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi
ExecStart=/usr/bin/python3 /home/pi/poll_hvac.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable hvac-poll.service
sudo systemctl start hvac-poll.service
sudo systemctl status hvac-poll.service
```

**Pros:**
- ✅ Independent service
- ✅ Lightweight
- ✅ Easy to customize

**Cons:**
- ❌ Another service to maintain
- ❌ Need to install paho-mqtt

### Option 5: Modify Z2M Source Code (Advanced)

Patch your local Z2M installation to support polling in external converters.

**Not recommended** because:
- Updates will overwrite your changes
- Hard to maintain
- No support from Z2M team

## Recommended Approach

**Short term:** Use Home Assistant automation (Option 2) or Node-RED (Option 3)

**Long term:** Submit to official Z2M repo (Option 1)

## Testing Any Solution

After implementing polling:

1. **Watch Z2M logs:**
```bash
journalctl -u zigbee2mqtt -f | grep -E "(running|poll|read)"
```

2. **Expect to see reads every 30 seconds:**
```
ZCL command 0x.../1 hvacThermostat.read(['runningMode', 'systemMode'])
Received Zigbee message, type 'readResponse', cluster 'hvacThermostat', data '{"runningMode":4,"systemMode":4}'
```

3. **Verify MQTT updates:**
```bash
mosquitto_sub -t "zigbee2mqtt/0x1051dbfffe1c7958" -v
```

4. **Test AC mode change:**
   - Turn AC to HEAT mode via remote
   - Within 30 seconds: `running_state` should update to "heat"

## Why meta.poll Doesn't Work

From the Z2M source code (`zigbee-herdsman-converters/src/lib/types.ts`):

```typescript
export interface DefinitionMeta {
    multiEndpoint?: boolean;
    poll?: {
        interval: (device: Device) => number;
        read: (device: Device) => Promise<void>;
    };
}
```

The `meta.poll` is read by Z2M during device initialization, but **the polling engine only processes it for internal converters**. External converters are loaded differently and bypass the polling system.

This is why:
- Internal converters in the Koenkk repo can use `m.poll()` ✅
- External converters trying to use `meta.poll` fail silently ❌

## Summary

**Current state:**
- ❌ meta.poll doesn't work in external converters
- ✅ Manual reads work perfectly (confirmed in logs)
- ✅ Firmware is working correctly
- ✅ Converter processes responses correctly

**Solution:**
Pick one of the workarounds above, or better yet, contribute your device to the official Z2M repository!

For now, I recommend **Option 2 (Home Assistant automation)** as it's the quickest to set up and test.
