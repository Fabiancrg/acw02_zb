# Zigbee2MQTT Integration Fix

## Problem
The device joins successfully but Z2M shows:
```
Device '0x7c2c67fffe42d2d4' with Zigbee model 'esp32c6' and manufacturer name 'ESPRESSIF' is NOT supported
```

This means Z2M doesn't have a device definition and won't expose the thermostat controls.

## Solution

### Step 1: Rebuild with Fixed Basic Cluster Attributes

The code has been updated to include all Basic cluster attributes that Z2M expects:
- `appVersion` (0x0001)
- `stackVersion` (0x0002)
- `hwVersion` (0x0003)
- `dateCode` (0x0006)
- `swBuildId` (0x4000)

**Rebuild and flash:**
```powershell
cd C:\Devel\Repositories\acw02_zb
idf.py build
idf.py flash monitor
```

### Step 2: Add Device Definition to Zigbee2MQTT

Since this is a custom device, Z2M needs to be told how to handle it.

#### Option A: External Converter (Recommended)

1. Copy the file `zigbee2mqtt_converter.js` to your Z2M data directory:
   ```bash
   # On Linux/Docker Z2M
   cp zigbee2mqtt_converter.js /opt/zigbee2mqtt/data/
   ```

2. Edit your Z2M `configuration.yaml`:
   ```yaml
   external_converters:
     - zigbee2mqtt_converter.js
   ```

3. Restart Zigbee2MQTT:
   ```bash
   sudo systemctl restart zigbee2mqtt
   # or for Docker:
   docker restart zigbee2mqtt
   ```

4. In Z2M UI:
   - Go to your device (0x7c2c67fffe42d2d4)
   - Click the "..." menu
   - Select "Reconfigure"
   - Wait for reconfiguration to complete

#### Option B: Manual Device Support

If external converter doesn't work, you can manually control the device using the Z2M UI:

1. Go to the device page in Z2M
2. Click on "Exposes" tab
3. You should see the clusters (Basic, Thermostat, Fan Control)
4. Click "Dev console" tab
5. You can manually read/write attributes:

**Read local temperature:**
```json
{
  "cluster": "hvacThermostat",
  "type": "read",
  "attributes": ["localTemp"]
}
```

**Set system mode (Off=0, Auto=1, Cool=3, Heat=4, Dry=8, Fan=7):**
```json
{
  "cluster": "hvacThermostat", 
  "type": "write",
  "attributes": {"systemMode": 3}
}
```

**Set cooling setpoint (in centidegrees, e.g., 2400 = 24°C):**
```json
{
  "cluster": "hvacThermostat",
  "type": "write", 
  "attributes": {"occupiedCoolingSetpoint": 2400}
}
```

**Set fan mode (Off=0, Low=1, Medium=2, High=3, On=4, Auto=5):**
```json
{
  "cluster": "hvacFanCtrl",
  "type": "write",
  "attributes": {"fanMode": 5}
}
```

### Step 3: Verify

After implementing either option:

1. **Check logs** - Device should no longer show "NOT supported" warning
2. **Check UI** - You should see climate controls with:
   - Temperature display
   - System mode selector (Off/Auto/Cool/Heat/Dry/Fan)
   - Heating/cooling setpoint sliders (16-31°C)
   - Fan mode selector
3. **Test control** - Change mode/temperature and check ESP32 logs for UART commands

## Expected Outcome

- ✅ Device recognized as "ACW02-HVAC-ZB"
- ✅ Climate entity with temperature and mode controls
- ✅ Fan control entity
- ✅ All attributes readable/writable via Z2M

## Troubleshooting

**Still shows "NOT supported":**
- Make sure `zigbee2mqtt_converter.js` is in the correct directory
- Check Z2M logs for converter loading errors
- Verify `external_converters` is correctly configured in `configuration.yaml`

**No temperature/controls visible:**
- Try removing and re-pairing the device
- Check ESP32 logs to verify clusters are created
- Use "Dev console" to manually read cluster attributes

**Commands not working:**
- Check ESP32 monitor for attribute write callbacks
- Verify HVAC driver is initialized
- Check UART TX/RX wiring (GPIO 16/17)

## Related Files

- `esp_zb_hvac.c` - Main Zigbee application (updated with Basic cluster attributes)
- `zigbee2mqtt_converter.js` - Z2M external converter definition
- `join.log` - Current Z2M interview log showing the issue
