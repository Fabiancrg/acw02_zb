| Supported Targets | ESP32-C6 | ESP32-H2 |
| ----------------- |  -------- | -------- |

# ESP32 Zigbee Multi-Sensor Device

This project implements a comprehensive environmental monitoring device using ESP32-C6 with Zigbee connectivity. The device combines multiple sensors and actuators into a single Zigbee end-device with five distinct endpoints.

This project is based on the exmaples provided in the ESP Zigbee SDK :
* [ESP Zigbee SDK Docs](https://docs.espressif.com/projects/esp-zigbee-sdk)
* [ESP Zigbee SDK Repo](https://github.com/espressif/esp-zigbee-sdk)

## Device Features

### 🌐 Zigbee Endpoints Overview

| Endpoint | Device Type | Clusters | Description |
|----------|-------------|----------|-------------|
| **1** | LED Strip Light | On/Off | Addressable LED strip (GPIO 8) with full on/off control |
| **2** | GPIO LED Light | On/Off | Single GPIO LED (GPIO 0) for secondary lighting |
| **3** | Button Sensor | Analog Input | External button (GPIO 12) with multi-action detection |
| **4** | Environmental Sensor | Temperature, Humidity, Pressure | BME280 sensor via I2C (GPIO 6/7) |
| **5** | Rain Gauge | Analog Input | Tipping bucket rain sensor (GPIO 18) with rainfall totals |

### 📋 Detailed Endpoint Descriptions

#### **Endpoint 1: LED Strip Controller** 
- **Hardware**: WS2812B-compatible LED strip on GPIO 8
- **Functionality**: Full on/off control with Zigbee integration
- **Features**: Hardware state tracking, button toggle support
- **Use Case**: Primary lighting control and status indication

#### **Endpoint 2: GPIO LED Controller**
- **Hardware**: Standard LED on GPIO 0  
- **Functionality**: Simple on/off switching
- **Features**: Independent control from LED strip
- **Use Case**: Secondary status indicator or backup lighting

#### **Endpoint 3: Smart Button Interface**
- **Hardware**: Push button on GPIO 12
- **Functionality**: Multi-action detection (single, double, hold, release)
- **Features**: Debounced input, action encoding, press counting
- **Use Case**: User interface for device control and interaction

#### **Endpoint 4: Environmental Monitoring**
- **Hardware**: BME280 sensor via I2C (SDA: GPIO 6, SCL: GPIO 7)
- **Measurements**: 
  - 🌡️ **Temperature**: -40°C to +85°C (±1°C accuracy)
  - 💧 **Humidity**: 0-100% RH (±3% accuracy) 
  - 🌪️ **Pressure**: 300-1100 hPa (±1 hPa accuracy)
- **Features**: Automatic 30-second reporting, Zigbee-standard units
- **Use Case**: Weather monitoring, HVAC automation, air quality tracking

#### **Endpoint 5: Rain Gauge System**
- **Hardware**: Tipping bucket rain gauge on GPIO 18
- **Measurements**: Cumulative rainfall in millimeters (0.36mm per tip)
- **Features**: 
  - Advanced debouncing (200ms + 1000ms bounce settle)
  - Persistent storage (NVS) for total tracking
  - Smart reporting (1mm threshold OR hourly)
  - Network-aware operation (only active when connected)
- **Specifications**: 
  - Maximum rate: 200mm/hour supported
  - Accuracy: ±0.36mm per bucket tip
  - Storage: Non-volatile total persistence across reboots
- **Use Case**: Weather station, irrigation control, flood monitoring

### 🔧 Hardware Configuration

#### **Required Components**
- ESP32-C6 development board
- BME280 environmental sensor module
- WS2812B LED strip (at least 1 LED)
- Standard LED + resistor
- Push button + pull-up resistor  
- Tipping bucket rain gauge with reed switch
- Zigbee coordinator (ESP32-H2 or commercial gateway)

#### **Pin Assignments**
```
GPIO 0  - GPIO LED output
GPIO 6  - I2C SDA (BME280)
GPIO 7  - I2C SCL (BME280) 
GPIO 8  - LED strip data (WS2812B)
GPIO 9  - Built-in button (factory reset)
GPIO 12 - External button input
GPIO 18 - Rain gauge input (reed switch)
```

## 🚀 Quick Start

### Configure the Project
```bash
idf.py set-target esp32c6
idf.py menuconfig
```

### Build and Flash
```bash
# Erase previous data (recommended for first flash)
idf.py -p [PORT] erase-flash

# Build and flash the project
idf.py -p [PORT] flash monitor
```

### Device Operation

#### **Button Controls**
- **Built-in Button (GPIO 9)**:
  - Short press: Toggle LED strip on/off
  - Long press (hold): Factory reset device
- **External Button (GPIO 12)**: 
  - Reports all actions (single, double, hold, release) to Zigbee

#### **Automatic Features**
- Environmental data reported every 30 seconds
- Rain gauge totals stored persistently  
- Smart rainfall reporting (1mm increments or hourly)
- Network connection status monitoring

## 📊 Example Output

### Device Initialization
```
I (403) app_start: Starting scheduler on CPU0
I (408) ESP_ZB_ON_OFF_LIGHT: Initialize Zigbee stack
I (558) ESP_ZB_ON_OFF_LIGHT: Deferred driver initialization successful
I (568) ESP_ZB_ON_OFF_LIGHT: BME280 sensor initialized successfully
I (578) ESP_ZB_ON_OFF_LIGHT: Rain gauge initialized successfully. Current total: 0.00 mm
I (578) ESP_ZB_ON_OFF_LIGHT: Start network steering
```

### Network Connection
```
I (3558) ESP_ZB_ON_OFF_LIGHT: Joined network successfully (Extended PAN ID: 74:4d:bd:ff:fe:63:f7:30, PAN ID: 0x13af, Channel:13, Short Address: 0x7c16)
I (3568) RAIN_GAUGE: Rain gauge enabled - device connected to Zigbee network
```

### Sensor Data Reporting
```
I (30000) ESP_ZB_ON_OFF_LIGHT: 🌡️ Temperature: 22.35°C reported to Zigbee  
I (30010) ESP_ZB_ON_OFF_LIGHT: 💧 Humidity: 45.20% reported to Zigbee
I (30020) ESP_ZB_ON_OFF_LIGHT: 🌪️ Pressure: 1013.25 hPa reported to Zigbee
I (30030) ESP_ZB_ON_OFF_LIGHT: 📡 Temp: 22.4°C
I (30040) ESP_ZB_ON_OFF_LIGHT: 📡 Humidity: 45.2%
I (30050) ESP_ZB_ON_OFF_LIGHT: 📡 Pressure: 1013.3 hPa
```

### Button Interactions  
```
I (45000) ESP_ZB_ON_OFF_LIGHT: 🔘 External button action detected: single
I (45010) ESP_ZB_ON_OFF_LIGHT: ✅ Button action single sent (encoded: 1001.0) - press #1
I (45020) ESP_ZB_ON_OFF_LIGHT: 📡 Button: single (#1)
```

### Rain Gauge Activity
```  
I (60000) RAIN_GAUGE: 🔍 Rain gauge interrupt received on GPIO18 (enabled: YES)
I (60010) RAIN_GAUGE: 🌧️ Rain pulse #1 detected! Total: 0.36 mm (+0.36 mm)
I (60020) RAIN_GAUGE: ✅ Rainfall total 0.36 mm reported to Zigbee
I (60030) ESP_ZB_ON_OFF_LIGHT: 📡 Rain: 0.36 mm
```

## 🏠 Home Assistant Integration

When connected to Zigbee2MQTT or other Zigbee coordinators, the device appears as:

- **2x Switch entities**: LED Strip & GPIO LED control
- **1x Sensor entity**: Button actions with press counter
- **3x Sensor entities**: Temperature, Humidity, Pressure  
- **1x Sensor entity**: Rainfall total with automatic updates

### Device Information
- **Manufacturer**: ESPRESSIF
- **Model**: esp32c6  
- **Firmware**: v5.5.1
- **Supported**: Automatic device detection in Zigbee2MQTT

## 🔧 Troubleshooting

### Common Issues

#### **Rain Gauge Not Detecting**
- Verify GPIO 18 connections and reed switch operation
- Check that device is connected to Zigbee network (rain gauge only active when connected)
- Ensure proper pull-down resistor on rain gauge input

#### **BME280 Not Reading**  
- Check I2C connections (SDA: GPIO 6, SCL: GPIO 7)
- Verify BME280 I2C address (default: 0x76 or 0x77)
- Ensure proper power supply to sensor (3.3V)

#### **Button Not Responding**
- Verify button connections and pull-up resistors
- Check GPIO 12 (external) and GPIO 9 (built-in) functionality
- Ensure proper debouncing in hardware

#### **Zigbee Connection Issues**
- Perform factory reset with long press on built-in button
- Ensure Zigbee coordinator is in pairing mode
- Check channel compatibility between coordinator and device

### 📋 Development Notes

- **ESP-IDF Version**: v5.5.1 recommended
- **Zigbee SDK**: Latest ESP Zigbee SDK required  
- **Memory Usage**: ~2MB flash, ~200KB RAM typical
- **Power Consumption**: ~100mA active, supports deep sleep for battery operation

### 🆘 Support

For technical queries, please open an [issue](https://github.com/espressif/esp-idf/issues) on GitHub. Include:
- Complete serial monitor output
- Hardware configuration details  
- ESP-IDF and SDK versions
- Specific symptoms and reproduction steps

---

**Project**: ESP32 Zigbee Multi-Sensor Device  
**Version**: v1.0  
**Compatible**: ESP32-C6, ESP-IDF v5.5.1+  
**License**: Apache 2.0
