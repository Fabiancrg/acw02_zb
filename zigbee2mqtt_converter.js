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

const definition = {
    zigbeeModel: ['esp32c6'],
    model: 'ACW02-HVAC-ZB',
    vendor: 'ESPRESSIF',
    description: 'ACW02 HVAC Thermostat Controller via Zigbee',
    
    // Supported features
    fromZigbee: [
        fz.thermostat,
        fz.fan,
    ],
    toZigbee: [
        tz.thermostat_local_temperature,
        tz.thermostat_occupied_heating_setpoint,
        tz.thermostat_occupied_cooling_setpoint,
        tz.thermostat_system_mode,
        tz.fan_mode,
    ],
    
    // Expose controls in the UI
    exposes: [
        e.climate()
            .withSetpoint('occupied_heating_setpoint', 16, 31, 1)
            .withSetpoint('occupied_cooling_setpoint', 16, 31, 1)
            .withLocalTemperature()
            .withSystemMode(['off', 'auto', 'cool', 'heat', 'dry', 'fan_only'])
            .withRunningState(['idle', 'heat', 'cool', 'fan_only']),
        exposes.enum('fan_mode', exposes.access.ALL, ['off', 'low', 'medium', 'high', 'on', 'auto', 'smart'])
            .withDescription('Fan speed control'),
    ],
    
    // Configure reporting
    configure: async (device, coordinatorEndpoint, logger) => {
        const endpoint = device.getEndpoint(1);
        
        // Bind clusters
        await reporting.bind(endpoint, coordinatorEndpoint, [
            'genBasic',
            'hvacThermostat', 
            'hvacFanCtrl',
        ]);
        
        // Configure reporting for thermostat attributes
        await reporting.thermostatTemperature(endpoint);
        await reporting.thermostatSystemMode(endpoint);
        await reporting.thermostatOccupiedHeatingSetpoint(endpoint);
        await reporting.thermostatOccupiedCoolingSetpoint(endpoint);
        
        // Configure reporting for fan mode
        await endpoint.configureReporting('hvacFanCtrl', [{
            attribute: 'fanMode',
            minimumReportInterval: 0,
            maximumReportInterval: 3600,
            reportableChange: 1,
        }]);
        
        logger.info('ACW02 HVAC Thermostat configured successfully');
    },
};

module.exports = definition;
