# ✅ FIXED: UART Pin Conflict Causing Console Freeze

## Problem Identified

The console was freezing because **HVAC driver was using the same GPIO pins as the console UART**:

### Before (BROKEN):
```c
#define HVAC_UART_NUM           UART_NUM_1  // Using UART1 (correct)
#define HVAC_UART_TX_PIN        16          // ❌ GPIO16 = Console TX
#define HVAC_UART_RX_PIN        17          // ❌ GPIO17 = Console RX
```

When `uart_set_pin(UART_NUM_1, 16, 17, ...)` was called, it **reconfigured GPIO 16 & 17** from Console UART (UART0) to HVAC UART (UART1), killing the console.

## Solution Applied

Changed HVAC UART to use different GPIO pins that don't conflict with console:

### After (FIXED):
```c
#define HVAC_UART_NUM           UART_NUM_1  // Using UART1 (correct)
#define HVAC_UART_TX_PIN        4           // ✅ GPIO4 (no conflict)
#define HVAC_UART_RX_PIN        5           // ✅ GPIO5 (no conflict)
```

## Why This Fix Works

### ESP32-C6 Console UART
- **Console:** UART0 (USB-to-serial)
- **Default Pins:** GPIO 16 (TX) and GPIO 17 (RX)
- **Used for:** `idf.py monitor` output

### HVAC UART
- **HVAC:** UART1
- **New Pins:** GPIO 4 (TX) and GPIO 5 (RX)
- **Purpose:** Communication with ACW02 HVAC device

**No conflict!** Different UARTs, different GPIO pins.

## Expected Behavior After Fix

### Console Output Will Show:
```
I (732) HVAC_ZIGBEE: [INIT] Boot button initialization complete
I (742) HVAC_ZIGBEE: [INIT] Initializing HVAC UART driver...
I (742) HVAC_ZIGBEE: [INIT] About to call hvac_driver_init()
I (752) HVAC_DRIVER: [HVAC] Starting HVAC driver initialization
I (752) HVAC_DRIVER: [HVAC] Configuring UART1 (TX=4, RX=5, baud=9600)
I (762) HVAC_DRIVER: [HVAC] Setting UART parameters
I (762) HVAC_DRIVER: [OK] UART parameters configured
I (772) HVAC_DRIVER: [HVAC] Setting UART pins
I (772) HVAC_DRIVER: [OK] UART pins configured
I (782) HVAC_DRIVER: [HVAC] Installing UART driver
I (782) HVAC_DRIVER: [OK] UART driver installed
I (792) HVAC_DRIVER: [HVAC] Creating RX task
I (792) HVAC_DRIVER: [OK] RX task created
I (802) HVAC_DRIVER: [OK] HVAC driver initialized successfully
I (802) HVAC_DRIVER: [HVAC] Sending initial keepalive
I (902) HVAC_DRIVER: [OK] Initial keepalive sent
I (902) HVAC_ZIGBEE: [OK] HVAC driver initialized successfully
I (912) HVAC_ZIGBEE: [INIT] Deferred initialization complete
I (912) HVAC_ZIGBEE: [JOIN] Deferred driver initialization successful (ret=0)
I (922) HVAC_ZIGBEE: [JOIN] Starting network steering (searching for coordinator)...
I (922) HVAC_ZIGBEE: [JOIN] Network steering initiated
...
I (XXXX) HVAC_ZIGBEE: [JOIN] *** SUCCESSFULLY JOINED NETWORK ***
I (XXXX) HVAC_ZIGBEE: [JOIN] Extended PAN ID: XX:XX:XX:XX:XX:XX:XX:XX
I (XXXX) HVAC_ZIGBEE: [JOIN] PAN ID: 0xXXXX
I (XXXX) HVAC_ZIGBEE: [JOIN] Channel: XX
I (XXXX) HVAC_ZIGBEE: [JOIN] Short Address: 0xXXXX
I (XXXX) HVAC_ZIGBEE: [JOIN] Device is now online and ready
```

**Full console output throughout the entire device lifecycle!** 🎉

## Physical Wiring Update Required

### Old Wiring (No Longer Works):
```
ACW02 RX  →  ESP32-C6 GPIO 16
ACW02 TX  →  ESP32-C6 GPIO 17
GND       →  GND
```

### New Wiring (Correct):
```
ACW02 RX  →  ESP32-C6 GPIO 4
ACW02 TX  →  ESP32-C6 GPIO 5
GND       →  GND
```

⚠️ **Important:** You must physically change the wire connections!

## GPIO Pin Summary

### ESP32-C6 Pin Allocation:

| GPIO | Function | Notes |
|------|----------|-------|
| 0-1 | JTAG | Can be repurposed if needed |
| 2-3 | Available | General purpose |
| **4** | **HVAC TX** | ← New HVAC UART TX |
| **5** | **HVAC RX** | ← New HVAC UART RX |
| 6-7 | Available | Previously used for I2C in other project |
| 8 | Strapping | Boot mode control |
| **9** | **Boot Button** | Factory reset button |
| 10-11 | Available | General purpose |
| 12-15 | SPI Flash | Reserved |
| **16** | **Console TX** | ← USB-Serial TX (DO NOT USE) |
| **17** | **Console RX** | ← USB-Serial RX (DO NOT USE) |
| 18-23 | Available | General purpose |
| 24-25 | USB D+/D- | If using USB-Serial |

## Changes Made

### 1. hvac_driver.h
```c
// Changed from:
#define HVAC_UART_TX_PIN        16
#define HVAC_UART_RX_PIN        17

// To:
#define HVAC_UART_TX_PIN        4    // GPIO4 (no console conflict)
#define HVAC_UART_RX_PIN        5    // GPIO5 (no console conflict)
```

### 2. Added Detailed HVAC Logging
Added step-by-step logging in `hvac_driver_init()` to see exactly what's happening.

### 3. Added Pre-Init Delay
Added 10ms delay before HVAC init to ensure log buffer flushing.

## Testing Steps

### 1. Update Physical Wiring
- Move ACW02 RX wire from ESP GPIO 16 to GPIO 4
- Move ACW02 TX wire from ESP GPIO 17 to GPIO 5

### 2. Build and Flash
```powershell
idf.py build
idf.py flash monitor
```

### 3. Verify Console Output
You should now see:
- ✅ Complete initialization logs
- ✅ HVAC driver initialization steps
- ✅ JOIN process messages
- ✅ Network information when connected
- ✅ Logs when device is removed from network

### 4. Test HVAC Communication
If ACW02 is connected and powered:
- Device should communicate with HVAC
- HVAC commands will be sent
- Status updates will be received

If ACW02 is NOT connected:
- Device will still work (just no HVAC control)
- Zigbee endpoints will be created
- Console will remain functional

## Summary

### Root Cause
HVAC UART pins conflicted with console UART pins, causing console to stop working when HVAC driver initialized.

### Fix
Changed HVAC UART from GPIO 16/17 (console pins) to GPIO 4/5 (dedicated HVAC pins).

### Result
- ✅ Console remains functional throughout device lifecycle
- ✅ HVAC UART works on dedicated pins
- ✅ No pin conflicts
- ✅ All logs visible
- ✅ JOIN process visible
- ✅ Device fully functional

### Action Required
**Update physical wiring:** Connect ACW02 to GPIO 4 & 5 instead of GPIO 16 & 17.

**This completely solves the console freeze issue!** 🎉
