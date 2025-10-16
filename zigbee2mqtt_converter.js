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
const m = require('zigbee-herdsman-converters/lib/modernExtend');
const e = exposes.presets;
const ea = exposes.access;

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
                const result = {fan_mode: fanModeMap[msg.data.fanMode]};
                meta.logger.info(`ACW02 fz.fan_mode: Received fanMode=0x${msg.data.fanMode.toString(16)} -> ${result.fan_mode}`);
                return result;
            }
        },
    },

    // Read-only binary sensor for clean status (endpoint 7)
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

    // Read-only binary sensor for error status (endpoint 9)
    error_status: {
        cluster: 'genOnOff',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 9 && msg.data.hasOwnProperty('onOff')) {
                const state = msg.data['onOff'] === 1 ? 'ON' : 'OFF';
                meta.logger.info(`ACW02 fz.error_status: Error status: ${state}`);
                return {error_status: state};
            }
        },
    },

    error_text: {
        cluster: 'genBasic',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 1 && msg.data['locationDesc'] !== undefined) {
                meta.logger.debug(`ACW02 fz.error_text: Raw locationDesc data type: ${typeof msg.data['locationDesc']}, value: ${JSON.stringify(msg.data['locationDesc'])}`);
                
                // Zigbee string: first byte is length, rest is text
                const errorTextBytes = msg.data['locationDesc'];
                if (errorTextBytes && errorTextBytes.length > 0) {
                    const textLength = errorTextBytes[0];
                    const textData = errorTextBytes.slice(1, 1 + textLength);
                    const errorText = String.fromCharCode.apply(null, textData);
                    meta.logger.info(`ACW02 fz.error_text: Decoded error text: "${errorText}"`);
                    
                    // Check if there's an error (non-empty text)
                    if (errorText && errorText.trim() !== '') {
                        // Check if it's a known error code (0x04 = PC fashion conflict)
                        if (errorText.includes('0x04') || errorText.includes('PC')) {
                            return {error_text: errorText};
                        } else {
                            // Unknown error - show generic message
                            return {error_text: 'Error, check error code on the display'};
                        }
                    } else {
                        // No error
                        return {error_text: ''};
                    }
                }
            }
        },
    },
    // Custom converter for thermostat attributes
    thermostat: {
        cluster: 'hvacThermostat',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            const result = {};
            if (msg.data.hasOwnProperty('localTemp')) {
                result.local_temperature = msg.data.localTemp / 100;
            }
            if (msg.data.hasOwnProperty('runningMode')) {
                // runningMode is an 8-bit enum: 0x00=idle, 0x03=cool, 0x04=heat, 0x07=fan
                const modeMap = {
                    0x00: 'idle',
                    0x03: 'cool',
                    0x04: 'heat',
                    0x07: 'fan_only',
                };
                result.running_state = modeMap[msg.data.runningMode] || 'idle';
                meta.logger.info(`ACW02 fz.thermostat: runningMode=0x${msg.data.runningMode.toString(16)} -> ${result.running_state}`);
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
                result.system_mode = sysModeMap[msg.data.systemMode] || 'off';
                meta.logger.info(`ACW02 fz.thermostat: systemMode=0x${msg.data.systemMode.toString(16)} -> ${result.system_mode}`);
            }
            // Map setpoint to occupied_heating_setpoint (used for both heating and cooling)
            if (msg.data.hasOwnProperty('occupiedHeatingSetpoint')) {
                result.occupied_heating_setpoint = msg.data.occupiedHeatingSetpoint / 100;
            } else if (msg.data.hasOwnProperty('occupiedCoolingSetpoint')) {
                result.occupied_heating_setpoint = msg.data.occupiedCoolingSetpoint / 100;
            }
            
            if (Object.keys(result).length > 0) {
                meta.logger.debug(`ACW02 fz.thermostat: Converted attributes: ${JSON.stringify(result)}`);
            }
            
            return result;
        },
    },
};

const definition = {
    zigbeeModel: ['acw02-z'],
    model: 'ACW02-ZB',
    vendor: 'ESPRESSIF',
    description: 'ACW02 HVAC Thermostat Controller via Zigbee (Router)',
    
    meta: {
        multiEndpoint: true,
    },
    
    // Supported features
    fromZigbee: [
        fzLocal.thermostat,   // Custom thermostat converter
        fzLocal.fan_mode,
        fzLocal.clean_status, // Read-only binary sensor for endpoint 7
        fzLocal.error_status, // Read-only binary sensor for endpoint 9
        fz.on_off,            // Standard on/off for switch endpoints (2,3,4,5,6,8)
        fzLocal.error_text,
    ],
    toZigbee: [
        tz.thermostat_local_temperature,
        tz.thermostat_occupied_heating_setpoint, // Single setpoint used for both heating and cooling
        tz.thermostat_system_mode,
        tzLocal.fan_mode,
        tz.on_off,              // Standard on/off for switch endpoints (2,3,4,5,6,8) - NOT endpoint 7
        // Note: clean_status (endpoint 7) is read-only, no toZigbee converter
    ],
    
    // Expose controls in the UI
    exposes: [
        e.climate()
            .withSetpoint('occupied_heating_setpoint', 16, 31, 1)
            .withLocalTemperature()
            .withSystemMode(['off', 'auto', 'cool', 'heat', 'dry', 'fan_only'])
            .withRunningState(['idle', 'heat', 'cool', 'fan_only']),
        exposes.enum('fan_mode', exposes.access.ALL, ['quiet', 'low', 'low-med', 'medium', 'med-high', 'high', 'auto'])
            .withDescription('Fan speed mapped to ACW02: Quiet=SILENT, Low=P20, Low-Med=P40, Medium=P60, Med-High=P80, High=P100, Auto=AUTO'),
        exposes.text('error_text', exposes.access.STATE_GET)
            .withDescription('Error message text from AC (shows specific message for known errors, generic message for unknown errors, empty when no error, read-only)'),
        exposes.binary('error_status', exposes.access.STATE_GET, 'ON', 'OFF').withEndpoint('error_status').withDescription('Error status indicator (read-only, ON when AC has an error)'),
        e.switch().withEndpoint('eco_mode').withDescription('Eco mode'),
        e.switch().withEndpoint('swing_mode').withDescription('Swing mode'),
        e.switch().withEndpoint('display').withDescription('Display on/off'),
        e.switch().withEndpoint('night_mode').withDescription('Night mode (sleep mode with adjusted settings)'),
        e.switch().withEndpoint('purifier').withDescription('Air purifier/ionizer'),
        exposes.binary('clean_status', exposes.access.STATE_GET, 'ON', 'OFF').withEndpoint('clean_status').withDescription('Filter cleaning status indicator (read-only, cleared by AC unit)'),
        e.switch().withEndpoint('mute').withDescription('Mute beep sounds on AC'),
    ],
    
    // Map endpoints with descriptive names
    endpoint: (device) => {
        return {
            'default': 1,      // Main thermostat
            'eco_mode': 2,     // Eco mode switch
            'swing_mode': 3,   // Swing switch
            'display': 4,      // Display switch
            'night_mode': 5,   // Night mode switch
            'purifier': 6,     // Purifier switch
            'clean_status': 7, // Clean status binary sensor
            'mute': 8,         // Mute switch
            'error_status': 9, // Error status binary sensor
        };
    },
    
    // Configure reporting
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
        await reporting.bind(endpoint5, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint5);
        
        // Bind and configure endpoint 6 (Purifier)
        await reporting.bind(endpoint6, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint6);
        
        // Bind and configure endpoint 7 (Clean status - read-only)
        await reporting.bind(endpoint7, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint7);
        
        // Bind and configure endpoint 8 (Mute)
        await reporting.bind(endpoint8, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint8);
        
        // Bind and configure endpoint 9 (Error status - read-only)
        await reporting.bind(endpoint9, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint9);
        
        // Initial read of unreportable attributes
        try {
            await endpoint1.read('hvacThermostat', ['runningMode', 'systemMode']);
            await endpoint1.read('genBasic', ['locationDesc']);
        } catch (error) {
            // Ignore errors on initial read
        }
    },
    
    // Use modern polling extend instead of onEvent
    extend: [
        m.poll({
            key: "acw02_state",
            option: e
                .numeric("acw02_poll_interval", ea.SET)
                .withValueMin(-1)
                .withDescription(
                    "ACW02 HVAC does not support reporting for some attributes (runningMode, systemMode, fanMode, errorText) so they are polled instead. Default is 60 seconds, set to -1 to disable."
                ),
            defaultIntervalSeconds: 60,
            poll: async (device) => {
                // Use console.log since logger is not available in modern extend poll context
                console.log(`ACW02 POLLING TRIGGERED for ${device.ieeeAddr}`);
                
                const endpoint1 = device.getEndpoint(1);
                if (!endpoint1) {
                    console.warn(`ACW02 polling: endpoint 1 not found`);
                    return;
                }
                
                // Poll unreportable thermostat attributes
                try {
                    console.log(`ACW02 polling: Reading hvacThermostat attributes...`);
                    await endpoint1.read('hvacThermostat', [
                        'runningMode',
                        'systemMode',
                        'occupiedHeatingSetpoint',
                    ]);
                    console.log(`ACW02 polling: hvacThermostat read successful`);
                } catch (error) {
                    console.error(`ACW02 polling: hvacThermostat read failed: ${error.message}`);
                }
                
                // Poll error text from locationDescription
                try {
                    console.log(`ACW02 polling: Reading genBasic locationDesc...`);
                    await endpoint1.read('genBasic', ['locationDesc']);
                    console.log(`ACW02 polling: genBasic read successful`);
                } catch (error) {
                    console.error(`ACW02 polling: genBasic read failed: ${error.message}`);
                }
                
                // Poll fan mode (unreportable attribute)
                try {
                    console.log(`ACW02 polling: Reading hvacFanCtrl fanMode...`);
                    await endpoint1.read('hvacFanCtrl', ['fanMode']);
                    console.log(`ACW02 polling: hvacFanCtrl read successful`);
                } catch (error) {
                    console.error(`ACW02 polling: hvacFanCtrl read failed: ${error.message}`);
                }
                
                console.log(`ACW02 POLLING COMPLETED for ${device.ieeeAddr}`);
            },
        }),
    ],
};

module.exports = definition;
