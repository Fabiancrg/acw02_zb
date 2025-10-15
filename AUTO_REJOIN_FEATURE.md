# Auto-Rejoin Feature

## Overview

The device now automatically attempts to rejoin the Zigbee network if it's removed from the coordinator.

## How It Works

When the device receives a **network leave** signal (e.g., when removed from Zigbee2MQTT), it:

1. **Logs the event** with details about the leave type
2. **Waits 30 seconds** (configurable delay)
3. **Automatically starts network steering** to search for and rejoin a coordinator

## Signal Handler Implementation

### ESP_ZB_ZDO_SIGNAL_LEAVE

This signal is triggered when:
- Device is removed from Z2M via "Remove Device"
- Coordinator sends a leave command
- Network connection is lost and device leaves

```c
case ESP_ZB_ZDO_SIGNAL_LEAVE:
    ESP_LOGW(TAG, "[LEAVE] *** DEVICE LEFT THE NETWORK ***");
    
    // Get leave parameters
    esp_zb_zdo_signal_leave_params_t *leave_params = 
        (esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
    
    if (leave_params->leave_type == 0) {
        // RESET type - device was removed from coordinator
        ESP_LOGW(TAG, "[LEAVE] Device was removed from the coordinator");
        ESP_LOGW(TAG, "[LEAVE] Waiting 30 seconds before attempting to rejoin...");
        
        // Schedule rejoin after 30 seconds
        esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                              ESP_ZB_BDB_MODE_NETWORK_STEERING, 30000);
    }
```

## Leave Types

The Zigbee stack supports two leave types:

### 1. Leave without Rejoin (Type 0x00)
- **Triggered when:** Device is removed from coordinator
- **Behavior:** Device leaves network, clears association
- **Auto-rejoin:** ✅ YES (after 30 seconds)
- **Use case:** Removing device from Z2M, moving to different network

### 2. Leave with Rejoin (Type 0x01)
- **Triggered when:** Device initiates rejoin procedure
- **Behavior:** Device automatically rejoins the same network
- **Auto-rejoin:** ✅ Automatic (no delay needed)
- **Use case:** Network recovery, coordinator restart

## Console Output

### When Device is Removed from Z2M

```
W (123456) HVAC_ZIGBEE: [LEAVE] *** DEVICE LEFT THE NETWORK ***
W (123456) HVAC_ZIGBEE: [LEAVE] Leave type: RESET (no rejoin)
W (123456) HVAC_ZIGBEE: [LEAVE] Device was removed from the coordinator
W (123456) HVAC_ZIGBEE: [LEAVE] Waiting 30 seconds before attempting to rejoin...
I (123456) HVAC_ZIGBEE: [LEAVE] Auto-rejoin scheduled in 30 seconds
```

### After 30 Seconds

```
I (153456) HVAC_ZIGBEE: [JOIN] Starting network steering (searching for coordinator)...
I (153456) HVAC_ZIGBEE: [JOIN] Network steering initiated
I (154567) HVAC_ZIGBEE: [JOIN] Steering signal received (status: ESP_OK)
I (154567) HVAC_ZIGBEE: [JOIN] *** SUCCESSFULLY JOINED NETWORK ***
I (154567) HVAC_ZIGBEE: [JOIN] Extended PAN ID: xx:xx:xx:xx:xx:xx:xx:xx
I (154567) HVAC_ZIGBEE: [JOIN] PAN ID: 0x1a62
I (154567) HVAC_ZIGBEE: [JOIN] Channel: 11
I (154567) HVAC_ZIGBEE: [JOIN] Short Address: 0x1234
I (154567) HVAC_ZIGBEE: [JOIN] Device is now online and ready
```

## Configuration

### Delay Time

The 30-second delay can be adjusted by changing the timeout parameter:

```c
// Current: 30 seconds (30000 ms)
esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                      ESP_ZB_BDB_MODE_NETWORK_STEERING, 30000);

// Example: 10 seconds
esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                      ESP_ZB_BDB_MODE_NETWORK_STEERING, 10000);
```

**Recommended delays:**
- **30 seconds**: Good for manual removal (gives time to decide if rejoining is wanted)
- **10 seconds**: Good for automatic recovery from network issues
- **60 seconds**: Conservative approach, ensures coordinator is ready

### Disabling Auto-Rejoin

To disable automatic rejoin, comment out the scheduler call:

```c
case ESP_ZB_ZDO_SIGNAL_LEAVE:
    ESP_LOGW(TAG, "[LEAVE] *** DEVICE LEFT THE NETWORK ***");
    ESP_LOGW(TAG, "[LEAVE] Device will NOT automatically rejoin");
    // esp_zb_scheduler_alarm(...);  // Commented out
    break;
```

## Use Cases

### 1. Testing / Development

**Scenario:** You remove the device from Z2M to test pairing

**Behavior:** 
- Device leaves network
- Waits 30 seconds
- Automatically starts searching for coordinator
- Rejoins the same network (if coordinator is in pairing mode)

**Benefit:** No need to manually reset the device or press buttons

### 2. Network Recovery

**Scenario:** Coordinator crashes and restarts

**Behavior:**
- Device detects network loss
- Waits 30 seconds for coordinator to stabilize
- Automatically rejoins the network
- Resumes operation

**Benefit:** Self-healing network, no manual intervention needed

### 3. Moving Between Networks

**Scenario:** You want to move device to a different Zigbee network

**Options:**
- **Option A:** Remove from old coordinator, wait 30s, put new coordinator in pairing mode
- **Option B:** Factory reset device (boot button 5 seconds) to prevent auto-rejoin

### 4. Production Deployment

**Scenario:** Device loses connection in production

**Behavior:**
- Logs leave event
- Waits 30 seconds (network might recover)
- Attempts to rejoin
- If successful, continues operation
- If failed, retries every 1 second (existing retry logic)

**Benefit:** Automatic recovery from temporary network issues

## Comparison with Factory Reset

| Feature | Auto-Rejoin | Factory Reset |
|---------|-------------|---------------|
| Triggered by | Network leave signal | Boot button (5 seconds) |
| Delay | 30 seconds | Immediate |
| Clears settings | ❌ No (keeps network info) | ✅ Yes (full reset) |
| Rejoins network | ✅ Yes (same network) | ✅ Yes (any network) |
| Use case | Automatic recovery | Manual network change |
| User action | None required | Button press required |

## Error Handling

If auto-rejoin fails (e.g., no coordinator found):

```
W (153456) HVAC_ZIGBEE: [JOIN] Network steering failed (status: ESP_FAIL)
I (153456) HVAC_ZIGBEE: [JOIN] Retrying network steering in 1 second...
```

The existing retry logic continues to attempt joining every 1 second until successful.

## Z2M Integration

### Removing Device from Z2M

1. **Via UI:** Devices → Select device → "Remove"
2. **Via MQTT:**
   ```bash
   mosquitto_pub -t "zigbee2mqtt/bridge/request/device/remove" -m '{"id":"Office_HVAC"}'
   ```

**Result:**
- Device receives leave command
- Logs show `[LEAVE] *** DEVICE LEFT THE NETWORK ***`
- After 30 seconds, device starts rejoining
- Put Z2M in pairing mode to accept the device back

### Re-pairing After Removal

1. Remove device from Z2M (device logs leave event)
2. Wait for `[LEAVE] Auto-rejoin scheduled in 30 seconds` message
3. Enable pairing in Z2M: `Permit join (All)`
4. Wait for device to join (within 30-60 seconds)
5. Device appears in Z2M with same IEEE address

**Note:** Z2M will recognize it as the same device (same IEEE address), so old entity names may persist. For clean entity names, also clear Z2M's device database entry.

## Advanced: Custom Leave Handler

For more complex behavior, you can extend the leave handler:

```c
case ESP_ZB_ZDO_SIGNAL_LEAVE:
    ESP_LOGW(TAG, "[LEAVE] Device left the network");
    
    // Custom logic here:
    // - Flash LED pattern
    // - Send status to HVAC unit
    // - Log to external system
    // - Conditional rejoin based on time of day, etc.
    
    bool should_rejoin = check_rejoin_conditions();
    if (should_rejoin) {
        esp_zb_scheduler_alarm(...);
    }
    break;
```

## Testing the Feature

### Test 1: Manual Remove

```bash
# In Z2M logs, watch for:
mosquitto_pub -t "zigbee2mqtt/bridge/request/device/remove" -m '{"id":"Office_HVAC"}'

# In ESP32 console:
# - [LEAVE] *** DEVICE LEFT THE NETWORK ***
# - [LEAVE] Waiting 30 seconds...
# - [JOIN] Starting network steering... (after 30s)
```

### Test 2: Factory Reset Comparison

```bash
# Hold boot button for 5 seconds
# ESP32 console:
# - [RESET] Performing factory reset...
# - [RESET] Factory reset successful - device will restart
# - Device reboots
# - [JOIN] Device first start - factory new device
```

## Troubleshooting

### Device Doesn't Rejoin

**Check:**
1. Is coordinator in pairing mode? (Z2M: Permit join enabled)
2. Is coordinator reachable? (same channel, same PAN ID)
3. Check ESP32 console for error messages
4. Check signal strength (device might be too far)

**Solution:**
- Enable pairing in Z2M before the 30-second delay expires
- Move device closer to coordinator
- Check Zigbee channel settings match

### Device Rejoins Too Quickly

**Problem:** 30 seconds isn't enough time to prepare new network

**Solution:**
- Increase delay to 60 seconds:
  ```c
  esp_zb_scheduler_alarm(..., 60000);
  ```

### Want to Prevent Rejoin

**Solution 1:** Factory reset instead of remove
- Hold boot button 5 seconds
- Device clears all network info
- Won't auto-rejoin

**Solution 2:** Disable auto-rejoin in code
- Comment out the `esp_zb_scheduler_alarm()` call
- Reflash firmware

## Summary

The auto-rejoin feature provides:

✅ **Automatic recovery** from network disruptions  
✅ **Detailed logging** of leave events  
✅ **Configurable delay** before rejoin attempt  
✅ **Self-healing** network behavior  
✅ **No user intervention** required  
✅ **Proper error handling** with retry logic  

This makes the device more robust in production environments while still allowing manual control through factory reset when needed.
