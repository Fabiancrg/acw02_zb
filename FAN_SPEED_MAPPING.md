# ACW02 Fan Speed Mapping - 8 Speeds to Zigbee 7 Modes

## ACW02 Remote Fan Speeds

Your ACW02 remote has **8 fan speed options**:

1. **Auto** - Automatic speed control
2. **Low** (P20) - 20% power, quietest manual setting
3. **Low-Med** (P40) - 40% power
4. **Medium** (P60) - 60% power
5. **Med-High** (P80) - 80% power
6. **High** (P100) - 100% power
7. **Quiet** (SILENT) - Special quiet mode (0x06)
8. **Turbo** - Maximum cooling/heating (0x0D)

## ACW02 Protocol Values

From the ESPHome component (`acw02.h`):

```cpp
enum class Fan : uint8_t { 
    AUTO    = 0x00,  // Auto
    P20     = 0x01,  // 20% - Low
    P40     = 0x02,  // 40% - Low-Med
    P60     = 0x03,  // 60% - Medium
    P80     = 0x04,  // 80% - Med-High
    P100    = 0x05,  // 100% - High
    SILENT  = 0x06,  // Quiet/Silent
    TURBO   = 0x0D   // Turbo/Max
};
```

## Zigbee Fan Control Modes

Zigbee standard has **7 fan modes**:

| Value | Name | Z2M Display |
|-------|------|-------------|
| 0 | Off | off |
| 1 | Low | low |
| 2 | Medium | medium |
| 3 | High | high |
| 4 | On | on |
| 5 | Auto | auto |
| 6 | Smart | smart |

## Mapping Strategy

Since we have **8 ACW02 speeds** but only **7 Zigbee modes**, we need to map multiple ACW02 speeds to some Zigbee modes.

### Z2M → ACW02 (Control from Zigbee2MQTT)

When user selects in Z2M:

| Z2M Selection | Zigbee Mode | → | ACW02 Speed | Description |
|---------------|-------------|---|-------------|-------------|
| **off** | 0 | → | SILENT (0x06) | Quiet/Silent mode |
| **low** | 1 | → | P20 (0x01) | 20% - Low |
| **medium** | 2 | → | P60 (0x03) | 60% - Medium |
| **high** | 3 | → | P100 (0x05) | 100% - High |
| **on** | 4 | → | AUTO (0x00) | Auto speed |
| **auto** | 5 | → | AUTO (0x00) | Auto speed |
| **smart** | 6 | → | TURBO (0x0D) | Maximum/Turbo |

**Note:** We skip P40 and P80 when controlling from Z2M to keep the interface simple with clear Low/Med/High options.

### ACW02 → Z2M (Status reporting)

When ACW02 reports its current fan speed:

| ACW02 Speed | Value | → | Zigbee Mode | Z2M Display |
|-------------|-------|---|-------------|-------------|
| **AUTO** | 0x00 | → | 5 | auto |
| **P20** (Low) | 0x01 | → | 1 | low |
| **P40** (Low-Med) | 0x02 | → | 2 | medium |
| **P60** (Medium) | 0x03 | → | 2 | medium |
| **P80** (Med-High) | 0x04 | → | 3 | high |
| **P100** (High) | 0x05 | → | 3 | high |
| **SILENT** (Quiet) | 0x06 | → | 0 | off |
| **TURBO** | 0x0D | → | 6 | smart |

**Note:** P40 and P60 both map to "medium", P80 and P100 both map to "high" for status display.

## Why This Mapping?

### Simplification for User Interface
Having 8 speeds in the UI would be confusing. The Zigbee standard provides 7 modes which is already comprehensive:
- **off** → Quiet operation (not completely off, just very quiet)
- **low** → Quiet manual speed
- **medium** → Moderate speed (we choose P60, the middle value)
- **high** → Maximum manual speed (P100)
- **auto** → Let the AC decide
- **smart** → Turbo for rapid cooling/heating

### Direct Control Options
If you need the intermediate speeds (P40, P80) or true SILENT mode:
1. Use the physical remote
2. Create custom MQTT commands (advanced)
3. Modify the Z2M converter to expose more options

## Code Implementation

### ESP32 - Zigbee to HVAC (esp_zb_hvac.c)

```c
// When Z2M sends fan mode change
switch (fan_mode) {
    case 0: hvac_fan = HVAC_FAN_SILENT; break;    // off → Silent
    case 1: hvac_fan = HVAC_FAN_P20; break;       // low → P20
    case 2: hvac_fan = HVAC_FAN_P60; break;       // medium → P60
    case 3: hvac_fan = HVAC_FAN_P100; break;      // high → P100
    case 4: hvac_fan = HVAC_FAN_AUTO; break;      // on → Auto
    case 5: hvac_fan = HVAC_FAN_AUTO; break;      // auto → Auto
    case 6: hvac_fan = HVAC_FAN_TURBO; break;     // smart → Turbo
}
```

### ESP32 - HVAC to Zigbee (esp_zb_hvac.c)

```c
// When reporting current HVAC state
switch (state.fan_speed) {
    case HVAC_FAN_AUTO:   zigbee_fan_mode = 5; break;  // Auto → auto
    case HVAC_FAN_P20:    zigbee_fan_mode = 1; break;  // P20 → low
    case HVAC_FAN_P40:    zigbee_fan_mode = 2; break;  // P40 → medium
    case HVAC_FAN_P60:    zigbee_fan_mode = 2; break;  // P60 → medium
    case HVAC_FAN_P80:    zigbee_fan_mode = 3; break;  // P80 → high
    case HVAC_FAN_P100:   zigbee_fan_mode = 3; break;  // P100 → high
    case HVAC_FAN_SILENT: zigbee_fan_mode = 0; break;  // Silent → off
    case HVAC_FAN_TURBO:  zigbee_fan_mode = 6; break;  // Turbo → smart
}
```

## Zigbee2MQTT Converter

Updated description to clarify mapping:

```javascript
exposes.enum('fan_mode', exposes.access.ALL, ['off', 'low', 'medium', 'high', 'on', 'auto', 'smart'])
    .withDescription('Fan speed: off=Quiet, low=P20, medium=P60, high=P100, auto=Auto, smart=Turbo')
```

## Usage Examples

### In Zigbee2MQTT UI:

**Select "low":**
- ESP32 receives: Zigbee mode = 1
- ESP32 sends to ACW02: 0x01 (P20)
- ACW02 runs at 20% (Low)

**Select "medium":**
- ESP32 receives: Zigbee mode = 2
- ESP32 sends to ACW02: 0x03 (P60)
- ACW02 runs at 60% (Medium)

**Select "smart":**
- ESP32 receives: Zigbee mode = 6
- ESP32 sends to ACW02: 0x0D (TURBO)
- ACW02 runs at maximum (Turbo)

### Status Reporting:

**User presses P40 on remote:**
- ACW02 reports: 0x02 (P40)
- ESP32 maps to: Zigbee mode = 2 (medium)
- Z2M displays: "medium"

**User presses SILENT on remote:**
- ACW02 reports: 0x06 (SILENT)
- ESP32 maps to: Zigbee mode = 0 (off)
- Z2M displays: "off"

## Console Output Examples

### Changing Fan Speed:

```
I (XXX) HVAC_ZIGBEE: Fan mode changed to 2
I (XXX) HVAC_ZIGBEE: Mapped to HVAC fan: 0x03
I (XXX) HVAC_DRIVER: Setting fan speed to 0x03
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=0, Swing=0, Display=1, Fan=2
```

Translation:
- User selected "medium" in Z2M (Zigbee mode 2)
- Mapped to HVAC P60 (0x03)
- HVAC driver sends command to ACW02
- Reported back as Zigbee mode 2

### Physical Remote Used:

```
I (XXX) HVAC_DRIVER: RX [18 bytes]: Valid frame received
I (XXX) HVAC_DRIVER: Decoded state: Power=ON, Mode=1, Temp=24°C
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=1, Temp=24°C, Eco=0, Swing=0, Display=1, Fan=3
```

Translation:
- ACW02 sent status update (user changed something on remote)
- If they set fan to P80, it reports as Zigbee mode 3 (high)

## Advanced: Accessing All 8 Speeds

If you want direct access to all 8 ACW02 fan speeds, you would need to:

### Option 1: Custom MQTT Commands
Create a custom topic that accepts ACW02 fan values directly:
```bash
mosquitto_pub -t "zigbee2mqtt/ACW02-HVAC/set/hvac_fan" -m '{"fan":0x04}'  # P80
```

### Option 2: Extended Z2M Converter
Modify the converter to expose a custom enum with all 8 options:
```javascript
exposes.enum('hvac_fan_speed', exposes.access.ALL, 
    ['auto', 'p20', 'p40', 'p60', 'p80', 'p100', 'silent', 'turbo'])
```

But this would require additional converter code to handle the custom property.

### Option 3: Use Physical Remote
The simplest solution - use the physical remote for intermediate speeds, use Z2M for the main speeds.

## Summary

✅ **8 ACW02 fan speeds** properly defined in code
✅ **7 Zigbee fan modes** mapped to most useful ACW02 speeds
✅ **Bidirectional mapping** for control and status
✅ **Clear user interface** in Z2M with meaningful labels
✅ **All speeds accessible** via physical remote

The mapping prioritizes ease of use while maintaining full protocol support. You get the most commonly used speeds in Z2M, and the physical remote still gives you complete control.
