# Zigbee HVAC Thermostat Controller

This project implements a Zigbee End Device that controls an HVAC unit via UART and exposes it as a standard Zigbee thermostat.

## Hardware Requirements

- ESP32-C6 development board
- ACW02 HVAC unit (or compatible)
- UART connection (TX/RX pins 16/17)

## Features

The Zigbee thermostat exposes the following capabilities:

### Standard Thermostat Cluster (0x0201)
- **System Mode**: Off, Auto, Cool, Heat, Dry, Fan
- **Temperature Control**: 16-31°C (configurable setpoint)
- **Local Temperature**: Current room temperature from HVAC unit
- **Occupied Cooling/Heating Setpoint**: Target temperature

### Fan Control Cluster (0x0202)
- **Fan Mode**: Auto, Low, Medium, High, Smart/Max

### Custom Attributes (Manufacturer-Specific)
- **Eco Mode** (0xF000): Energy-saving mode (only works in Cool mode)
- **Swing Mode** (0xF001): Vertical air flow swing
- **Display** (0xF002): HVAC unit display on/off
- **Error Text** (0xF003): Error messages from HVAC unit

## UART Protocol

The device communicates with the HVAC unit using a proprietary protocol at 9600 baud:

- **TX Pin**: GPIO 16
- **RX Pin**: GPIO 17
- **Baud Rate**: 9600
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1

### Frame Format

All frames start with `0x7A 0x7A` and end with a 16-bit CRC.

**Command Frame (28 bytes)**:
```
[0x7A][0x7A][Header][Mode][Temp][Fan][Swing][Options][...][CRC_H][CRC_L]
```

**Status Request (13 bytes)**:
```
0x7A 0x7A 0x21 0xD5 0x0C 0x00 0x00 0xA2 0x0A 0x0A 0xFE 0x29
```

**Keepalive (13 bytes)**:
```
0x7A 0x7A 0x21 0xD5 0x0C 0x00 0x00 0xAB 0x0A 0x0A 0xFC 0xF9
```

## Project Structure

```
acw02_zb/
├── main/
│   ├── esp_zb_hvac.c          # Main Zigbee application
│   ├── esp_zb_light.h         # Zigbee configuration header
│   ├── hvac_driver.c          # HVAC UART driver implementation
│   ├── hvac_driver.h          # HVAC driver header
│   ├── CMakeLists.txt         # Component build configuration
│   └── idf_component.yml      # Component dependencies
├── CMakeLists.txt             # Project CMakeLists
├── sdkconfig                  # ESP-IDF configuration
└── README.md                  # This file
```

## Building and Flashing

1. Set up ESP-IDF v5.5.1 or later
2. Configure the project:
   ```bash
   idf.py set-target esp32c6
   idf.py menuconfig
   ```
3. Build the project:
   ```bash
   idf.py build
   ```
4. Flash to device:
   ```bash
   idf.py -p COMx flash monitor
   ```

## Configuration

### Zigbee Configuration

The device is configured as a Zigbee End Device (ZED):
- **Endpoint**: 1
- **Profile**: Home Automation (0x0104)
- **Device ID**: Thermostat (0x0301)
- **Channel Mask**: All channels

### HVAC Settings

Temperature range: 16-31°C (mapped to 61-88°F internally)

### Joining the Network

On first boot, the device will automatically enter network steering mode. The LED will blink while searching for a network. Once joined, the device will save the network credentials and automatically rejoin on subsequent boots.

## Usage

### From Zigbee Coordinator (e.g., Zigbee2MQTT, Home Assistant)

1. Put your coordinator in pairing mode
2. Power on the ESP32-C6 device
3. Wait for the device to join (check logs)
4. The thermostat will appear with the following entities:
   - System Mode (Off/Auto/Cool/Heat/Dry/Fan)
   - Target Temperature (16-31°C)
   - Current Temperature
   - Fan Speed
   - Eco Mode switch
   - Swing Mode switch
   - Display switch
   - Error status

### Control Examples

**Set cooling mode at 24°C:**
- Set system_mode = "cool"
- Set occupied_cooling_setpoint = 2400 (24.00°C in centidegrees)

**Enable eco mode:**
- Set eco_mode attribute (0xF000) = true
- Note: Only works when system_mode is "cool"

**Adjust fan speed:**
- Set fan_mode in Fan Control cluster

## Removed Features

This project was adapted from the `ZigbeeMultiSensor` example. The following endpoints and features were removed:

- ❌ BME280 environmental sensor endpoint
- ❌ LED strip control endpoint
- ❌ GPIO LED control endpoint
- ❌ Button sensor endpoint
- ❌ Rain gauge sensor endpoint

All sensor and LED control logic has been replaced with HVAC thermostat functionality.

## HVAC Driver Details

### State Management

The driver maintains a local copy of the HVAC state and synchronizes it with:
1. Commands sent to the HVAC unit via UART
2. Status responses received from the HVAC unit
3. Zigbee attribute updates

### Periodic Updates

- **Status Request**: Every 30 seconds
- **Keepalive**: Every 60 seconds
- **Zigbee Sync**: After every state change + periodic updates

### Error Handling

- CRC validation on all received frames
- Retry logic for failed commands (future enhancement)
- Error text attribute reports HVAC errors to coordinator

## Troubleshooting

### Device won't join network
- Check that Zigbee coordinator is in pairing mode
- Verify ESP32-C6 is powered correctly
- Check serial logs for error messages
- Try factory reset (hold button during boot - if button is implemented)

### HVAC not responding
- Verify UART wiring (TX ↔ RX crossover)
- Check baud rate is 9600
- Monitor UART traffic in logs
- Verify HVAC unit is powered on

### Temperature not updating
- HVAC may not report ambient temperature in all modes
- Check that status requests are being sent every 30s
- Verify frame CRC is correct

## License

This code is based on Espressif's Zigbee examples and inherits their licensing.

## Credits

- Base Zigbee framework: Espressif ESP-IDF examples
- HVAC protocol: Reverse-engineered from ACW02 ESPHome component
- Original ESPHome component: See `CodeToReusePartially/acw02_esphome/`

## Future Enhancements

- [ ] Add button for factory reset
- [ ] Implement command retry logic
- [ ] Add OTA firmware update support
- [ ] Support horizontal swing control
- [ ] Add night mode support
- [ ] Implement filter status monitoring
- [ ] Add clean mode support
