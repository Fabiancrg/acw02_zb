# Migration Guide: ZigbeeMultiSensor to HVAC Zigbee Thermostat

## Overview

This document describes the changes made to transform the `ZigbeeMultiSensor` project into an HVAC Zigbee thermostat controller.

## Files Created

### New HVAC Driver Files

1. **`main/hvac_driver.h`** - HVAC driver interface
   - Defines HVAC modes, fan speeds, swing positions
   - Declares HVAC state structure
   - Function prototypes for HVAC control

2. **`main/hvac_driver.c`** - HVAC driver implementation
   - UART communication at 9600 baud on GPIO 16/17
   - Frame building with CRC16 calculation
   - State decoding from HVAC responses
   - Command queue and retry logic

3. **`main/esp_zb_hvac.c`** - Main Zigbee application
   - Replaces `esp_zb_light.c`
   - Single endpoint (Thermostat)
   - Integrates HVAC driver with Zigbee stack
   - Attribute handlers for thermostat cluster

4. **`README_HVAC.md`** - Project documentation
   - Hardware requirements
   - Feature list
   - UART protocol details
   - Building and usage instructions

## Files Modified

### `main/esp_zb_light.h`
- Removed old endpoint definitions (LIGHT1, LIGHT2, BUTTON, BME280, RAIN_GAUGE)
- Added `HA_ESP_HVAC_ENDPOINT` definition
- Updated includes to reference `hvac_driver.h` instead of `light_driver.h`

### `main/idf_component.yml`
- Removed `espressif/bme280` dependency
- Removed `espressif/led_strip` dependency
- Kept Zigbee libraries (esp-zboss-lib, esp-zigbee-lib)

## Files Backed Up (Renamed .old)

The following files were preserved for reference but are no longer used:

- `main/esp_zb_light.c.old` - Original multi-endpoint application
- `main/bme280_app.c.old` - BME280 sensor driver
- `main/bme280_app.h.old` - BME280 sensor header
- `main/light_driver.c.old` - LED strip driver
- `main/light_driver.h.old` - LED driver header

## Endpoint Changes

### Before (5 Endpoints)
1. **Endpoint 1**: On/Off Light (LED Strip)
2. **Endpoint 2**: On/Off Light (GPIO LED)
3. **Endpoint 3**: Simple Sensor (Button with action detection)
4. **Endpoint 4**: Temperature Sensor (BME280 - temp/humidity/pressure)
5. **Endpoint 5**: Simple Sensor (Rain Gauge)

### After (1 Endpoint)
1. **Endpoint 1**: Thermostat (HVAC controller)
   - System Mode control
   - Temperature setpoint
   - Local temperature reading
   - Fan control
   - Custom attributes (eco, swing, display, error)

## Cluster Changes

### Removed Clusters
- ❌ On/Off (0x0006) - for LED control
- ❌ Temperature Measurement (0x0402) - BME280
- ❌ Humidity Measurement (0x0405) - BME280
- ❌ Pressure Measurement (0x0403) - BME280
- ❌ Analog Input (0x000C) - Button and rain gauge
- ❌ Level Control (0x0008) - LED dimming

### Added Clusters
- ✅ Thermostat (0x0201) - HVAC control
  - Standard attributes: system_mode, local_temperature, setpoints
  - Custom attributes: eco_mode (0xF000), swing_mode (0xF001), display_on (0xF002), error_text (0xF003)
- ✅ Fan Control (0x0202) - HVAC fan speed

## Code Reuse from ACW02 ESPHome Component

The HVAC communication protocol was adapted from the ESPHome component at:
`CodeToReusePartially/acw02_esphome/components/acw02/`

### Key Concepts Ported
1. **Frame Structure**
   - Header bytes: `0x7A 0x7A`
   - CRC16 calculation (Modbus style)
   - Valid frame sizes: 13, 18, 28, 34 bytes

2. **Temperature Encoding**
   - Celsius to Fahrenheit conversion
   - Lookup table for Fahrenheit encoding (61-88°F)

3. **Mode Mapping**
   - Off, Auto, Cool, Dry, Fan, Heat modes
   - Fan speeds: Auto, P20, P40, P60, P80, P100, Silent, Turbo
   - Swing positions: Stop, Auto, P1-P5

4. **Command Building**
   - Power state byte
   - Temperature byte encoding
   - Fan speed byte
   - Options byte (eco, display flags)

### Simplifications Made
The ESP-IDF implementation is simpler than the ESPHome version:
- No MQTT support (Zigbee provides network connectivity)
- No preset storage (can be added later)
- No localization (English only)
- No advanced features like clean mode, purifier, night mode (can be added)
- Simplified retry logic (can be enhanced)

## Building the Project

### Prerequisites
- ESP-IDF v5.5.1 or later
- ESP32-C6 target configured

### Build Commands
```bash
cd c:\Devel\Repositories\acw02_zb

# Set target
idf.py set-target esp32c6

# Build
idf.py build

# Flash and monitor
idf.py -p COMx flash monitor
```

## Testing Checklist

- [ ] Device joins Zigbee network successfully
- [ ] Thermostat endpoint appears in coordinator
- [ ] Can set system mode (Off/Cool/Heat/etc)
- [ ] Can adjust temperature setpoint
- [ ] Local temperature updates from HVAC
- [ ] Fan control works
- [ ] Eco mode toggles correctly
- [ ] Swing mode toggles correctly
- [ ] Display control works
- [ ] Error messages appear if HVAC has issues
- [ ] Device reconnects after power cycle
- [ ] UART communication is stable (check logs)

## Known Limitations

1. **Temperature Range**: Limited to 16-31°C (61-88°F)
2. **Eco Mode**: Only available in Cool mode
3. **Ambient Temperature**: May not be available from HVAC in all modes
4. **No Retry Logic**: Commands are sent once (can be enhanced)
5. **No OTA Updates**: Not implemented yet

## Future Enhancements

See README_HVAC.md for planned features:
- Command retry logic
- Factory reset button
- Additional HVAC modes (clean, night)
- Horizontal swing support
- Filter status monitoring
- OTA firmware updates

## Troubleshooting

### Compile Errors
- Ensure ESP-IDF is version 5.5.1 or later
- Run `idf.py fullclean` and rebuild if switching from old code
- Check that all .old files are not being compiled

### UART Issues
- Verify GPIO pins 16 (TX) and 17 (RX) are correct for your board
- Check that UART is wired correctly (TX→RX, RX→TX crossover)
- Monitor UART traffic in logs to verify frame structure

### Zigbee Issues
- Check that coordinator is in pairing mode
- Verify Zigbee channel matches your network
- Try resetting device and rejoining network

## References

- [ESP-IDF Zigbee Examples](https://github.com/espressif/esp-idf/tree/master/examples/zigbee)
- [Zigbee Cluster Library Specification](https://zigbeealliance.org/wp-content/uploads/2019/12/07-5123-06-zigbee-cluster-library-specification.pdf)
- [ACW02 ESPHome Component](../CodeToReusePartially/acw02_esphome/)
