# ZHA Integration Guide - ACW02-ZB HVAC Thermostat

## Overview

The ACW02-ZB firmware uses standard Zigbee 3.0 clusters, so ZHA can discover and partially control the device out of the box. However, the custom multi-endpoint structure and non-standard fan mode values require a custom quirk for full functionality.

## Files

- **acw02_zb.py** — ZHA custom quirk (device signature + custom fan mode enum)

## Installation

### 1. Copy the quirk file

Create a directory for custom ZHA quirks in your Home Assistant config folder (if it doesn't exist):

```
/config/custom_zha_quirks/acw02_zb.py
```

### 2. Configure Home Assistant

Add the following to your `configuration.yaml`:

```yaml
zha:
  custom_quirks_path: /config/custom_zha_quirks
```

### 3. Restart and re-interview

Restart Home Assistant, then in the ZHA integration UI, select the ACW02-ZB device and click **Reconfigure** (re-interview) to apply the quirk.

---

## Device Endpoints

| Endpoint | Function                    | HA Entity Type     | Notes                        |
|----------|-----------------------------|--------------------|------------------------------|
| EP1      | Thermostat + Fan control    | Climate            | Main HVAC control            |
| EP2      | Eco mode                    | Switch             |                              |
| EP3      | Swing mode                  | Switch             |                              |
| EP4      | Display on/off              | Switch             |                              |
| EP5      | Night / sleep mode          | Switch             |                              |
| EP6      | Air purifier / ionizer      | Switch             |                              |
| EP7      | Filter clean status         | Switch (read-only) | Do not toggle — see note     |
| EP8      | Mute beep sounds            | Switch             |                              |
| EP9      | AC error status             | Switch (read-only) | Do not toggle — see note     |

---

## What Works vs Zigbee2MQTT

| Feature                          | Zigbee2MQTT        | ZHA with quirk              |
|----------------------------------|--------------------|-----------------------------|
| Climate modes (off/cool/heat/dry/fan_only) | ✅         | ✅ Standard Zigbee SystemMode values match |
| Temperature reading              | ✅                 | ✅                           |
| Temperature setpoint             | ✅                 | ✅                           |
| Eco / Swing / Display / Night / Purifier / Mute | ✅  | ✅ As switch entities        |
| Fan speed labels in climate entity | ✅ custom converter | ⚠️ Labels incorrect — see note |
| EP7 clean status as binary sensor | ✅                | ⚠️ Appears as switch — disable in HA |
| EP9 error status as binary sensor | ✅                | ⚠️ Appears as switch — disable in HA |
| Error text (locationDesc)        | ✅ Sensor entity   | ⚠️ Manual poll required     |
| runningMode / fanMode polling    | ✅ Built-in        | ⚠️ Manual automation required |

---

## Known Limitations and Workarounds

### Fan speed labels in climate entity

The ACW02 firmware uses non-standard fan mode values that differ from the Zigbee spec:

| Value | ACW02 label    | Zigbee spec label (shown without quirk) |
|-------|----------------|-----------------------------------------|
| 0x00  | Auto           | Off                                     |
| 0x01  | Low (P20)      | Low ← coincidentally correct            |
| 0x02  | Low-Med (P40)  | Medium ← close                          |
| 0x03  | Medium (P60)   | High ← wrong                            |
| 0x04  | Med-High (P80) | On ← wrong                              |
| 0x05  | High (P100)    | Auto ← wrong                            |
| 0x06  | Quiet / SILENT | Smart ← wrong                           |

The quirk corrects the attribute inspector view in ZHA, but the **climate entity** fan mode labels remain incorrect because ZHA's HVAC cluster handler uses the standard enum internally. To set fan speed from an automation, use the raw integer value:

```yaml
service: zha.set_attribute_value
data:
  ieee: "<device_ieee>"
  endpoint_id: 1
  cluster_id: 514        # 0x0202 hvacFanCtrl
  cluster_type: in
  attribute: 0           # fan_mode
  value: 3               # 3 = Medium (P60)
```

### EP7 and EP9 — read-only binary sensors

ZHA creates switch entities for EP7 (filter clean status) and EP9 (AC error status) because they use the standard `genOnOff` cluster. These are driven by the AC unit and are read-only — writing to them has no effect.

**Recommended:** disable these switch entities in the HA entity registry and create binary sensor template entities instead:

```yaml
template:
  - binary_sensor:
      - name: "ACW02 Filter Clean"
        state: "{{ states('switch.acw02_zb_filter_clean') }}"
        device_class: problem

      - name: "ACW02 Error Status"
        state: "{{ states('switch.acw02_zb_error_status') }}"
        device_class: problem
```

### Error text (locationDescription attribute)

The error message string is stored in the `genBasic` cluster's `locationDescription` attribute (cluster `0x0000`, attribute `0x0010`) on EP1. ZHA does not automatically expose this as a sensor entity.

**Workaround:** use an automation to poll and store the value periodically:

```yaml
automation:
  - alias: "ACW02 Poll Error Text"
    trigger:
      - platform: time_pattern
        minutes: "/1"
    action:
      - service: zha.get_attribute_value
        data:
          ieee: "<device_ieee>"
          endpoint_id: 1
          cluster_id: 0      # genBasic
          cluster_type: in
          attribute: 16      # locationDescription (0x0010)
        response_variable: error_response
      - service: input_text.set_value
        target:
          entity_id: input_text.acw02_error_text
        data:
          value: "{{ error_response.value }}"
```

### Polling for runningMode and fanMode

The ESP-Zigbee stack cannot auto-report `runningMode` (thermostat attribute `0x001E`) and `fanMode` (fan control attribute `0x0000`). Add a 60-second polling automation:

```yaml
automation:
  - alias: "ACW02 Poll Unreportable Attributes"
    trigger:
      - platform: time_pattern
        seconds: "/60"
    action:
      - service: zha.get_attribute_value
        data:
          ieee: "<device_ieee>"
          endpoint_id: 1
          cluster_id: 513    # 0x0201 hvacThermostat
          cluster_type: in
          attribute: 30      # runningMode (0x001E)
      - service: zha.get_attribute_value
        data:
          ieee: "<device_ieee>"
          endpoint_id: 1
          cluster_id: 514    # 0x0202 hvacFanCtrl
          cluster_type: in
          attribute: 0       # fanMode
```

---

## Device Pairing

1. Power on the device — it enters pairing mode if factory-new, or long-press the boot button for 5 s to factory reset
2. Enable **Permit joining** in ZHA
3. The device should appear with model `acw02-z` and manufacturer `Custom devices (DiY)`
4. After pairing, trigger a **Reconfigure** to bind all clusters and enable reporting

---

## Hardware

- **MCU:** Seeedstudio XIAO ESP32-C6 (or compatible ESP32-H2)
- **Firmware:** ESP-IDF with ESP-Zigbee SDK
- **Zigbee role:** Router
- **OTA:** Supported (via ZHA OTA provider)
