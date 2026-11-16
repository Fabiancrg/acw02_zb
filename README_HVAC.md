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

**Command Frame (24 bytes)**:
```
[0x7A][0x7A][Header][Fan/Power/Mode][Temp][Swing][Options][Mute][...][CRC_H][CRC_L]
```

- **Swing byte**: `(horizontal << 4) | vertical`. Vertical `0x0` = stop, `0x1` = swing, `0x2`–`0x6` = fixed positions.
- **Temperature byte**: `°C − 16` (0x00–0x0F), or a table-encoded value when the AC is set to °F.

The AC answers with a 13-byte ACK, a 34-byte status frame, or a 28-byte warning/fault frame.

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

1. Set up ESP-IDF v5.5.3 or later
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

Temperature range: 16-31°C.

If the AC's display unit is set to Fahrenheit (61-88°F), the driver detects it from the status frame, converts the setpoint to °C for Zigbee, and encodes commands back in °F so the unit chosen on the AC is kept.

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

The AC is the source of truth:

- **At boot** the driver does not send any command. It requests the AC's status and adopts it, so a module reboot (power glitch, OTA) never changes what the AC is doing. Settings are still saved to NVS (at most once every 15 minutes) but are only used until the first status frame arrives; the mute setting, which the AC does not report, is the one that effectively persists.
- **Swing**: the switch maps to vertical swing (`0x1`). The horizontal swing and any fixed louvre position set from the IR remote are preserved when other settings are changed.
- **Clean status** (endpoint 7) is ON when the AC reports the filter-cleaning warning, or while a self-clean cycle is running (only reported in cool or dry mode).
- **Power off** clears eco, night and purifier, because the AC drops them when it switches off. Turning any of them on while the AC is off is refused and the switch returns to OFF.

### Louvres and Self-Clean

| Control | Zigbee | Values |
|---|---|---|
| Vertical swing | EP3 On/Off | on = swing (`0x1`), off = stop |
| Vertical louvre position | EP1 Thermostat, `acLouverPosition` (0x0045) | 1 (top) – 5 (bottom); selecting one stops vertical swing |
| Horizontal swing | EP3 Multistate Value, `presentValue` | 1 stop, 2 full swing, 3–7 positions 1–5, 8 range 1–5, 9 swing from the middle outwards |
| Self-clean | EP10 On/Off | on = start a cycle, off = stop it |

`acLouverPosition` carries the raw ACW02 position number, not the ZCL "fully closed / quarter open" meaning; the Zigbee2MQTT converter exposes it as `position_1` … `position_5`.

Self-clean only runs while the AC is off in cool or dry mode. Turning it on while the AC is running switches the AC off (and to cool mode if needed), then sends the clean command 3 seconds later. Any other command cancels the cycle, and the switch turns off by itself when the AC reports the cycle finished.

### Command Confirmation and Retry

All UART transmission is done by the HVAC task, one frame at a time:

- A frame is sent only when the line has been quiet for 120 ms and at least 180 ms after the previous frame.
- After a command, the next status frame is compared with what was requested. If the AC has not confirmed it within 300 ms, the command is re-sent, up to 3 times. Retries are sent with the mute bit set so the AC does not beep again.
- If the AC still has not confirmed after the last retry (1.2 s), the driver gives up, requests the AC's real state and pushes it to Zigbee, so the coordinator shows what the AC actually did.
- While a command is waiting or unconfirmed, status frames do not overwrite the requested state.
- If a new command arrives before the previous one is confirmed, it replaces it (every frame carries the full state).

Serial log lines to look for: `Command confirmed by AC after N ms`, `Command not confirmed - retry n/3`, `Command not confirmed after 4 tries - giving up, resyncing with AC`.

### Periodic Updates

- **Keepalive + status request**: every 30 seconds, started after joining or rejoining the Zigbee network
- **Zigbee Sync**: after every state change reported by the AC

### Error Handling

- CRC validation on all received frames
- Command confirmation and retry (see above)
- Error text attribute reports HVAC errors to coordinator
- Requests the AC rejects (eco outside cool mode, fan speed while in eco) are corrected on the Zigbee side

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

- [x] Add button for factory reset
- [x] Implement command retry logic
- [x] Add OTA firmware update support
- [x] Add night mode support
- [x] Implement filter status monitoring
- [x] Detect Fahrenheit setpoints
- [x] Support horizontal swing control
- [x] Support fixed vertical louvre positions
- [x] Add clean mode support (start a self-clean cycle)
