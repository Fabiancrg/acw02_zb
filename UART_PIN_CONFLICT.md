# 🔴 CRITICAL: UART Pin Conflict Found!

## The Problem

### Current Configuration (WRONG):
```c
#define HVAC_UART_NUM           UART_NUM_1     // ✅ Correct - using UART1
#define HVAC_UART_TX_PIN        16             // ❌ WRONG - Console UART TX!
#define HVAC_UART_RX_PIN        17             // ❌ WRONG - Console UART RX!
```

### Why Console Dies

**ESP32-C6 Default Console UART Pins:**
- Console: UART0 (USB-to-serial)
- TX: GPIO16
- RX: GPIO17

**When HVAC driver calls:**
```c
uart_set_pin(UART_NUM_1, 16, 17, ...);  // Reconfigures GPIO 16 & 17 to UART1
```

This **disconnects GPIO 16/17 from UART0 (console)** and connects them to UART1 (HVAC), causing:
- ✅ Console works until `uart_set_pin()` is called
- ❌ Console stops working immediately after
- ✅ Device continues running (Zigbee works)
- ❌ No more log output

## The Fix

### Use Different GPIO Pins for HVAC

ESP32-C6 has flexible IOMUX - any GPIO can be used for UART. Choose pins that don't conflict with:
- Console UART (GPIO 16/17)
- Zigbee radio
- Button (GPIO 9)

### Recommended HVAC Pins:

**Option 1: GPIO 6 & 7 (previously used for I2C in ZigbeeMultiSensor)**
```c
#define HVAC_UART_TX_PIN        6   // GPIO6
#define HVAC_UART_RX_PIN        7   // GPIO7
```

**Option 2: GPIO 4 & 5**
```c
#define HVAC_UART_TX_PIN        4   // GPIO4
#define HVAC_UART_RX_PIN        5   // GPIO5
```

**Option 3: GPIO 20 & 21**
```c
#define HVAC_UART_TX_PIN        20  // GPIO20
#define HVAC_UART_RX_PIN        21  // GPIO21
```

### ESP32-C6 Available GPIOs

**Total GPIOs:** GPIO 0-30 (some reserved)

**Reserved/Special:**
- GPIO 0-1: JTAG (can be repurposed)
- GPIO 8: Boot mode strapping
- GPIO 9: Boot button (already used)
- GPIO 12-13: SPI Flash
- GPIO 14-15: SPI Flash
- GPIO 16-17: **Console UART (DO NOT USE)**
- GPIO 24-25: USB (if using USB-Serial)

**Safe for UART:**
- GPIO 2-7: General purpose
- GPIO 10-11: General purpose
- GPIO 18-23: General purpose (if not using USB)

## Recommended Fix

### Change to GPIO 4 & 5

This is the safest option that avoids all potential conflicts:

```c
/* UART Configuration */
#define HVAC_UART_NUM           UART_NUM_1
#define HVAC_UART_TX_PIN        4    // Changed from 16
#define HVAC_UART_RX_PIN        5    // Changed from 17
#define HVAC_UART_BAUD_RATE     9600
#define HVAC_UART_BUF_SIZE      1024
```

### Physical Wiring

You'll need to update the physical connections:
- **ACW02 RX** → **ESP32-C6 GPIO 4** (HVAC_UART_TX_PIN)
- **ACW02 TX** → **ESP32-C6 GPIO 5** (HVAC_UART_RX_PIN)
- **GND** → **GND**

## Apply the Fix Now

I'll update the header file with the correct pins.
