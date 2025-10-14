/**
 * Zigbee2MQTT External Converter for ACW02 HVAC Thermostat
 * 
 * This file defines the device for Zigbee2MQTT to properly expose all controls.
 * 
 * Installation:
 * 1. Copy this file to your Zigbee2MQTT data directory (e.g., /opt/zigbee2mqtt/data/)
 * 2. Edit your Zigbee2MQTT configuration.yaml and add:
 *    external_converters:
 *      - zigbee2mqtt_converter.js
 * 3. Restart Zigbee2MQTT
 * 4. Re-pair the device or click "Reconfigure" in the Z2M UI
 */

const fz = require('zigbee-herdsman-converters/converters/fromZigbee');
const tz = require('zigbee-herdsman-converters/converters/toZigbee');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const reporting = require('zigbee-herdsman-converters/lib/reporting');
const e = exposes.presets;

// Custom converters for named switches and custom fan modes
const tzLocal = {
    fan_mode: {
        key: ['fan_mode'],
        convertSet: async (entity, key, value, meta) => {
            // Map custom fan mode names to ACW02 protocol values
            const fanModeMap = {
                'quiet': 0x06,   // SILENT
                'low': 0x01,     // P20
                'low-med': 0x02, // P40
                'medium': 0x03,  // P60
                'med-high': 0x04,// P80
                'high': 0x05,    // P100
                'auto': 0x00,    // AUTO
            };
            const numericValue = fanModeMap[value];
            if (numericValue !== undefined) {
                await entity.write('hvacFanCtrl', {'fanMode': numericValue});
                return {state: {fan_mode: value}};
            }
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('hvacFanCtrl', ['fanMode']);
        },
    },
    eco_mode: {
        key: ['eco_mode'],
        convertSet: async (entity, key, value, meta) => {
            // Use command instead of write (on/off are commands, not attributes)
            if (value === 'ON') {
                await entity.command('genOnOff', 'on', {}, {disableDefaultResponse: true});
            } else {
                await entity.command('genOnOff', 'off', {}, {disableDefaultResponse: true});
            }
            return {state: {eco_mode: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('genOnOff', ['onOff']);
        },
    },
    swing_mode: {
        key: ['swing_mode'],
        convertSet: async (entity, key, value, meta) => {
            // Use command instead of write
            if (value === 'ON') {
                await entity.command('genOnOff', 'on', {}, {disableDefaultResponse: true});
            } else {
                await entity.command('genOnOff', 'off', {}, {disableDefaultResponse: true});
            }
            return {state: {swing_mode: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('genOnOff', ['onOff']);
        },
    },
    display: {
        key: ['display'],
        convertSet: async (entity, key, value, meta) => {
            // Use command instead of write
            if (value === 'ON') {
                await entity.command('genOnOff', 'on', {}, {disableDefaultResponse: true});
            } else {
                await entity.command('genOnOff', 'off', {}, {disableDefaultResponse: true});
            }
            return {state: {display: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('genOnOff', ['onOff']);
        },
    },
    night_mode: {
        key: ['night_mode'],
        convertSet: async (entity, key, value, meta) => {
            // Use command instead of write
            if (value === 'ON') {
                await entity.command('genOnOff', 'on', {}, {disableDefaultResponse: true});
            } else {
                await entity.command('genOnOff', 'off', {}, {disableDefaultResponse: true});
            }
            return {state: {night_mode: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('genOnOff', ['onOff']);
        },
    },
    purifier: {
        key: ['purifier'],
        convertSet: async (entity, key, value, meta) => {
            // Use command instead of write
            if (value === 'ON') {
                await entity.command('genOnOff', 'on', {}, {disableDefaultResponse: true});
            } else {
                await entity.command('genOnOff', 'off', {}, {disableDefaultResponse: true});
            }
            return {state: {purifier: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('genOnOff', ['onOff']);
        },
    },
    mute: {
        key: ['mute'],
        convertSet: async (entity, key, value, meta) => {
            // Use command instead of write
            if (value === 'ON') {
                await entity.command('genOnOff', 'on', {}, {disableDefaultResponse: true});
            } else {
                await entity.command('genOnOff', 'off', {}, {disableDefaultResponse: true});
            }
            return {state: {mute: value}};
        },
        convertGet: async (entity, key, meta) => {
            await entity.read('genOnOff', ['onOff']);
        },
    },
};

const fzLocal = {
    fan_mode: {
        cluster: 'hvacFanCtrl',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.data.hasOwnProperty('fanMode')) {
                // Map ACW02 protocol values to custom fan mode names
                const fanModeMap = {
                    0x00: 'auto',     // AUTO
                    0x01: 'low',      // P20
                    0x02: 'low-med',  // P40
                    0x03: 'medium',   // P60
                    0x04: 'med-high', // P80
                    0x05: 'high',     // P100
                    0x06: 'quiet',    // SILENT
                    0x0D: 'quiet',    // TURBO (map to quiet for now)
                };
                return {fan_mode: fanModeMap[msg.data.fanMode]};
            }
        },
    },
    eco_mode: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 2) {
                return {eco_mode: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            }
        },
    },
    swing_mode: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 3) {
                return {swing_mode: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            }
        },
    },
    display: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 4) {
                return {display: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            }
        },
    },
    night_mode: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 5) {
                return {night_mode: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            }
        },
    },
    purifier: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 6) {
                return {purifier: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            }
        },
    },
    clean_status: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 7) {
                return {clean_status: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            }
        },
    },
    mute: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 8) {
                return {mute: msg.data.onOff === 1 ? 'ON' : 'OFF'};
            }
        },
    },
    error_text: {
        cluster: 'genBasic',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 1 && msg.data['locationDesc'] !== undefined) {
                // Zigbee string: first byte is length, rest is text
                const errorTextBytes = msg.data['locationDesc'];
                if (errorTextBytes && errorTextBytes.length > 0) {
                    const textLength = errorTextBytes[0];
                    const textData = errorTextBytes.slice(1, 1 + textLength);
                    const errorText = String.fromCharCode.apply(null, textData);
                    return {error_text: errorText || ''};  // Empty string when no error
                }
            }
        },
    },
    // Custom converter for thermostat to add _ep1 suffix for endpoint 1
    thermostat_ep1: {
        cluster: 'hvacThermostat',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            if (msg.data.hasOwnProperty('localTemp')) {
                result.local_temperature_ep1 = msg.data.localTemp / 100;
            }
            if (msg.data.hasOwnProperty('runningMode')) {
                // runningMode is an 8-bit enum: 0x00=idle, 0x03=cool, 0x04=heat, 0x07=fan
                const modeMap = {
                    0x00: 'idle',
                    0x03: 'cool',
                    0x04: 'heat',
                    0x07: 'fan_only',
                };
                result.running_state_ep1 = modeMap[msg.data.runningMode] || 'idle';
            }
            if (msg.data.hasOwnProperty('systemMode')) {
                const sysModeMap = {
                    0x00: 'off',
                    0x01: 'auto',
                    0x03: 'cool',
                    0x04: 'heat',
                    0x07: 'fan_only',
                    0x08: 'dry',
                };
                result.system_mode_ep1 = sysModeMap[msg.data.systemMode] || 'off';
            }
            if (msg.data.hasOwnProperty('occupiedHeatingSetpoint')) {
                result.occupied_heating_setpoint_ep1 = msg.data.occupiedHeatingSetpoint / 100;
            }
            if (msg.data.hasOwnProperty('occupiedCoolingSetpoint')) {
                result.occupied_cooling_setpoint_ep1 = msg.data.occupiedCoolingSetpoint / 100;
            }
            return result;
        },
    },
};

const definition = {
    zigbeeModel: ['acw02-z'],
    model: 'ACW02-ZB',
    vendor: 'ESPRESSIF',
    description: 'ACW02 HVAC Thermostat Controller via Zigbee',
    
    // Supported features
    fromZigbee: [
        fzLocal.thermostat_ep1,  // Custom thermostat converter with _ep1 suffix
        fzLocal.fan_mode,
        fzLocal.eco_mode,
        fzLocal.swing_mode,
        fzLocal.display,
        fzLocal.night_mode,
        fzLocal.purifier,
        fzLocal.clean_status,
        fzLocal.mute,
        fzLocal.error_text,
    ],
    toZigbee: [
        tz.thermostat_local_temperature,
        tz.thermostat_occupied_heating_setpoint,
        tz.thermostat_occupied_cooling_setpoint,
        tz.thermostat_system_mode,
        tzLocal.fan_mode,
        tzLocal.eco_mode,
        tzLocal.swing_mode,
        tzLocal.display,
        tzLocal.night_mode,
        tzLocal.purifier,
        tzLocal.mute,
        // Note: clean_status is read-only (from AC), so no toZigbee converter needed
    ],
    
    // Expose controls in the UI
    exposes: [
        e.climate()
            .withSetpoint('occupied_heating_setpoint', 16, 31, 1)
            .withSetpoint('occupied_cooling_setpoint', 16, 31, 1)
            .withLocalTemperature()
            .withSystemMode(['off', 'auto', 'cool', 'heat', 'dry', 'fan_only'])
            .withRunningState(['idle', 'heat', 'cool', 'fan_only'])
            .withEndpoint('ep1'),
        exposes.enum('fan_mode', exposes.access.ALL, ['quiet', 'low', 'low-med', 'medium', 'med-high', 'high', 'auto'])
            .withDescription('Fan speed mapped to ACW02: Quiet=SILENT, Low=P20, Low-Med=P40, Medium=P60, Med-High=P80, High=P100, Auto=AUTO')
            .withEndpoint('ep1'),
        exposes.binary('eco_mode', exposes.access.ALL, 'ON', 'OFF')
            .withDescription('Eco mode')
            .withEndpoint('ep2'),
        exposes.binary('swing_mode', exposes.access.ALL, 'ON', 'OFF')
            .withDescription('Swing mode')
            .withEndpoint('ep3'),
        exposes.binary('display', exposes.access.ALL, 'ON', 'OFF')
            .withDescription('Display on/off')
            .withEndpoint('ep4'),
        exposes.binary('night_mode', exposes.access.ALL, 'ON', 'OFF')
            .withDescription('Night mode (sleep mode with adjusted settings)')
            .withEndpoint('ep5'),
        exposes.binary('purifier', exposes.access.ALL, 'ON', 'OFF')
            .withDescription('Air purifier/ionizer')
            .withEndpoint('ep6'),
        exposes.binary('clean_status', exposes.access.STATE_GET, 'ON', 'OFF')
            .withDescription('Filter cleaning status (read-only from AC)')
            .withEndpoint('ep7'),
        exposes.binary('mute', exposes.access.ALL, 'ON', 'OFF')
            .withDescription('Mute beep sounds on AC')
            .withEndpoint('ep8'),
        exposes.text('error_text', exposes.access.STATE_GET)
            .withDescription('Error message text from AC (empty when no error, read-only)')
            .withEndpoint('ep1'),
    ],
    
    // Map endpoints with descriptive names
    endpoint: (device) => {
        return {
            'ep1': 1,          // Main thermostat (includes error_text)
            'ep2': 2,          // Eco mode switch
            'ep3': 3,          // Swing switch
            'ep4': 4,          // Display switch
            'ep5': 5,          // Night mode switch
            'ep6': 6,          // Purifier switch
            'ep7': 7,          // Clean status binary sensor
            'ep8': 8,          // Mute switch
        };
    },
    
    // Configure reporting
    configure: async (device, coordinatorEndpoint, logger) => {
        const endpoint1 = device.getEndpoint(1);
        const endpoint2 = device.getEndpoint(2);
        const endpoint3 = device.getEndpoint(3);
        const endpoint4 = device.getEndpoint(4);
        
        // Bind clusters for main thermostat (endpoint 1)
        await reporting.bind(endpoint1, coordinatorEndpoint, [
            'genBasic',
            'hvacThermostat', 
            'hvacFanCtrl',
        ]);
        
        // Configure reporting for thermostat attributes
        // localTemp (current temperature) is reportable
        await reporting.thermostatTemperature(endpoint1);
        
        // Note: runningMode (0x001E) is NOT reportable - we poll it instead
        
        // Bind on/off cluster for switches (endpoints 2-8)
        // and fanMode are NOT reportable attributes in ESP-Zigbee stack
        // Z2M will poll these when needed or they can be read manually
        
        // Bind and configure endpoint 2 (Eco mode)
        await reporting.bind(endpoint2, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint2);
        
        // Bind and configure endpoint 3 (Swing)
        await reporting.bind(endpoint3, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint3);
        
        // Bind and configure endpoint 4 (Display)
        await reporting.bind(endpoint4, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint4);
        
        // Bind and configure endpoint 5 (Night mode)
        const endpoint5 = device.getEndpoint(5);
        await reporting.bind(endpoint5, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint5);
        
        // Bind and configure endpoint 6 (Purifier)
        const endpoint6 = device.getEndpoint(6);
        await reporting.bind(endpoint6, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint6);
        
        // Bind and configure endpoint 7 (Clean status - read-only)
        const endpoint7 = device.getEndpoint(7);
        await reporting.bind(endpoint7, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint7);
        
        // Bind and configure endpoint 8 (Mute)
        const endpoint8 = device.getEndpoint(8);
        await reporting.bind(endpoint8, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint8);
    },
    
    // Poll unreportable attributes on device events and with periodic timer
    onEvent: async (type, data, device, options, state) => {
        const endpoint1 = device.getEndpoint(1);
        if (!endpoint1) return;
        
        // Helper function to poll unreportable attributes
        const pollAttributes = async () => {
            try {
                // Read thermostat attributes (runningMode is unreportable, needs polling)
                await endpoint1.read('hvacThermostat', ['runningMode', 'systemMode', 
                                                        'occupiedHeatingSetpoint', 
                                                        'occupiedCoolingSetpoint']);
            } catch (error) {
                // Silently ignore read errors (device may be offline or busy)
            }
            
            try {
                // Read error text from locationDescription in Basic cluster
                await endpoint1.read('genBasic', ['locationDesc']);
            } catch (error) {
                // Silently ignore read errors
            }
        };
        
        // Poll immediately on device announce or message events
        if (type === 'deviceAnnounce' || type === 'message') {
            await pollAttributes();
        }
        
        // Set up periodic polling when Z2M starts OR when device joins network
        // This ensures timer starts even if device joins after Z2M is already running
        if (type === 'start' || type === 'deviceAnnounce') {
            // Use device-specific timer key to support multiple devices
            const timerKey = `acw02PollTimer_${device.ieeeAddr}`;
            
            // Clear any existing timer for this device
            if (globalThis[timerKey]) {
                clearInterval(globalThis[timerKey]);
            }
            
            // Start new polling timer (default: 30 seconds, configurable)
            const pollInterval = (options && options.state_poll_interval) || 30;
            if (pollInterval > 0) {
                globalThis[timerKey] = setInterval(async () => {
                    await pollAttributes();
                }, pollInterval * 1000);
            }
        }
        
        // Clean up timer on stop
        if (type === 'stop') {
            const timerKey = `acw02PollTimer_${device.ieeeAddr}`;
            if (globalThis[timerKey]) {
                clearInterval(globalThis[timerKey]);
                globalThis[timerKey] = null;
            }
        }
    },
};

module.exports = definition;
