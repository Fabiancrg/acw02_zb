# Critical Fix: Button Task Crash

## Problem
Logs were freezing at:
```
I (733) HVAC_ZIGBEE: [INIT] Boot button initiali�
```

No JOIN messages appeared even though the device was joining.

## Root Cause
The button_task was **crashing or hanging immediately** after creation, preventing any further log output.

Possible causes:
1. **Stack overflow** - Original stack size (2048) was too small
2. **GPIO not ready** - Task tried to read GPIO before it was stable
3. **Log buffer issues** - Complex format strings causing problems
4. **Character encoding** - Special characters in log messages

## Fixes Applied

### 1. Increased Task Stack Size
**Before:** 2048 bytes
**After:** 4096 bytes

```c
xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);
```

### 2. Added Startup Delay in Task
Added 50ms delay at start of button_task to let GPIO stabilize:
```c
static void button_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(50));  // Let GPIO stabilize
    ESP_LOGI(TAG, "[BUTTON] Monitoring active");
    ...
}
```

### 3. Simplified All Log Messages
Removed format specifiers that might cause issues:

**Before:**
```c
ESP_LOGI(TAG, "[INIT] Configuring GPIO%d for button...", BOOT_BUTTON_GPIO);
ESP_LOGI(TAG, "[BUTTON] Task started on GPIO%d", BOOT_BUTTON_GPIO);
ESP_LOGI(TAG, "[BUTTON] Hold for %d sec to factory reset", BUTTON_LONG_PRESS_TIME_MS / 1000);
```

**After:**
```c
ESP_LOGI(TAG, "[INIT] Configuring GPIO9 for button");
ESP_LOGI(TAG, "[BUTTON] Monitoring active");
ESP_LOGI(TAG, "[BUTTON] Hold 5 sec for reset");
```

### 4. Added Delay After Task Creation
Added 100ms delay in button_init() after creating task:
```c
ESP_LOGI(TAG, "[OK] Task created");
vTaskDelay(pdMS_TO_TICKS(100));  // Let task start
ESP_LOGI(TAG, "[OK] Button init complete");
```

## Expected New Log Output

### Normal Boot:
```
I (XXX) HVAC_ZIGBEE: [INIT] Starting deferred driver initialization...
I (XXX) HVAC_ZIGBEE: [INIT] Initializing boot button...
I (XXX) HVAC_ZIGBEE: [INIT] Configuring GPIO9 for button
I (XXX) HVAC_ZIGBEE: [OK] GPIO ready
I (XXX) HVAC_ZIGBEE: [INIT] Creating button task
I (XXX) HVAC_ZIGBEE: [OK] Task created
I (XXX) HVAC_ZIGBEE: [BUTTON] Monitoring active
I (XXX) HVAC_ZIGBEE: [BUTTON] Hold 5 sec for reset
I (XXX) HVAC_ZIGBEE: [OK] Button init complete
I (XXX) HVAC_ZIGBEE: [INIT] Boot button initialization complete
I (XXX) HVAC_ZIGBEE: [INIT] Initializing HVAC UART driver...
```

### Then Continue to Join:
```
I (XXX) HVAC_ZIGBEE: [INIT] Deferred initialization complete
I (XXX) HVAC_ZIGBEE: [JOIN] Deferred driver initialization successful (ret=0)
I (XXX) HVAC_ZIGBEE: [JOIN] Starting network steering (searching for coordinator)...
I (XXX) HVAC_ZIGBEE: [JOIN] Network steering initiated
```

### When Device Joins:
```
I (XXX) HVAC_ZIGBEE: [JOIN] Steering signal received (status: ESP_OK)
I (XXX) HVAC_ZIGBEE: [JOIN] *** SUCCESSFULLY JOINED NETWORK ***
I (XXX) HVAC_ZIGBEE: [JOIN] Extended PAN ID: XX:XX:XX:XX:XX:XX:XX:XX
I (XXX) HVAC_ZIGBEE: [JOIN] PAN ID: 0xXXXX
I (XXX) HVAC_ZIGBEE: [JOIN] Channel: XX
I (XXX) HVAC_ZIGBEE: [JOIN] Short Address: 0xXXXX
I (XXX) HVAC_ZIGBEE: [JOIN] IEEE Address: XX:XX:XX:XX:XX:XX:XX:XX
I (XXX) HVAC_ZIGBEE: [JOIN] Device is now online and ready
I (XXX) HVAC_ZIGBEE: [JOIN] Scheduling periodic HVAC updates...
I (XXX) HVAC_ZIGBEE: [JOIN] Setup complete!
```

## Why JOIN Messages Were Missing

Even though the device was joining, the **button_task crash** was preventing the console from showing any more output. The Zigbee stack was running in the background, but the serial output was frozen.

After this fix:
- ✅ Button task will start successfully
- ✅ Console output will continue
- ✅ JOIN process will be visible
- ✅ All network details will be logged

## Testing

1. **Build and flash:**
   ```powershell
   idf.py build
   idf.py flash monitor
   ```

2. **Watch for these messages in order:**
   - `[OK] GPIO ready` ← GPIO configured
   - `[OK] Task created` ← Task created successfully
   - `[BUTTON] Monitoring active` ← Task is running!
   - `[OK] Button init complete` ← Init finished
   - `[JOIN] Starting network steering...` ← Network join starting
   - `[JOIN] *** SUCCESSFULLY JOINED NETWORK ***` ← Join complete!

3. **If it still freezes:**
   - Note the LAST message you see
   - The message just BEFORE that is where the crash occurs

## Changes Summary

| Change | Purpose |
|--------|---------|
| Stack 2048 → 4096 | Prevent stack overflow |
| Added 50ms delay in task | Let GPIO stabilize before first read |
| Added 100ms delay after task create | Ensure task starts before continuing |
| Simplified log messages | Avoid format string issues |
| Removed special characters | Fix encoding issues |

## If It Still Crashes

If the issue persists, we can:

1. **Disable button monitoring entirely** for testing:
   ```c
   static esp_err_t deferred_driver_init(void)
   {
       ESP_LOGI(TAG, "[INIT] Starting deferred driver initialization...");
       ESP_LOGW(TAG, "[WARN] Button init SKIPPED for testing");
       // button_init();  // COMMENTED OUT
       ...
   }
   ```

2. **Check heap memory:**
   Add before button_init():
   ```c
   ESP_LOGI(TAG, "[DEBUG] Free heap: %d bytes", esp_get_free_heap_size());
   ```

3. **Use simpler task:**
   Replace button_task with minimal version:
   ```c
   static void button_task(void *arg)
   {
       ESP_LOGI(TAG, "[BUTTON] Task alive");
       while (1) {
           vTaskDelay(pdMS_TO_TICKS(1000));
       }
   }
   ```

## Summary

✅ Increased button task stack size (2048 → 4096)
✅ Added GPIO stabilization delay (50ms)
✅ Added task startup delay (100ms)  
✅ Simplified all log messages
✅ Removed special characters and format specifiers
✅ Should now see JOIN messages when device connects

**This should resolve the freeze and show all JOIN messages!**
