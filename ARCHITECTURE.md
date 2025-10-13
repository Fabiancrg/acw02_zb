# Architecture Diagram

## System Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Zigbee Network                              │
│  ┌──────────────┐         ┌──────────────┐                         │
│  │ Coordinator  │◄───────►│ ESP32-C6     │                         │
│  │ (Z2M/ZHA)    │  Zigbee │ Thermostat   │                         │
│  └──────────────┘         └───────┬──────┘                         │
│         │                         │                                 │
└─────────┼─────────────────────────┼─────────────────────────────────┘
          │                         │
          │                         │ UART (9600 baud)
          │                         │ TX: GPIO 16
          │                         │ RX: GPIO 17
          │                         │
          │                  ┌──────▼──────┐
          │                  │   ACW02     │
          │                  │ HVAC Unit   │
          │                  └─────────────┘
          │
          ▼
    ┌─────────────┐
    │Home Assistant│
    │or Controller │
    └─────────────┘
```

## Software Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                         ESP32-C6 Firmware                          │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
│  ┌──────────────────────────────────────────────────────────────┐ │
│  │                    esp_zb_hvac.c                             │ │
│  │  ┌────────────────────────────────────────────────────────┐  │ │
│  │  │            Zigbee Application Layer                    │  │ │
│  │  │  • Signal handlers (join, steering, etc)              │  │ │
│  │  │  • Attribute handlers (mode, temp, fan, etc)          │  │ │
│  │  │  • Periodic update scheduler                          │  │ │
│  │  └────────────────────────────────────────────────────────┘  │ │
│  │                          │                                    │ │
│  │                          │                                    │ │
│  │  ┌────────────────────────────────────────────────────────┐  │ │
│  │  │         Zigbee Endpoint (Endpoint 1)                   │  │ │
│  │  │  ┌──────────────────────────────────────────────────┐  │  │ │
│  │  │  │  Thermostat Cluster (0x0201)                     │  │  │ │
│  │  │  │    - System Mode        (0x001C)                 │  │  │ │
│  │  │  │    - Local Temperature  (0x0000)                 │  │  │ │
│  │  │  │    - Cooling Setpoint   (0x0011)                 │  │  │ │
│  │  │  │    - Heating Setpoint   (0x0012)                 │  │  │ │
│  │  │  │    - Eco Mode           (0xF000) [Custom]        │  │  │ │
│  │  │  │    - Swing Mode         (0xF001) [Custom]        │  │  │ │
│  │  │  │    - Display On         (0xF002) [Custom]        │  │  │ │
│  │  │  │    - Error Text         (0xF003) [Custom]        │  │  │ │
│  │  │  └──────────────────────────────────────────────────┘  │  │ │
│  │  │  ┌──────────────────────────────────────────────────┐  │  │ │
│  │  │  │  Fan Control Cluster (0x0202)                    │  │  │ │
│  │  │  │    - Fan Mode           (0x0000)                 │  │  │ │
│  │  │  │    - Fan Mode Sequence  (0x0001)                 │  │  │ │
│  │  │  └──────────────────────────────────────────────────┘  │  │ │
│  │  │  ┌──────────────────────────────────────────────────┐  │  │ │
│  │  │  │  Basic Cluster (0x0000)                          │  │  │ │
│  │  │  │  Identify Cluster (0x0003)                       │  │  │ │
│  │  │  └──────────────────────────────────────────────────┘  │  │ │
│  │  └────────────────────────────────────────────────────────┘  │ │
│  └──────────────────────────────────────────────────────────────┘ │
│                                │                                   │
│                                ▼                                   │
│  ┌──────────────────────────────────────────────────────────────┐ │
│  │                    hvac_driver.c                             │ │
│  │  ┌────────────────────────────────────────────────────────┐  │ │
│  │  │              HVAC Control Layer                        │  │ │
│  │  │  • State management (hvac_state_t)                    │  │ │
│  │  │  • Command building (28-byte frames)                  │  │ │
│  │  │  • Temperature encoding (C→F lookup)                  │  │ │
│  │  │  • CRC16 calculation                                  │  │ │
│  │  └────────────────────────────────────────────────────────┘  │ │
│  │                          │                                    │ │
│  │  ┌────────────────────────────────────────────────────────┐  │ │
│  │  │              UART Communication Layer                  │  │ │
│  │  │  • TX task (send frames)                              │  │ │
│  │  │  • RX task (receive & decode)                         │  │ │
│  │  │  • Frame validation (CRC check)                       │  │ │
│  │  │  • Buffer management                                  │  │ │
│  │  └────────────────────────────────────────────────────────┘  │ │
│  └──────────────────────────────────────────────────────────────┘ │
│                                │                                   │
└────────────────────────────────┼───────────────────────────────────┘
                                 │
                          ┌──────▼──────┐
                          │  UART HAL   │
                          │  (ESP-IDF)  │
                          └─────────────┘
```

## Data Flow

### 1. Zigbee Command → HVAC

```
Coordinator                ESP32-C6                    HVAC
    │                          │                         │
    │ Set Temp = 24°C         │                         │
    ├─────────────────────────►│                         │
    │  (ZCL Write: 0x0011)     │                         │
    │                          │                         │
    │                   ┌──────▼──────┐                  │
    │                   │ Attr Handler│                  │
    │                   │ (esp_zb_    │                  │
    │                   │  hvac.c)    │                  │
    │                   └──────┬──────┘                  │
    │                          │ hvac_set_temperature(24)│
    │                   ┌──────▼──────┐                  │
    │                   │HVAC Driver  │                  │
    │                   │Build Frame: │                  │
    │                   │ 7A 7A ... CRC│                 │
    │                   └──────┬──────┘                  │
    │                          │ uart_write_bytes()      │
    │                          ├─────────────────────────►│
    │                          │ [28 bytes]              │
    │                          │                         │
    │                          │◄────────────────────────┤
    │                          │ ACK [13-34 bytes]       │
    │                   ┌──────▼──────┐                  │
    │                   │Decode State │                  │
    │                   │Verify CRC   │                  │
    │                   └──────┬──────┘                  │
    │                          │                         │
    │◄─────────────────────────┤                         │
    │  Report Success          │                         │
    │                          │                         │
```

### 2. Periodic Status Update

```
    ESP32-C6                             HVAC
        │                                 │
   ┌────▼────┐                            │
   │ Timer   │ Every 30 seconds           │
   │ (30s)   │                            │
   └────┬────┘                            │
        │ hvac_periodic_update()          │
        │                                 │
   ┌────▼────────┐                        │
   │Send Status  │                        │
   │Request Frame│                        │
   └────┬────────┘                        │
        ├─────────────────────────────────►│
        │ 7A 7A 21 D5 ... FE 29           │
        │                                 │
        │◄────────────────────────────────┤
        │ Response [13-34 bytes]          │
        │                                 │
   ┌────▼─────────┐                       │
   │Decode:       │                       │
   │ • Power state│                       │
   │ • Mode       │                       │
   │ • Temperature│                       │
   │ • Fan speed  │                       │
   │ • Errors     │                       │
   └────┬─────────┘                       │
        │                                 │
   ┌────▼─────────────┐                   │
   │Update Zigbee     │                   │
   │Attributes        │                   │
   │ • local_temp     │                   │
   │ • system_mode    │                   │
   │ • setpoint       │                   │
   └──────────────────┘                   │
```

### 3. HVAC State to Zigbee

```
    HVAC                ESP32-C6                Coordinator
     │                      │                         │
     │ State Change         │                         │
     │ (e.g., temp reached) │                         │
     ├──────────────────────►│                         │
     │ Status Frame         │                         │
     │                      │                         │
     │               ┌──────▼──────┐                  │
     │               │hvac_decode_ │                  │
     │               │state()      │                  │
     │               │             │                  │
     │               │Update:      │                  │
     │               │ target_temp │                  │
     │               │ ambient_temp│                  │
     │               │ power_on    │                  │
     │               └──────┬──────┘                  │
     │                      │                         │
     │               ┌──────▼──────────────────┐      │
     │               │hvac_update_zigbee_      │      │
     │               │attributes()             │      │
     │               │                         │      │
     │               │ esp_zb_zcl_set_        │      │
     │               │ attribute_val()         │      │
     │               └──────┬──────────────────┘      │
     │                      │ Report Attribute        │
     │                      ├─────────────────────────►│
     │                      │ (ZCL Report: 0x0A)      │
     │                      │                         │
     │                      │◄────────────────────────┤
     │                      │ Default Response        │
     │                      │                         │
```

## Frame Format Details

### Command Frame (28 bytes)
```
Offset  Field           Value       Description
───────────────────────────────────────────────────────────
0-1     Header          7A 7A       Frame start marker
2-3     Protocol        21 D5       Protocol identifier
4       Length          1C          Frame length indicator
5-6     Reserved        00 00       Reserved bytes
7       Command Type    A3          Command frame type
8       Mode            00-04       HVAC mode (00=Auto, 01=Cool, etc)
9       Temperature     20-3F       Encoded temperature
10      Fan Speed       00-0D       Fan speed setting
11      Swing           00-06       Swing position
12      Options         00-FF       Bit flags (eco, display, etc)
13-25   Data            0A...       Additional data bytes
26-27   CRC             XX XX       CRC16 checksum
```

### Response Frame (Variable: 13/18/28/34 bytes)
```
Offset  Field           Description
──────────────────────────────────────────────
0-1     Header          7A 7A
2-7     Protocol Info   Command response
8+      State Data      Power, mode, temp, etc.
N-2     CRC_H           CRC high byte
N-1     CRC_L           CRC low byte
```

## Memory Layout

```
┌────────────────────────────────────┐
│         Flash Memory               │
├────────────────────────────────────┤
│ Bootloader         (~32 KB)        │
│ Partition Table    (~4 KB)         │
│ NVS (Config)       (~24 KB)        │
│ PHY Init           (~4 KB)         │
│ Application        (~500 KB)       │
│   ├─ Zigbee Stack                  │
│   ├─ ESP-IDF Core                  │
│   ├─ HVAC Driver                   │
│   └─ App Logic                     │
└────────────────────────────────────┘

┌────────────────────────────────────┐
│         RAM (SRAM)                 │
├────────────────────────────────────┤
│ Zigbee Stack       (~80 KB)        │
│ FreeRTOS           (~40 KB)        │
│ Application Heap   (~140 KB)       │
│   ├─ UART Buffers  (4 KB)          │
│   ├─ Zigbee Queues (8 KB)          │
│   ├─ Task Stacks   (16 KB)         │
│   └─ Dynamic Alloc (112 KB)        │
└────────────────────────────────────┘

┌────────────────────────────────────┐
│     NVS Storage (Non-Volatile)     │
├────────────────────────────────────┤
│ Zigbee Network Credentials         │
│ Short Address                      │
│ Extended PAN ID                    │
│ Network Key                        │
│ HVAC State (future)                │
└────────────────────────────────────┘
```

## Task Architecture

```
┌──────────────────────────────────────────────────────────┐
│                    FreeRTOS Tasks                        │
├──────────────────────────────────────────────────────────┤
│                                                          │
│  ┌────────────────┐  Priority: 5, Stack: 4096 bytes    │
│  │ Zigbee Main    │  Main Zigbee stack loop             │
│  │ Task           │  Handles all Zigbee events          │
│  └────────┬───────┘                                     │
│           │                                              │
│           ├──► Signal Handler (Network events)          │
│           ├──► Action Handler (Attribute changes)       │
│           └──► Scheduler Alarms (Periodic callbacks)    │
│                                                          │
│  ┌────────────────┐  Priority: 5, Stack: 3072 bytes    │
│  │ HVAC RX Task   │  UART receive processing            │
│  │                │  Frame validation & decoding        │
│  └────────┬───────┘                                     │
│           │                                              │
│           ├──► Read UART buffer                         │
│           ├──► Validate CRC                             │
│           ├──► Decode state                             │
│           └──► Update internal state                    │
│                                                          │
│  ┌────────────────┐  Callbacks from Zigbee scheduler    │
│  │ Scheduled      │  - hvac_periodic_update (30s)       │
│  │ Callbacks      │  - hvac_update_zigbee_attributes    │
│  └────────────────┘                                     │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

## Error Handling Flow

```
┌─────────────────────────────────────────────────────┐
│              Error Detection Points                 │
├─────────────────────────────────────────────────────┤
│                                                     │
│  UART Level:                                        │
│  ┌────────────────────────────────────────┐        │
│  │ • Framing errors                       │        │
│  │ • Buffer overflow                      │        │
│  │ • Read/write failures                  │        │
│  └───────────────┬────────────────────────┘        │
│                  │                                  │
│                  ▼                                  │
│  Protocol Level:                                    │
│  ┌────────────────────────────────────────┐        │
│  │ • Invalid header (not 0x7A 0x7A)       │        │
│  │ • CRC mismatch                         │        │
│  │ • Invalid frame size                   │        │
│  └───────────────┬────────────────────────┘        │
│                  │                                  │
│                  ▼                                  │
│  Application Level:                                 │
│  ┌────────────────────────────────────────┐        │
│  │ • HVAC not responding                  │        │
│  │ • Invalid mode transitions             │        │
│  │ • Temperature out of range             │        │
│  └───────────────┬────────────────────────┘        │
│                  │                                  │
│                  ▼                                  │
│  ┌────────────────────────────────────────┐        │
│  │  Error Response:                       │        │
│  │  • Log error message                   │        │
│  │  • Update error_text attribute         │        │
│  │  • Continue operation (non-fatal)      │        │
│  │  • Retry on next cycle (if applicable) │        │
│  └────────────────────────────────────────┘        │
│                                                     │
└─────────────────────────────────────────────────────┘
```

This architecture provides:
- ✅ Clear separation of concerns
- ✅ Modular design for easy extension
- ✅ Robust error handling
- ✅ Efficient resource usage
- ✅ Zigbee standard compliance
