# Zigbee2MQTT Integration Guide - ACW02 HVAC Thermostat

## Overview
This guide explains how to integrate the ACW02 HVAC Thermostat converter into the official Zigbee2MQTT repository.

## Files Created
- **acw02-zb.ts** - TypeScript converter definition for upstream contribution
- **acw02-zb.js** - JavaScript converter for local/external use

## Steps to Integrate into Zigbee2MQTT

### 1. Fork and Clone the Repository
```bash
# Fork the repository on GitHub first
git clone https://github.com/YOUR_USERNAME/zigbee-herdsman-converters.git
cd zigbee-herdsman-converters
```

### 2. Create a New Branch
```bash
git checkout -b add-acw02-hvac-thermostat
```

### 3. Add the Device Definition

Open the file: `src/devices/custom_devices_diy.ts`

Add the following import at the top (if not already present):
```typescript
import type {DefinitionWithExtend, Fz, Tz, KeyValue} from '../lib/types';
```

Then add the converter definition to the `definitions` array in `custom_devices_diy.ts`:

```typescript
export const definitions: DefinitionWithExtend[] = [
    // ... existing devices ...
    
    {
        zigbeeModel: ['acw02-z'],
        model: 'ACW02-ZB',
        vendor: 'Custom devices (DiY)',
        description: 'ACW02 HVAC Thermostat Controller via Zigbee (Router)',
        
        meta: {
            multiEndpoint: true,
        },
        
        fromZigbee: [
            fzLocal.acw02_thermostat,
            fzLocal.acw02_fan_mode,
            fzLocal.acw02_clean_status,
            fzLocal.acw02_error_status,
            fz.on_off,
            fzLocal.acw02_error_text,
        ],
        toZigbee: [
            tz.thermostat_local_temperature,
            tz.thermostat_occupied_heating_setpoint,
            tz.thermostat_system_mode,
            tzLocal.acw02_fan_mode,
            tz.on_off,
        ],
        
        exposes: [
            e.climate()
                .withSetpoint('occupied_heating_setpoint', 16, 31, 1)
                .withLocalTemperature()
                .withSystemMode(['off', 'auto', 'cool', 'heat', 'dry', 'fan_only'])
                .withRunningState(['idle', 'heat', 'cool', 'fan_only']),
            exposes.enum('fan_mode', ea.ALL, ['quiet', 'low', 'low-med', 'medium', 'med-high', 'high', 'auto'])
                .withDescription('Fan speed: Quiet=SILENT, Low=P20, Low-Med=P40, Medium=P60, Med-High=P80, High=P100, Auto=AUTO'),
            exposes.text('error_text', ea.STATE_GET)
                .withDescription('Error message from AC unit (read-only)'),
            exposes.binary('ac_error_status', ea.STATE_GET, 'ON', 'OFF')
                .withDescription('Error status indicator (read-only)'),
            e.switch().withEndpoint('eco_mode').withDescription('Eco mode'),
            e.switch().withEndpoint('swing_mode').withDescription('Swing mode'),
            e.switch().withEndpoint('display').withDescription('Display control'),
            e.switch().withEndpoint('night_mode').withDescription('Night/sleep mode'),
            e.switch().withEndpoint('purifier').withDescription('Air purifier/ionizer'),
            exposes.binary('filter_clean_status', ea.STATE_GET, 'ON', 'OFF')
                .withDescription('Filter cleaning reminder (read-only)'),
            e.switch().withEndpoint('mute').withDescription('Mute beep sounds'),
        ],
        
        endpoint: (device) => {
            return {
                'default': 1,
                'eco_mode': 2,
                'swing_mode': 3,
                'display': 4,
                'night_mode': 5,
                'purifier': 6,
                'clean_sensor': 7,
                'mute': 8,
                'error_sensor': 9,
            };
        },
        
        configure: async (device, coordinatorEndpoint, logger) => {
            const endpoint1 = device.getEndpoint(1);
            const endpoint2 = device.getEndpoint(2);
            const endpoint3 = device.getEndpoint(3);
            const endpoint4 = device.getEndpoint(4);
            const endpoint5 = device.getEndpoint(5);
            const endpoint6 = device.getEndpoint(6);
            const endpoint7 = device.getEndpoint(7);
            const endpoint8 = device.getEndpoint(8);
            const endpoint9 = device.getEndpoint(9);
            
            await reporting.bind(endpoint1, coordinatorEndpoint, ['genBasic', 'hvacThermostat', 'hvacFanCtrl']);
            await reporting.thermostatTemperature(endpoint1);
            await reporting.thermostatOccupiedHeatingSetpoint(endpoint1);
            await endpoint1.configureReporting('hvacThermostat', [{
                attribute: 'systemMode',
                minimumReportInterval: 1,
                maximumReportInterval: 300,
                reportableChange: 1,
            }]);
            
            // Configure all switch endpoints
            for (const ep of [endpoint2, endpoint3, endpoint4, endpoint5, endpoint6, endpoint7, endpoint8, endpoint9]) {
                await reporting.bind(ep, coordinatorEndpoint, ['genOnOff']);
                await reporting.onOff(ep);
            }
            
            // Initial read of unreportable attributes
            try {
                await endpoint1.read('hvacThermostat', ['runningMode']);
                await endpoint1.read('genBasic', ['locationDesc']);
                await endpoint1.read('hvacFanCtrl', ['fanMode']);
            } catch (error) {
                logger?.warn(`ACW02: Initial read failed: ${(error as Error).message}`);
            }
        },
        
        extend: [
            m.poll({
                key: "acw02_state",
                option: e.numeric("acw02_poll_interval", ea.SET)
                    .withValueMin(-1)
                    .withDescription("Polling interval in seconds for unreportable attributes (default: 60s, -1 to disable)"),
                defaultIntervalSeconds: 60,
                poll: async (device) => {
                    const endpoint1 = device.getEndpoint(1);
                    if (!endpoint1) return;
                    
                    try {
                        await endpoint1.read('hvacThermostat', ['runningMode']);
                        await endpoint1.read('hvacFanCtrl', ['fanMode']);
                        await endpoint1.read('genBasic', ['locationDesc']);
                    } catch (error) {
                        console.error(`ACW02 polling failed: ${(error as Error).message}`);
                    }
                },
            }),
        ],
        ota: true,
    },
];
```

### 4. Add Custom Converters (fzLocal and tzLocal)

Add these converters to the `fzLocal` and `tzLocal` objects in `custom_devices_diy.ts`:

**In `tzLocal` object:**
```typescript
const tzLocal = {
    // ... existing converters ...
    
    acw02_fan_mode: {
        key: ['fan_mode'],
        convertSet: async (entity, key, value, meta) => {
            const fanModeMap: {[key: string]: number} = {
                'quiet': 0x06, 'low': 0x01, 'low-med': 0x02,
                'medium': 0x03, 'med-high': 0x04, 'high': 0x05, 'auto': 0x00,
            };
            const numericValue = fanModeMap[value as string];
            if (numericValue !== undefined) {
                await entity.write('hvacFanCtrl', {'fanMode': numericValue});
                return {state: {fan_mode: value}};
            }
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('hvacFanCtrl', ['fanMode']);
        },
    } satisfies Tz.Converter,
};
```

**In `fzLocal` object:**
```typescript
const fzLocal = {
    // ... existing converters ...
    
    acw02_fan_mode: {
        cluster: 'hvacFanCtrl',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.hasOwnProperty('fanMode')) {
                const fanModeMap: {[key: number]: string} = {
                    0x00: 'auto', 0x01: 'low', 0x02: 'low-med',
                    0x03: 'medium', 0x04: 'med-high', 0x05: 'high',
                    0x06: 'quiet', 0x0D: 'quiet',
                };
                return {fan_mode: fanModeMap[msg.data.fanMode as number]};
            }
        },
    } satisfies Fz.Converter<'hvacFanCtrl', undefined, ['attributeReport', 'readResponse']>,
    
    acw02_clean_status: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 7 && msg.data.hasOwnProperty('onOff')) {
                return {filter_clean_status: msg.data['onOff'] === 1 ? 'ON' : 'OFF'};
            }
        },
    } satisfies Fz.Converter<'genOnOff', undefined, ['attributeReport', 'readResponse']>,
    
    acw02_error_status: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 9 && msg.data.hasOwnProperty('onOff')) {
                return {ac_error_status: msg.data['onOff'] === 1 ? 'ON' : 'OFF'};
            }
        },
    } satisfies Fz.Converter<'genOnOff', undefined, ['attributeReport', 'readResponse']>,
    
    acw02_error_text: {
        cluster: 'genBasic',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 1 && msg.data['locationDesc'] !== undefined) {
                let errorText = '';
                const locationDesc = msg.data['locationDesc'];
                if (typeof locationDesc === 'string') {
                    errorText = locationDesc;
                } else if (Array.isArray(locationDesc) && locationDesc.length > 0) {
                    const textLength = locationDesc[0];
                    errorText = String.fromCharCode.apply(null, locationDesc.slice(1, 1 + textLength));
                }
                return {error_text: errorText.trim()};
            }
        },
    } satisfies Fz.Converter<'genBasic', undefined, ['attributeReport', 'readResponse']>,
    
    acw02_thermostat: {
        cluster: 'hvacThermostat',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result: KeyValue = {};
            if (msg.data.hasOwnProperty('localTemp')) {
                result.local_temperature = (msg.data.localTemp as number) / 100;
            }
            if (msg.data.hasOwnProperty('runningMode')) {
                const modeMap: {[key: number]: string} = {
                    0x00: 'idle', 0x03: 'cool', 0x04: 'heat', 0x07: 'fan_only',
                };
                result.running_state = modeMap[msg.data.runningMode as number] || 'idle';
            }
            if (msg.data.hasOwnProperty('systemMode')) {
                const sysModeMap: {[key: number]: string} = {
                    0x00: 'off', 0x01: 'auto', 0x03: 'cool',
                    0x04: 'heat', 0x07: 'fan_only', 0x08: 'dry',
                };
                result.system_mode = sysModeMap[msg.data.systemMode as number] || 'off';
            }
            if (msg.data.hasOwnProperty('occupiedHeatingSetpoint')) {
                result.occupied_heating_setpoint = (msg.data.occupiedHeatingSetpoint as number) / 100;
            } else if (msg.data.hasOwnProperty('occupiedCoolingSetpoint')) {
                result.occupied_heating_setpoint = (msg.data.occupiedCoolingSetpoint as number) / 100;
            }
            return result;
        },
    } satisfies Fz.Converter<'hvacThermostat', undefined, ['attributeReport', 'readResponse']>,
};
```

### 5. Test Locally

```bash
# Build the project
npm ci
npm run build

# The built converter will be in dist/
```

### 6. Commit and Push

```bash
git add src/devices/custom_devices_diy.ts
git commit -m "Add support for ACW02 HVAC Thermostat (Custom devices DiY)

- Multi-endpoint Zigbee HVAC thermostat controller
- Supports temperature control, fan speeds, and multiple switches
- 9 endpoints: main thermostat + eco/swing/display/night/purifier/clean/mute/error
- Custom fan modes: quiet, low, low-med, medium, med-high, high, auto
- Error reporting via locationDesc attribute
- Filter cleaning status indicator
- OTA update support"

git push origin add-acw02-hvac-thermostat
```

### 7. Create Pull Request

1. Go to https://github.com/Koenkk/zigbee-herdsman-converters
2. Click "New Pull Request"
3. Select your branch
4. Fill in the PR template:

**Title:** Add support for ACW02 HVAC Thermostat (Custom devices DiY)

**Description:**
```markdown
## Description
Adds support for ACW02 HVAC Thermostat - a custom DIY Zigbee device based on ESP32-C6.

## Device Details
- **Model:** ACW02-ZB
- **Vendor:** Custom devices (DiY)
- **Zigbee Model ID:** acw02-z
- **Manufacturer Name:** Custom devices (DiY)
- **Device Type:** Router
- **Chip:** ESP32-C6 with ESP-Zigbee SDK

## Features
- **Climate Control:**
  - Temperature setpoint: 16-31°C (single setpoint for both heating/cooling)
  - Local temperature reading
  - System modes: off, auto, cool, heat, dry, fan_only
  - Running state: idle, heat, cool, fan_only

- **Fan Control:**
  - Custom fan speeds: quiet, low, low-med, medium, med-high, high, auto
  - Maps to ACW02 protocol values (SILENT, P20, P40, P60, P80, P100, AUTO)

- **Switches (9 endpoints total):**
  - Eco mode (endpoint 2)
  - Swing mode (endpoint 3)
  - Display control (endpoint 4)
  - Night/sleep mode (endpoint 5)
  - Air purifier/ionizer (endpoint 6)
  - Mute beep sounds (endpoint 8)

- **Read-only Sensors:**
  - Filter cleaning status (endpoint 7)
  - Error status indicator (endpoint 9)
  - Error text messages (via locationDesc attribute)

- **Additional:**
  - OTA firmware updates supported
  - Optimized reporting (most attributes auto-report via REPORTING flag)
  - Minimal polling for unreportable attributes (runningMode, fanMode, error_text)

## Testing
- Tested with Zigbee2MQTT version: [YOUR_VERSION]
- Coordinator: [YOUR_COORDINATOR]
- All features verified working

## Screenshots
[Add screenshots of the device in Zigbee2MQTT UI if possible]
```

## Device Information for Users

Once merged, users can find the device in Zigbee2MQTT as:

**Supported Devices → Custom devices (DiY) → ACW02-ZB**

### Device Pairing
1. Power on the device
2. It will automatically enter pairing mode (factory new)
3. Permit joining in Zigbee2MQTT
4. Device should appear with all endpoints

### Configuration Options
- `acw02_poll_interval`: Polling interval for unreportable attributes (default: 60s, set to -1 to disable)

## External Converter (Temporary Use)

While waiting for PR approval, users can use `acw02-zb.js` as an external converter:

1. Copy `acw02-zb.js` to your Zigbee2MQTT configuration directory
2. Edit `configuration.yaml`:
```yaml
external_converters:
  - acw02-zb.js
```
3. Restart Zigbee2MQTT

## Notes

- **Manufacturer Name:** Device reports "Custom devices (DiY)" matching the Zigbee2MQTT vendor category
- **Multi-endpoint:** Uses 9 endpoints for complete AC control
- **Reporting Optimization:** Most attributes use ESP_ZB_ZCL_ATTR_ACCESS_REPORTING flag for automatic updates
- **Polling:** Only 3 attributes need polling (runningMode, fanMode, error_text) due to ESP-Zigbee stack limitations

## Hardware Information

- **MCU:** ESP32-C6 (XIAO ESP32-C6 board)
- **UART:** Connected to ACW02 AC unit via custom protocol
- **Power:** 5V via USB or direct connection
- **Antenna:** Supports both onboard and external antenna (FM8625H switch)

## Future Enhancements

Potential improvements for future versions:
- Additional error code decoding (currently only CL confirmed)
- TURBO fan mode support (currently mapped to quiet)
- Temperature sensor calibration
- Energy monitoring (if AC supports it)
