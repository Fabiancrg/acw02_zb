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

// Custom converters for named switches
const tzLocal = {
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
};

const fzLocal = {
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
};

const definition = {
    zigbeeModel: ['acw02-z'],
    model: 'ACW02-HVAC-ZB',
    vendor: 'ESPRESSIF',
    description: 'ACW02 HVAC Thermostat Controller via Zigbee',
    
    // Supported features
    fromZigbee: [
        fz.thermostat,
        fz.fan,
        fzLocal.eco_mode,
        fzLocal.swing_mode,
        fzLocal.display,
    ],
    toZigbee: [
        tz.thermostat_local_temperature,
        tz.thermostat_occupied_heating_setpoint,
        tz.thermostat_occupied_cooling_setpoint,
        tz.thermostat_system_mode,
        tz.fan_mode,
        tzLocal.eco_mode,
        tzLocal.swing_mode,
        tzLocal.display,
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
        exposes.enum('fan_mode', exposes.access.ALL, ['off', 'low', 'medium', 'high', 'on', 'auto', 'smart'])
            .withDescription('Fan speed: off=Off, low=Quiet(P20), medium=Low-Med(P40)/Medium(P60), high=Med-High(P80)/High(P100), auto=Auto, smart=Turbo')
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
    ],
    
    // Map endpoints with descriptive names
    endpoint: (device) => {
        return {
            'ep1': 1,        // Main thermostat
            'ep2': 2,        // Eco mode switch
            'ep3': 3,        // Swing switch
            'ep4': 4,        // Display switch
            'eco_mode': 2,   // Alternative name for eco mode
            'swing_mode': 3, // Alternative name for swing mode
            'display': 4,    // Alternative name for display
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
        await reporting.thermostatTemperature(endpoint1);
        await reporting.thermostatSystemMode(endpoint1);
        await reporting.thermostatOccupiedHeatingSetpoint(endpoint1);
        await reporting.thermostatOccupiedCoolingSetpoint(endpoint1);
        
        // Configure reporting for fan mode
        await endpoint1.configureReporting('hvacFanCtrl', [{
            attribute: 'fanMode',
            minimumReportInterval: 0,
            maximumReportInterval: 3600,
            reportableChange: 1,
        }]);
        
        // Bind and configure endpoint 2 (Eco mode)
        await reporting.bind(endpoint2, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint2);
        
        // Bind and configure endpoint 3 (Swing)
        await reporting.bind(endpoint3, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint3);
        
        // Bind and configure endpoint 4 (Display)
        await reporting.bind(endpoint4, coordinatorEndpoint, ['genOnOff']);
        await reporting.onOff(endpoint4);
        
        logger.info('ACW02 HVAC Thermostat with switches configured successfully');
    },
};

module.exports = definition;
