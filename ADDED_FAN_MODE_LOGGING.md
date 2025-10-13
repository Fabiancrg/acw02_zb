# Added Fan Mode to Attribute Updates

## Problem

When changing fan mode in Zigbee2MQTT, the console only showed:
```
I (XXX) HVAC_ZIGBEE: Fan mode changed to 3
```

But the periodic update log didn't include fan mode:
```
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=22°C, Eco=0, Swing=0, Display=1
```

Fan mode was missing from the attribute update summary.

## Root Cause

The `hvac_update_zigbee_attributes()` function was updating:
- ✅ Thermostat system mode
- ✅ Temperature setpoints
- ✅ Eco mode switch
- ✅ Swing switch
- ✅ Display switch
- ❌ **Fan mode** ← Missing!

The fan mode attribute was never being updated or logged.

## Solution

Added fan mode attribute update to `hvac_update_zigbee_attributes()`:

```c
/* Update Fan Mode - Endpoint 1 */
// Map HVAC fan speed to Zigbee fan mode
uint8_t zigbee_fan_mode = 5;  // Default to auto
switch (state.fan_speed) {
    case HVAC_FAN_AUTO:   zigbee_fan_mode = 5; break;  // Auto
    case HVAC_FAN_P20:    zigbee_fan_mode = 1; break;  // Low
    case HVAC_FAN_P40:    zigbee_fan_mode = 2; break;  // Medium
    case HVAC_FAN_P60:    zigbee_fan_mode = 3; break;  // High
    case HVAC_FAN_P80:    zigbee_fan_mode = 3; break;  // High
    case HVAC_FAN_P100:   zigbee_fan_mode = 6; break;  // Smart/Max
    case HVAC_FAN_SILENT: zigbee_fan_mode = 1; break;  // Low
    case HVAC_FAN_TURBO:  zigbee_fan_mode = 6; break;  // Smart/Max
    default:              zigbee_fan_mode = 5; break;  // Auto
}

esp_zb_zcl_set_attribute_val(HA_ESP_HVAC_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL,
                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                             ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID,
                             &zigbee_fan_mode, false);

ESP_LOGI(TAG, "Updated Zigbee attributes: Mode=%d, Temp=%d°C, Eco=%d, Swing=%d, Display=%d, Fan=%d", 
         system_mode, state.target_temp_c, state.eco_mode, state.swing_on, state.display_on, zigbee_fan_mode);
```

## Fan Mode Mapping

### HVAC Fan Speed → Zigbee Fan Mode

| HVAC Fan Speed | Zigbee Mode | Z2M Display |
|----------------|-------------|-------------|
| HVAC_FAN_AUTO (0x00) | 5 | auto |
| HVAC_FAN_P20 (0x01) | 1 | low |
| HVAC_FAN_P40 (0x02) | 2 | medium |
| HVAC_FAN_P60 (0x03) | 3 | high |
| HVAC_FAN_P80 (0x04) | 3 | high |
| HVAC_FAN_P100 (0x05) | 6 | smart |
| HVAC_FAN_SILENT (0x06) | 1 | low |
| HVAC_FAN_TURBO (0x0D) | 6 | smart |

### Zigbee Fan Modes (Standard)

| Value | Name | Description |
|-------|------|-------------|
| 0 | Off | Fan off |
| 1 | Low | Low speed |
| 2 | Medium | Medium speed |
| 3 | High | High speed |
| 4 | On | Fan on (auto speed) |
| 5 | Auto | Automatic control |
| 6 | Smart | Smart/intelligent mode |

## Expected New Output

### When Fan Mode Changes:

**Immediate log (from attribute handler):**
```
I (XXX) HVAC_ZIGBEE: Fan mode changed to 3
```

**Periodic update (every 30s or after changes):**
```
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=22°C, Eco=0, Swing=0, Display=1, Fan=3
                                                                                               ^^^^^^
                                                                                               Now shows fan mode!
```

### When Any Attribute Changes:

The log will always show all attributes including fan mode:
```
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=1, Swing=0, Display=1, Fan=5
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=1, Swing=1, Display=1, Fan=5
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=1, Swing=1, Display=1, Fan=2
```

## How It Works

### Fan Mode Change Flow:

1. **User changes fan mode in Z2M UI** → Selects "medium"

2. **Z2M sends command** to endpoint 1:
   ```
   ESP_ZB_ZCL_CLUSTER_ID_FAN_CONTROL
   ESP_ZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID = 2
   ```

3. **ESP32 receives attribute change:**
   ```c
   zb_attribute_handler() receives fan mode = 2
   ```

4. **Logs immediate change:**
   ```
   I (XXX) HVAC_ZIGBEE: Fan mode changed to 2
   ```

5. **Maps to HVAC fan speed:**
   ```c
   case 2: hvac_fan = HVAC_FAN_P40; break;  // Medium = 40%
   ```

6. **Sends to HVAC device:**
   ```c
   hvac_set_fan_speed(HVAC_FAN_P40);
   ```

7. **Schedules attribute update:**
   ```c
   esp_zb_scheduler_alarm(hvac_update_zigbee_attributes, 0, 500);
   ```

8. **After 500ms, updates all attributes:**
   ```c
   hvac_update_zigbee_attributes() called
   ```

9. **Reads current HVAC state:**
   ```c
   hvac_get_state(&state);
   // state.fan_speed = HVAC_FAN_P40
   ```

10. **Maps back to Zigbee:**
    ```c
    case HVAC_FAN_P40: zigbee_fan_mode = 2; break;
    ```

11. **Updates Zigbee attribute:**
    ```c
    esp_zb_zcl_set_attribute_val(...FAN_CONTROL..., &zigbee_fan_mode, false);
    ```

12. **Logs complete state:**
    ```
    I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=1, Swing=1, Display=1, Fan=2
    ```

## Benefits

### Before:
- ❌ Fan mode changes not visible in periodic updates
- ❌ Had to rely on immediate "Fan mode changed to X" message
- ❌ Incomplete attribute summary

### After:
- ✅ Fan mode included in all attribute updates
- ✅ Complete state visibility in one log line
- ✅ Easier to verify state after multiple changes
- ✅ Consistent logging format for all attributes

## Testing

### 1. Rebuild and Flash
```powershell
idf.py build
idf.py flash monitor
```

### 2. Change Fan Mode in Z2M
Select different fan modes: low, medium, high, auto, smart

### 3. Watch Console Output
You should now see:
```
I (XXX) HVAC_ZIGBEE: Fan mode changed to 2
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=0, Swing=0, Display=1, Fan=2
```

### 4. Change Multiple Attributes
Change temperature, mode, eco, swing, display, and fan

Each change should trigger an update showing ALL attributes including fan:
```
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=25°C, Eco=1, Swing=1, Display=1, Fan=5
```

### 5. Wait for Periodic Update
Every 30 seconds, you'll see the complete state including fan mode:
```
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=1, Swing=0, Display=1, Fan=3
```

## Summary

✅ Added fan mode attribute update to `hvac_update_zigbee_attributes()`
✅ Maps HVAC fan speeds to Zigbee fan modes
✅ Updates Fan Control cluster attribute on endpoint 1
✅ Includes fan mode in attribute summary log
✅ Complete state visibility in console output

**Fan mode changes are now fully visible in all attribute updates!** 🎉
