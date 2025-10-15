# Polling Debug Guide

## Added Debug Logging

The converter now has comprehensive logging to diagnose polling issues:

### 1. onEvent Logging

**All events are logged:**
```javascript
logger.info(`ACW02 onEvent: type='${type}', device='${device.ieeeAddr}'`);
```

**What to look for:**
- `type='interval'` - Z2M's polling framework is working
- `type='start'` - Device joined/started
- `type='stop'` - Device removed
- `type='message'` - Device sent a message (attribute report, etc.)

### 2. Polling Trigger Logging

**When 'interval' event fires:**
```
info: ACW02 POLLING TRIGGERED for 0x1051dbfffe1c7958
```

**If you DON'T see this message, the polling framework is not running!**

### 3. Attribute Read Logging

**For each cluster read:**
- `debug: ACW02 polling: Reading hvacThermostat attributes...`
- `info: ACW02 polling: hvacThermostat read successful: {"runningMode":4,"systemMode":4}`
- OR `error: ACW02 polling: hvacThermostat read failed: timeout`

**For error text:**
- `debug: ACW02 polling: Reading genBasic locationDesc...`
- `info: ACW02 polling: genBasic read successful: {"locationDesc":[8,78,111,32,69,114,114,111,114]}`

**For fan mode:**
- `debug: ACW02 polling: Reading hvacFanCtrl fanMode...`
- `info: ACW02 polling: hvacFanCtrl read successful: {"fanMode":3}`

### 4. Converter Processing Logging

**When data is processed by fromZigbee converters:**
- `info: ACW02 fz.thermostat: runningMode=0x04 -> heat`
- `info: ACW02 fz.thermostat: systemMode=0x04 -> heat`
- `info: ACW02 fz.fan_mode: Received fanMode=0x03 -> medium`
- `info: ACW02 fz.error_text: Decoded error text: "No Error"`
- `debug: ACW02 fz.error_text: Raw locationDesc data type: object, value: [8,78,111,32,69,114,114,111,114]`

## How to Enable Debug Logging

### Method 1: Z2M Configuration File

Edit your `configuration.yaml`:

```yaml
advanced:
  log_level: debug  # Or 'info' to see most messages
```

Then restart Z2M:
```bash
sudo systemctl restart zigbee2mqtt
```

### Method 2: MQTT Command (Temporary)

```bash
mosquitto_pub -t "zigbee2mqtt/bridge/request/options" -m '{"options":{"advanced":{"log_level":"debug"}}}'
```

This lasts until Z2M restart.

### Method 3: Docker Logs

If running in Docker:
```bash
docker logs -f zigbee2mqtt 2>&1 | grep -E "(ACW02|interval|poll)"
```

## What to Look For

### ✅ Polling IS Working

You should see this sequence **every 60 seconds** (or your configured interval):

```
info: z2m: ACW02 onEvent: type='interval', device='0x1051dbfffe1c7958'
info: z2m: ACW02 POLLING TRIGGERED for 0x1051dbfffe1c7958
debug: z2m: ACW02 polling: Reading hvacThermostat attributes...
info: z2m: ACW02 polling: hvacThermostat read successful: {"runningMode":4,"systemMode":4,"occupiedHeatingSetpoint":2200,"occupiedCoolingSetpoint":2200}
info: z2m: ACW02 fz.thermostat: runningMode=0x04 -> heat
info: z2m: ACW02 fz.thermostat: systemMode=0x04 -> heat
debug: z2m: ACW02 polling: Reading genBasic locationDesc...
info: z2m: ACW02 polling: genBasic read successful: {"locationDesc":[8,78,111,32,69,114,114,111,114]}
info: z2m: ACW02 fz.error_text: Decoded error text: "No Error"
debug: z2m: ACW02 polling: Reading hvacFanCtrl fanMode...
info: z2m: ACW02 polling: hvacFanCtrl read successful: {"fanMode":3}
info: z2m: ACW02 fz.fan_mode: Received fanMode=0x03 -> medium
info: z2m: ACW02 POLLING COMPLETED for 0x1051dbfffe1c7958
```

### ❌ Polling NOT Working

**Problem 1: No 'interval' events at all**

If you see:
```
info: z2m: ACW02 onEvent: type='start', device='0x1051dbfffe1c7958'
```

But NEVER see:
```
info: z2m: ACW02 onEvent: type='interval', device='0x1051dbfffe1c7958'
```

**Cause:** Z2M's polling framework is not enabled for this device.

**Solutions:**
1. Check device options in Z2M UI - is "Measurement poll interval" visible?
2. If not visible, `exposes.options.measurement_poll_interval()` may not be recognized
3. Try reconfiguring the device (Z2M UI → Device → "Reconfigure")
4. Try removing and re-adding the external converter
5. Check Z2M version - polling framework added in v1.28.0+

**Problem 2: 'interval' events fire, but reads fail**

If you see:
```
info: z2m: ACW02 POLLING TRIGGERED for 0x1051dbfffe1c7958
error: z2m: ACW02 polling: hvacThermostat read failed: Timeout
```

**Cause:** Device is not responding to read commands.

**Solutions:**
1. Check device is powered on and online
2. Check signal strength (RSSI) in Z2M UI
3. Try manual read in dev console to confirm device responds
4. Check ESP32 logs for Zigbee stack errors
5. Verify clusters are bound in configure() function

**Problem 3: Reads succeed but converters don't fire**

If you see:
```
info: z2m: ACW02 polling: hvacThermostat read successful: {"runningMode":4}
```

But NOT:
```
info: z2m: ACW02 fz.thermostat: runningMode=0x04 -> heat
```

**Cause:** fromZigbee converter is not matching the message.

**Solutions:**
1. Check `type:` array in converter includes `'readResponse'`
2. Check cluster name matches: `cluster: 'hvacThermostat'`
3. Check `msg.data` has expected property name
4. Look for errors in Z2M logs about converter failures

## Testing Scenarios

### Test 1: Verify Polling Starts

1. Restart Z2M with updated converter
2. Wait for device to join/start
3. Look for: `ACW02 device started/paired: 0x...`
4. Wait 60 seconds
5. Look for: `ACW02 POLLING TRIGGERED`

**Expected:** Polling starts automatically after device joins.

### Test 2: Verify Polling Interval

1. Set custom interval:
   ```bash
   mosquitto_pub -t "zigbee2mqtt/Office_HVAC/set" -m '{"measurement_poll_interval":30}'
   ```
2. Watch logs
3. Count seconds between `POLLING TRIGGERED` messages

**Expected:** Polling every 30 seconds (or configured interval).

### Test 3: Verify Attribute Updates

1. Turn AC to HEAT mode via remote
2. Wait for next poll (watch for `POLLING TRIGGERED`)
3. Look for: `runningMode=0x04 -> heat`
4. Check MQTT: `mosquitto_sub -t "zigbee2mqtt/Office_HVAC"`

**Expected:** `"running_state":"heat"` in MQTT payload.

### Test 4: Verify Error Text

1. Trigger AC error (if possible)
2. Wait for next poll
3. Look for: `Decoded error text: "FAULT 0x04: ..."`
4. Check MQTT

**Expected:** `"error_text":"FAULT 0x04: Low voltage protection"` or similar.

## Troubleshooting Commands

### Check Device Options
```bash
mosquitto_pub -t "zigbee2mqtt/bridge/request/device/options" -m '{"id":"Office_HVAC"}'
```

### Force Immediate Poll (if supported)
```bash
mosquitto_pub -t "zigbee2mqtt/Office_HVAC/get" -m '{"running_state":"","fan_mode":"","error_text":""}'
```

### Check Z2M Version
```bash
mosquitto_sub -t "zigbee2mqtt/bridge/info" -C 1
```

Look for `"version":"1.x.x"` - needs v1.28.0+ for polling framework.

### Enable Debug Logs via MQTT
```bash
mosquitto_pub -t "zigbee2mqtt/bridge/request/options" -m '{"options":{"advanced":{"log_level":"debug"}}}'
```

### Watch Live Logs
```bash
# Systemd
journalctl -u zigbee2mqtt -f | grep ACW02

# Docker
docker logs -f zigbee2mqtt 2>&1 | grep ACW02

# PM2
pm2 logs zigbee2mqtt --lines 100 | grep ACW02
```

## Common Issues and Fixes

### Issue: "logger is not defined" in onEvent

**Symptom:** Z2M crashes or logs error about `logger` being undefined.

**Cause:** Old Z2M version or incorrect function signature.

**Fix:** `onEvent` signature is `async (type, data, device, options, logger)` - logger is 5th parameter.

### Issue: No measurement_poll_interval option in UI

**Symptom:** Device settings don't show polling interval option.

**Cause:** Z2M not recognizing `exposes.options.measurement_poll_interval()`.

**Fix 1:** Update Z2M to v1.28.0 or newer.
**Fix 2:** Reload external converters (remove and re-add in config).
**Fix 3:** Reconfigure device in Z2M UI.

### Issue: Polling stops after device restart

**Symptom:** Polling works initially but stops after Z2M or device restart.

**Cause:** Device state not persisted, or onEvent not firing 'start' event.

**Fix:** Check for `type='start'` event in logs. Ensure device rejoins network properly.

### Issue: Reads timeout constantly

**Symptom:** Every poll shows `read failed: Timeout`.

**Cause:** Device offline, poor signal, or cluster not bound.

**Fix:**
1. Check device power and signal strength
2. Verify clusters bound in `configure()` function
3. Try manual read in dev console to isolate issue
4. Check ESP32 logs for Zigbee stack errors

## Expected Log Output (Full Example)

```
info: z2m: ACW02 onEvent: type='start', device='0x1051dbfffe1c7958'
info: z2m: ACW02 device started/paired: 0x1051dbfffe1c7958

[... 60 seconds later ...]

info: z2m: ACW02 onEvent: type='interval', device='0x1051dbfffe1c7958'
info: z2m: ACW02 POLLING TRIGGERED for 0x1051dbfffe1c7958
debug: z2m: ACW02 polling: Reading hvacThermostat attributes...
debug: z2m: Zigbee publish to 'Office_HVAC', endpoint: 1, cluster: 'hvacThermostat', read: ['runningMode','systemMode','occupiedHeatingSetpoint','occupiedCoolingSetpoint']
debug: z2m: Received Zigbee message from 'Office_HVAC', type: 'readResponse', cluster: 'hvacThermostat', data: '{"runningMode":4,"systemMode":4,"occupiedHeatingSetpoint":2200,"occupiedCoolingSetpoint":2200}'
info: z2m: ACW02 polling: hvacThermostat read successful: {"runningMode":4,"systemMode":4,"occupiedHeatingSetpoint":2200,"occupiedCoolingSetpoint":2200}
info: z2m: ACW02 fz.thermostat: runningMode=0x04 -> heat
info: z2m: ACW02 fz.thermostat: systemMode=0x04 -> heat
debug: z2m: ACW02 fz.thermostat: Converted attributes: {"running_state":"heat","system_mode":"heat","occupied_heating_setpoint":22,"occupied_cooling_setpoint":22}
debug: z2m: ACW02 polling: Reading genBasic locationDesc...
debug: z2m: Zigbee publish to 'Office_HVAC', endpoint: 1, cluster: 'genBasic', read: ['locationDesc']
debug: z2m: Received Zigbee message from 'Office_HVAC', type: 'readResponse', cluster: 'genBasic', data: '{"locationDesc":[8,78,111,32,69,114,114,111,114]}'
info: z2m: ACW02 polling: genBasic read successful: {"locationDesc":[8,78,111,32,69,114,114,111,114]}
debug: z2m: ACW02 fz.error_text: Raw locationDesc data type: object, value: [8,78,111,32,69,114,114,111,114]
info: z2m: ACW02 fz.error_text: Decoded error text: "No Error"
debug: z2m: ACW02 polling: Reading hvacFanCtrl fanMode...
debug: z2m: Zigbee publish to 'Office_HVAC', endpoint: 1, cluster: 'hvacFanCtrl', read: ['fanMode']
debug: z2m: Received Zigbee message from 'Office_HVAC', type: 'readResponse', cluster: 'hvacFanCtrl', data: '{"fanMode":3}'
info: z2m: ACW02 polling: hvacFanCtrl read successful: {"fanMode":3}
info: z2m: ACW02 fz.fan_mode: Received fanMode=0x03 -> medium
info: z2m: ACW02 POLLING COMPLETED for 0x1051dbfffe1c7958
debug: z2m: Publishing MQTT message to 'zigbee2mqtt/Office_HVAC' with payload: '{"running_state":"heat","system_mode":"heat","fan_mode":"medium","error_text":"No Error",...}'

[... 60 seconds later, repeat ...]
```

## Summary

The converter now has **verbose logging** that will tell you:

1. ✅ Whether polling framework is triggering ('interval' events)
2. ✅ What attributes are being read
3. ✅ Whether reads succeed or fail (with error messages)
4. ✅ What data is received from the device
5. ✅ How converters process the data
6. ✅ Final MQTT payload being published

**Next Steps:**
1. Update your Z2M external converter with this new version
2. Restart Zigbee2MQTT
3. Set log level to 'debug' or 'info'
4. Watch logs for `ACW02` messages
5. Share the logs if polling still doesn't work!
