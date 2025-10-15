# Clean Status - Read-Only Binary Sensor

## Problem

The `clean_status` (endpoint 7) was exposed as a **switch**, which implied users could toggle it from Zigbee2MQTT. However, this is a **read-only status indicator** from the AC unit:

- ✅ The AC sets this flag when the filter needs cleaning
- ✅ Users can see the status in Z2M and Home Assistant
- ❌ Users **cannot** toggle it from Z2M (it's cleared by the AC unit itself)

## Solution

Changed `clean_status` from a **switch** to a **binary sensor**:

### 1. Changed Expose Definition

**Before:**
```javascript
e.switch().withEndpoint('clean_status').withDescription('Filter cleaning status (read-only from AC)'),
```

**After:**
```javascript
e.binary().withEndpoint('clean_status').withValueToggle('ON', 'OFF').withDescription('Filter cleaning status indicator (read-only, cleared by AC unit)'),
```

### 2. Added Custom fromZigbee Converter

**Purpose:** Only handle incoming status updates (no write capability)

```javascript
clean_status: {
    cluster: 'genOnOff',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.endpoint.ID === 7 && msg.data.hasOwnProperty('onOff')) {
            const state = msg.data['onOff'] === 1 ? 'ON' : 'OFF';
            meta.logger.info(`ACW02 fz.clean_status: Filter cleaning status: ${state}`);
            return {clean_status: state};
        }
    },
},
```

### 3. Removed toZigbee Support

**Before:** `tz.on_off` applied to all endpoints (including endpoint 7)

**After:** 
- `fz.on_off` processes **all** on/off attributeReports (endpoints 2-8)
- `fzLocal.clean_status` **specifically** handles endpoint 7 with read-only logic
- `tz.on_off` still handles writes to endpoints 2,3,4,5,6,8 (but **not** endpoint 7)

**Note:** The standard `tz.on_off` converter requires endpoint matching, so it won't accidentally write to endpoint 7 even if the user tries (no matching expose with write access).

## Result

In Zigbee2MQTT and Home Assistant:

- ✅ `clean_status` appears as a **binary sensor** (ON/OFF indicator)
- ✅ No toggle switch in UI (read-only)
- ✅ Updates automatically when AC sets/clears the flag
- ✅ Can be used in automations (e.g., notify when filter needs cleaning)
- ❌ Cannot be toggled manually from Z2M or HA

## Firmware Behavior

The ESP32 firmware (endpoint 7) reports the clean status via standard on/off attribute:

```c
// In esp_zb_hvac.c
esp_zb_zcl_set_attribute_val(
    7,  // Endpoint
    ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
    ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
    ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
    &clean_status_flag,  // 0x00 or 0x01
    false
);
```

The AC unit itself manages this flag based on:
- Runtime hours
- Filter condition sensors
- User clearing the filter and resetting via AC remote

## Home Assistant Entity

After re-pairing, the entity will be:

**Entity ID:** `binary_sensor.office_hvac_clean_status`

**Attributes:**
- State: `on` or `off`
- Device class: None (generic binary sensor)
- Entity category: Diagnostic

**Example Automation:**
```yaml
automation:
  - alias: "Notify when HVAC filter needs cleaning"
    trigger:
      - platform: state
        entity_id: binary_sensor.office_hvac_clean_status
        to: 'on'
    action:
      - service: notify.mobile_app
        data:
          message: "Office HVAC filter needs cleaning!"
          title: "Maintenance Required"
```

## Debug Logging

When the AC changes clean status, you'll see:

```
info: z2m: Received Zigbee message from 'Office_HVAC', type 'attributeReport', cluster 'genOnOff', data '{"onOff":1}' endpoint 7
info: z2m: ACW02 fz.clean_status: Filter cleaning status: ON
info: z2m: Publishing MQTT to 'zigbee2mqtt/Office_HVAC' payload: '{"clean_status":"ON",...}'
```

When cleared by the AC:

```
info: z2m: ACW02 fz.clean_status: Filter cleaning status: OFF
```

## Summary

| Aspect | Before | After |
|--------|--------|-------|
| Entity Type | Switch | Binary Sensor |
| User Control | ❌ Appeared controllable (but shouldn't be) | ✅ Read-only (correct behavior) |
| Z2M UI | Toggle switch | Status indicator only |
| Home Assistant | `switch.clean_status` | `binary_sensor.clean_status` |
| MQTT Write | Accepted but ignored | Not accepted |
| MQTT Read | ✅ Works | ✅ Works |
| Reporting | ✅ Works | ✅ Works |

The change makes the UI/API match the actual device behavior - users can **monitor** the filter status but cannot toggle it manually (only the AC unit can change it).
