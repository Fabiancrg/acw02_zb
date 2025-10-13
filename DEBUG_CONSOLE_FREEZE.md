# Debugging: Console Output Stops After Button Init

## Current Situation

### What's Working ✅
- Boot button initialization **completes successfully**
- All button init steps show in logs:
  ```
  I (692) HVAC_ZIGBEE: [INIT] Initializing boot button on GPIO9
  I (702) HVAC_ZIGBEE: [OK] GPIO configured
  I (702) HVAC_ZIGBEE: [OK] Event queue created
  I (712) HVAC_ZIGBEE: [OK] ISR service ready
  I (712) HVAC_ZIGBEE: [OK] ISR handler added
  I (722) HVAC_ZIGBEE: [BUTTON] Task started - waiting for button events
  I (722) HVAC_ZIGBEE: [OK] Button task created
  I (732) HVAC_ZIGBEE: [OK] Boot button initialization complete
  ```

### The Problem ❌
Last message shows:
```
I (732) HVAC_ZIGBEE: [INIT] Boot button initiali�
```

**Observations:**
1. Message is **truncated** - should say "Boot button initialization complete"
2. Has **corrupted character** at end (`�`)
3. **No further logs** appear after this
4. Device **DOES join** network (coordinator sees it)
5. **No logs when device is removed** from network

## Hypothesis

The console output is **freezing** but the device continues running in the background. This suggests:

1. **UART conflict** - HVAC driver might be interfering with console UART
2. **Log buffer overflow** - Too many logs too quickly
3. **HVAC driver hanging** - `hvac_driver_init()` might be blocking
4. **Task priority issue** - Console task might be starved

## Most Likely Cause: UART Conflict

### UART Pin Assignment
Let me check the UART configuration:

**Console UART:** Typically UART0 on ESP32-C6
**HVAC UART:** Need to verify which UART and pins

If HVAC driver is using the **same UART as console**, this would explain:
- ✅ Console works until HVAC init
- ✅ Console stops when UART driver is installed
- ✅ Device continues running (Zigbee still works)
- ✅ No logs appear anymore (console UART taken over)

### Fix Strategy

Check `hvac_driver.h` for UART configuration:
```c
#define HVAC_UART_NUM       ?  // Should NOT be UART_NUM_0 (console)
#define HVAC_UART_TX_PIN    16
#define HVAC_UART_RX_PIN    17
```

**If HVAC_UART_NUM == UART_NUM_0:** This is the problem!
- Console uses UART_NUM_0
- HVAC driver would take over console
- Solution: Change to UART_NUM_1

## Changes Made for Debugging

### 1. Added Detailed Logging to HVAC Driver

Added step-by-step logging in `hvac_driver_init()`:
```c
ESP_LOGI(TAG, "[HVAC] Starting HVAC driver initialization");
ESP_LOGI(TAG, "[HVAC] Configuring UART%d (TX=%d, RX=%d, baud=%d)", ...);
ESP_LOGI(TAG, "[HVAC] Setting UART parameters");
ESP_LOGI(TAG, "[OK] UART parameters configured");
ESP_LOGI(TAG, "[HVAC] Setting UART pins");
ESP_LOGI(TAG, "[OK] UART pins configured");
ESP_LOGI(TAG, "[HVAC] Installing UART driver");  // ← Likely hangs here
ESP_LOGI(TAG, "[OK] UART driver installed");
ESP_LOGI(TAG, "[HVAC] Creating RX task");
ESP_LOGI(TAG, "[OK] RX task created");
```

### 2. Added Delay Before HVAC Init

Added small delay to ensure log buffer is flushed:
```c
ESP_LOGI(TAG, "[INIT] Boot button initialization complete");
vTaskDelay(pdMS_TO_TICKS(10));  // Flush logs
ESP_LOGI(TAG, "[INIT] Initializing HVAC UART driver...");
ESP_LOGI(TAG, "[INIT] About to call hvac_driver_init()");
```

## Expected New Output

### If UART conflict:
```
I (732) HVAC_ZIGBEE: [INIT] Boot button initialization complete
I (742) HVAC_ZIGBEE: [INIT] Initializing HVAC UART driver...
I (742) HVAC_ZIGBEE: [INIT] About to call hvac_driver_init()
I (752) HVAC_DRIVER: [HVAC] Starting HVAC driver initialization
I (752) HVAC_DRIVER: [HVAC] Configuring UART0 (TX=16, RX=17, baud=9600)
I (762) HVAC_DRIVER: [HVAC] Setting UART parameters
I (762) HVAC_DRIVER: [OK] UART parameters configured
I (772) HVAC_DRIVER: [HVAC] Setting UART pins
I (772) HVAC_DRIVER: [OK] UART pins configured
I (782) HVAC_DRIVER: [HVAC] Installing UART driver
[CONSOLE DIES HERE - No more output]
```

### If not UART conflict:
We should see **which step** HVAC init reaches before hanging.

## Next Steps

### 1. Test Current Changes
Rebuild and check console output:
```powershell
idf.py build
idf.py flash monitor
```

Watch for:
- Does "[INIT] Initializing HVAC UART driver..." appear?
- Does "[INIT] About to call hvac_driver_init()" appear?
- Which HVAC log messages appear before it stops?

### 2. Check UART Configuration
Read `hvac_driver.h` and verify:
```c
#define HVAC_UART_NUM  // If this is 0, THAT'S THE PROBLEM
```

### 3. Fix UART Conflict (if confirmed)
Change HVAC to use UART1:
```c
#define HVAC_UART_NUM       UART_NUM_1  // Use UART1, not UART0
#define HVAC_UART_TX_PIN    16          // GPIO16
#define HVAC_UART_RX_PIN    17          // GPIO17
```

### 4. Alternative: Disable HVAC for Testing
Temporarily comment out HVAC init to confirm device works:
```c
// ret = hvac_driver_init();
ESP_LOGW(TAG, "[WARN] HVAC driver DISABLED for testing");
ret = ESP_OK;
```

If this makes console work and JOIN messages appear → HVAC driver is the issue.

## Technical Notes

### ESP32-C6 UART Pins
- **UART0:** Default console (USB-to-serial)
  - TX: GPIO16 (by default) 
  - RX: GPIO17 (by default)
  
- **UART1:** Available for application
  - Can use any GPIO with IOMUX

**PROBLEM:** HVAC driver might be configured to use GPIO16/17, which are the **same pins as console UART**!

### UART Driver Behavior
When `uart_driver_install()` is called on a UART:
- Takes exclusive control of that UART peripheral
- Reconfigures pins
- If it's the console UART → **console stops working**

## Summary

🔍 **Debugging Goal:** Identify where exactly the freeze occurs

📝 **Added Logging:**
- Detailed step-by-step logs in `hvac_driver_init()`
- Extra log before calling HVAC init
- Delay to ensure log buffer flushing

🎯 **Expected Result:**
- Will see exactly which HVAC init step causes freeze
- If logs stop at "Installing UART driver" → UART conflict confirmed
- If logs stop elsewhere → Different issue

⚡ **Quick Test:**
Comment out `hvac_driver_init()` call. If device then works normally and shows JOIN messages → HVAC driver is definitely the culprit.
