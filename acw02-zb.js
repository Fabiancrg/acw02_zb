/**
 * Zigbee2MQTT External Converter for ACW02 HVAC Thermostat
 * 
 * This file defines the device for Zigbee2MQTT to properly expose all controls.
 * 
 * REPORTING FLAG OPTIMIZATION:
 * - Device uses ESP_ZB_ZCL_ATTR_ACCESS_REPORTING flag for automatic attribute reporting
 * - Temperature, setpoints, system_mode, and all switches now auto-report on changes
 * - Minimal polling only for attributes ESP-Zigbee stack cannot auto-report:
 *   * runningMode (Zigbee stack limitation)
 *   * fanMode (not in standard reportable attributes)
 *   * error_text (locationDesc - not typically reportable)
 * 
 * Installation:
 * 1. Copy this file to your Zigbee2MQTT data directory (e.g., /opt/zigbee2mqtt/data/)
 * 2. Edit your Zigbee2MQTT configuration.yaml and add:
 *    external_converters:
 *      - acw02-zb.js
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
                return {filter_clean_status: state};
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
                return {ac_error_status: state};
            }
        },
    },

    error_text: {
        cluster: 'genBasic',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg, publish, options, meta) => {
            if (msg.endpoint.ID === 1 && msg.data['locationDesc'] !== undefined) {
                meta.logger.debug(`ACW02 fz.error_text: Raw locationDesc data type: ${typeof msg.data['locationDesc']}, value: ${JSON.stringify(msg.data['locationDesc'])}`);
                
                let errorText = '';
                const locationDesc = msg.data['locationDesc'];
                
                if (typeof locationDesc === 'string') {
                    // ESP32 is sending string directly
                    errorText = locationDesc;
                    meta.logger.info(`ACW02 fz.error_text: Received string directly: "${errorText}"`);
                } else if (locationDesc && Array.isArray(locationDesc) && locationDesc.length > 0) {
                    // Zigbee string format: first byte is length, rest is text
                    const textLength = locationDesc[0];
                    const textData = locationDesc.slice(1, 1 + textLength);
                    errorText = String.fromCharCode.apply(null, textData);
                    meta.logger.info(`ACW02 fz.error_text: Decoded from bytes: "${errorText}"`);
                }
                
                // Check if there's an error (non-empty text)
                if (errorText && errorText.trim() !== '') {
                    // Return the actual error text
                    return {error_text: errorText};
                } else {
                    // No error
                    return {error_text: ''};
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
        exposes.binary('ac_error_status', exposes.access.STATE_GET, 'ON', 'OFF').withDescription('Error status indicator (read-only, ON when AC has an error)'),
        e.switch().withEndpoint('eco_mode').withDescription('Eco mode'),
        e.switch().withEndpoint('swing_mode').withDescription('Swing mode'),
        e.switch().withEndpoint('display').withDescription('Display on/off'),
        e.switch().withEndpoint('night_mode').withDescription('Night mode (sleep mode with adjusted settings)'),
        e.switch().withEndpoint('purifier').withDescription('Air purifier/ionizer'),
        exposes.binary('filter_clean_status', exposes.access.STATE_GET, 'ON', 'OFF').withDescription('Filter cleaning status indicator (read-only, cleared by AC unit)'),
        e.switch().withEndpoint('mute').withDescription('Mute beep sounds on AC'),
    ],
    
    // Map endpoints with descriptive names
    endpoint: (device) => {
        return {
            'default': 1,          // Main thermostat
            'eco_mode': 2,         // Eco mode switch
            'swing_mode': 3,       // Swing switch
            'display': 4,          // Display switch
            'night_mode': 5,       // Night mode switch
            'purifier': 6,         // Purifier switch
            'clean_sensor': 7,     // Clean status binary sensor (read-only)
            'mute': 8,             // Mute switch
            'error_sensor': 9,     // Error status binary sensor (read-only)
        };
    },
    
    // Configure reporting - with REPORTING flag, most attributes auto-report!
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
        
        // Configure reporting for thermostat attributes (with REPORTING flag, these auto-report!)
        await reporting.thermostatTemperature(endpoint1);  // localTemp
        await reporting.thermostatOccupiedHeatingSetpoint(endpoint1);  // setpoint
        
        // Configure systemMode reporting (0x001C)
        await endpoint1.configureReporting('hvacThermostat', [{
            attribute: 'systemMode',
            minimumReportInterval: 1,
            maximumReportInterval: 300,
            reportableChange: 1,
        }]);
        
        // Note: runningMode (0x001E) is NOT auto-reportable by ESP-Zigbee stack - we poll it
        
        // Bind and configure on/off switches (endpoints 2-8) - with REPORTING flag, these auto-report!
        await reporting.bind(endpoint2, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint2);  // Eco mode
        
        await reporting.bind(endpoint3, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint3);  // Swing
        
        await reporting.bind(endpoint4, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint4);  // Display
        
        await reporting.bind(endpoint5, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint5);  // Night mode
        
        await reporting.bind(endpoint6, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint6);  // Purifier
        
        await reporting.bind(endpoint7, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint7);  // Clean status (read-only)
        
        await reporting.bind(endpoint8, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint8);  // Mute
        
        await reporting.bind(endpoint9, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint9);  // Error status (read-only)
        
        // Initial read of unreportable attributes (only the ones we need to poll)
        try {
            await endpoint1.read('hvacThermostat', ['runningMode']);  // Not auto-reportable
            await endpoint1.read('genBasic', ['locationDesc']);  // Error text
            await endpoint1.read('hvacFanCtrl', ['fanMode']);  // Not auto-reportable
        } catch (error) {
            logger.warn(`ACW02 configure: Initial read failed: ${error.message}`);
        }
    },
    
    // Minimal polling for truly unreportable attributes only (with REPORTING flag, most attributes now auto-report!)
    extend: [
        m.poll({
            key: "acw02_state",
            option: e
                .numeric("acw02_poll_interval", ea.SET)
                .withValueMin(-1)
                .withDescription(
                    "ACW02 HVAC minimal polling for attributes that ESP-Zigbee stack cannot auto-report (runningMode, fanMode, errorText). Default is 60 seconds. Most attributes now auto-report via REPORTING flag! Set to -1 to disable."
                ),
            defaultIntervalSeconds: 60,  // Can be increased since most things now auto-report
            poll: async (device) => {
                const endpoint1 = device.getEndpoint(1);
                if (!endpoint1) {
                    console.warn(`ACW02 polling: endpoint 1 not found`);
                    return;
                }
                
                // Poll ONLY the truly unreportable attributes
                try {
                    // runningMode - ESP-Zigbee stack limitation, cannot auto-report
                    await endpoint1.read('hvacThermostat', ['runningMode']);
                } catch (error) {
                    console.error(`ACW02 polling: runningMode read failed: ${error.message}`);
                }
                
                try {
                    // fanMode - Not in standard reportable attributes
                    await endpoint1.read('hvacFanCtrl', ['fanMode']);
                } catch (error) {
                    console.error(`ACW02 polling: fanMode read failed: ${error.message}`);
                }
                
                try {
                    // error_text (locationDesc) - Not typically reportable
                    await endpoint1.read('genBasic', ['locationDesc']);
                } catch (error) {
                    console.error(`ACW02 polling: error_text read failed: ${error.message}`);
                }
                
                // Note: All other attributes (temperature, setpoints, system_mode, all switches)
                // now auto-report via REPORTING flag - no polling needed!
            },
        }),
    ],
    ota: true,
};

module.exports = definition;
