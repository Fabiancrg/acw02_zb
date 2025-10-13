# HVAC Zigbee Attribute Quick Reference

## Endpoint Configuration
- **Endpoint ID**: 1
- **Profile**: Home Automation (0x0104)
- **Device Type**: Thermostat (0x0301)

## Thermostat Cluster (0x0201)

### Standard Attributes

| Attribute ID | Name | Type | Access | Description |
|--------------|------|------|--------|-------------|
| 0x0000 | Local Temperature | int16 | Read | Current room temperature (°C × 100) |
| 0x0011 | Occupied Cooling Setpoint | int16 | Read/Write | Target cooling temperature (°C × 100) |
| 0x0012 | Occupied Heating Setpoint | int16 | Read/Write | Target heating temperature (°C × 100) |
| 0x001B | Control Sequence | enum8 | Read | 0x04 = Cooling and Heating |
| 0x001C | System Mode | enum8 | Read/Write | See System Modes below |

### System Modes (Attribute 0x001C)

| Value | Mode | HVAC Operation |
|-------|------|----------------|
| 0x00 | Off | Power off |
| 0x01 | Auto | Automatic mode |
| 0x03 | Cool | Cooling mode |
| 0x04 | Heat | Heating mode |
| 0x07 | Fan Only | Fan without heating/cooling |
| 0x08 | Dry | Dehumidification mode |

### Custom Attributes (Manufacturer-Specific)

| Attribute ID | Name | Type | Access | Description |
|--------------|------|------|--------|-------------|
| 0xF000 | Eco Mode | boolean | Read/Write | Energy-saving mode (Cool only) |
| 0xF001 | Swing Mode | boolean | Read/Write | Vertical air flow swing |
| 0xF002 | Display On | boolean | Read/Write | HVAC display state |
| 0xF003 | Error Text | char_str | Read | Error message from HVAC |

## Fan Control Cluster (0x0202)

| Attribute ID | Name | Type | Access | Description |
|--------------|------|------|--------|-------------|
| 0x0000 | Fan Mode | enum8 | Read/Write | See Fan Modes below |
| 0x0001 | Fan Mode Sequence | enum8 | Read | 0x02 = Low/Med/High |

### Fan Modes (Attribute 0x0000)

| Value | Zigbee Mode | HVAC Fan Speed |
|-------|-------------|----------------|
| 0 | Off/Auto | Auto |
| 1 | Low | 20% |
| 2 | Medium | 40% |
| 3 | High | 60% |
| 4 | On | Auto |
| 5 | Smart | 100% |

## Temperature Format

All temperatures are reported in **centidegrees Celsius**:
- 2400 = 24.00°C
- 2350 = 23.50°C
- 2000 = 20.00°C

**Valid Range**: 1600 to 3100 (16°C to 31°C)

## Usage Examples

### Set Cooling to 24°C
```json
{
  "cluster": "0x0201",
  "attribute": "0x001C",
  "value": 3
}
{
  "cluster": "0x0201",
  "attribute": "0x0011",
  "value": 2400
}
```

### Enable Eco Mode
```json
{
  "cluster": "0x0201",
  "attribute": "0xF000",
  "value": true
}
```

### Set Fan to High
```json
{
  "cluster": "0x0202",
  "attribute": "0x0000",
  "value": 3
}
```

### Turn On Swing
```json
{
  "cluster": "0x0201",
  "attribute": "0xF001",
  "value": true
}
```

### Power Off
```json
{
  "cluster": "0x0201",
  "attribute": "0x001C",
  "value": 0
}
```

## Zigbee2MQTT Integration

If using Zigbee2MQTT, the device should auto-configure with the following controls:

- **System Mode**: Dropdown (Off, Auto, Cool, Heat, Fan, Dry)
- **Temperature**: Number slider (16-31°C)
- **Current Temperature**: Sensor (read-only)
- **Fan Mode**: Dropdown (Auto, Low, Medium, High, Max)
- **Eco Mode**: Switch
- **Swing**: Switch  
- **Display**: Switch
- **Error**: Text sensor

## Home Assistant Integration

Example YAML entities after pairing:

```yaml
climate.hvac_thermostat:
  hvac_mode: cool
  temperature: 24
  current_temperature: 25
  fan_mode: auto

switch.hvac_eco_mode:
  state: off

switch.hvac_swing:
  state: on

switch.hvac_display:
  state: on

sensor.hvac_error:
  state: "No Error"
```

## Notes

1. **Eco Mode**: Only functional when System Mode is Cool (0x03)
2. **Temperature Updates**: Ambient temperature updates every 30 seconds
3. **Error Messages**: Check Error Text attribute (0xF003) for HVAC issues
4. **Fan in Eco**: Fan is forced to Auto when Eco Mode is enabled
5. **Persistence**: Settings are maintained across power cycles via NVS

## Troubleshooting

### Attribute Read Fails
- Check that endpoint 1 is being addressed
- Verify cluster ID is correct (0x0201 for thermostat)
- Ensure device is joined to network

### Writes Don't Take Effect
- Check UART connection to HVAC unit
- Monitor logs for TX/RX frames
- Verify HVAC unit is powered on
- Check CRC errors in logs

### Temperature Not Updating
- HVAC may not report temperature in all modes
- Check that status requests are sent every 30s
- Verify UART RX is working (check logs for RX frames)

## Contact & Support

For issues, check:
1. Serial monitor logs (`idf.py monitor`)
2. Zigbee coordinator logs
3. HVAC UART traffic (should see 0x7A 0x7A frames)
4. GitHub repository for updates
