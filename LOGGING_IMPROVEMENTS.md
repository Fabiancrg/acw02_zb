# Enhanced JOIN Process Logging & Fixed Character Encoding

## Changes Made

### 1. Fixed PowerShell Character Encoding Issue

**Problem:** Emojis in log messages appeared as corrupted characters in Windows PowerShell:
```
I (683) HVAC_ZIGBEE: ✅ Boot button ini�
```

**Solution:** Replaced all emoji characters with ASCII text tags:

| Old Emoji | New Tag | Meaning |
|-----------|---------|---------|
| 🚀 | `[START]` | Starting/launching |
| ✅ | `[OK]` | Success/completed |
| ➕ | `[+]` | Adding/creating |
| 🌡️ | `[HVAC]` | HVAC related |
| 🌿 | `[ECO]` | Eco mode |
| 🌀 | `[SWING]` | Swing mode |
| 📺 | `[DISP]` | Display control |
| 🔌 | `[EP]` | Endpoint |
| 🏭 | `[INFO]` | Information |
| 📝 | `[REG]` | Registration |
| 🔧 | `[INIT]` | Initialization |
| 📻 | `[CFG]` | Configuration |
| 🔘 | `[BUTTON]` | Button events |
| 🔄 | `[RESET]` | Factory reset |

### 2. Enhanced JOIN Process Logging

Added detailed logging for every step of the Zigbee network joining process.

## Expected Log Output

### Startup & Initialization
```
I (XXX) HVAC_ZIGBEE: [START] Starting Zigbee task...
I (XXX) HVAC_ZIGBEE: [INIT] Initializing Zigbee stack as End Device...
I (XXX) HVAC_ZIGBEE: [OK] Zigbee stack initialized
I (XXX) HVAC_ZIGBEE: [INIT] Creating endpoint list...
I (XXX) HVAC_ZIGBEE: [HVAC] Creating HVAC thermostat clusters...
I (XXX) HVAC_ZIGBEE: [+] Adding Basic cluster (0x0000)...
I (XXX) HVAC_ZIGBEE: [OK] Basic cluster added with extended attributes
I (XXX) HVAC_ZIGBEE: [+] Adding Thermostat cluster (0x0201)...
I (XXX) HVAC_ZIGBEE: [OK] Thermostat cluster added
I (XXX) HVAC_ZIGBEE: [+] Adding Fan Control cluster (0x0202)...
I (XXX) HVAC_ZIGBEE: [OK] Fan Control cluster added
I (XXX) HVAC_ZIGBEE: [+] Adding Identify cluster (0x0003)...
I (XXX) HVAC_ZIGBEE: [OK] Identify cluster added
I (XXX) HVAC_ZIGBEE: [EP] Creating HVAC endpoint 1 (Profile: 0x0104, Device: 0x0301)...
I (XXX) HVAC_ZIGBEE: [OK] Endpoint 1 added to endpoint list
I (XXX) HVAC_ZIGBEE: [ECO] Creating Eco Mode switch endpoint 2...
I (XXX) HVAC_ZIGBEE: [OK] Eco Mode switch endpoint 2 added
I (XXX) HVAC_ZIGBEE: [SWING] Creating Swing switch endpoint 3...
I (XXX) HVAC_ZIGBEE: [OK] Swing switch endpoint 3 added
I (XXX) HVAC_ZIGBEE: [DISP] Creating Display switch endpoint 4...
I (XXX) HVAC_ZIGBEE: [OK] Display switch endpoint 4 added
I (XXX) HVAC_ZIGBEE: [INFO] Adding manufacturer info (Espressif, esp32c6)...
I (XXX) HVAC_ZIGBEE: [OK] Manufacturer info added to all endpoints
I (XXX) HVAC_ZIGBEE: [REG] Registering Zigbee device...
I (XXX) HVAC_ZIGBEE: [OK] Device registered
I (XXX) HVAC_ZIGBEE: [REG] Registering action handler...
I (XXX) HVAC_ZIGBEE: [OK] Action handler registered
I (XXX) HVAC_ZIGBEE: [CFG] Setting Zigbee channel mask: 0x07FFF800
I (XXX) HVAC_ZIGBEE: [START] Starting Zigbee stack...
I (XXX) HVAC_ZIGBEE: [OK] Zigbee stack started successfully
```

### First Device Start (Factory New)
```
I (XXX) HVAC_ZIGBEE: [JOIN] Initialize Zigbee stack
I (XXX) HVAC_ZIGBEE: [JOIN] Device first start - factory new device
I (XXX) HVAC_ZIGBEE: [JOIN] Deferred driver initialization successful
I (XXX) HVAC_ZIGBEE: [INIT] Starting deferred driver initialization...
I (XXX) HVAC_ZIGBEE: [BUTTON] Monitoring started on GPIO9
I (XXX) HVAC_ZIGBEE: [BUTTON] Press and hold for 5 seconds to factory reset
I (XXX) HVAC_ZIGBEE: [OK] Boot button initialized on GPIO9
I (XXX) HVAC_ZIGBEE: [INIT] Initializing HVAC UART driver...
I (XXX) HVAC_DRIVER: Initializing HVAC UART on GPIO16 (TX) and GPIO17 (RX)
I (XXX) HVAC_ZIGBEE: [OK] HVAC driver initialized successfully
I (XXX) HVAC_ZIGBEE: [JOIN] Starting network steering (searching for coordinator)...
```

### Network Steering (Searching for Network)
```
I (XXX) HVAC_ZIGBEE: [JOIN] *** SUCCESSFULLY JOINED NETWORK ***
I (XXX) HVAC_ZIGBEE: [JOIN] Extended PAN ID: 01:23:45:67:89:AB:CD:EF
I (XXX) HVAC_ZIGBEE: [JOIN] PAN ID: 0x1A2B
I (XXX) HVAC_ZIGBEE: [JOIN] Channel: 15
I (XXX) HVAC_ZIGBEE: [JOIN] Short Address: 0x1D92
I (XXX) HVAC_ZIGBEE: [JOIN] IEEE Address: 7c:2c:67:ff:fe:42:d2:d4
I (XXX) HVAC_ZIGBEE: [JOIN] Device is now online and ready
```

### Device Reboot (Previously Joined)
```
I (XXX) HVAC_ZIGBEE: [JOIN] Device reboot - previously joined network
I (XXX) HVAC_ZIGBEE: [JOIN] Deferred driver initialization successful
I (XXX) HVAC_ZIGBEE: [JOIN] Rejoining previous network...
I (XXX) HVAC_ZIGBEE: [JOIN] IEEE Address: 7c:2c:67:ff:fe:42:d2:d4
```

### Failed JOIN Attempt
```
W (XXX) HVAC_ZIGBEE: [JOIN] Network steering failed (status: ESP_ERR_TIMEOUT)
I (XXX) HVAC_ZIGBEE: [JOIN] Retrying network steering in 1 second...
```

### Button Events
```
I (XXX) HVAC_ZIGBEE: [BUTTON] Boot button pressed
I (XXX) HVAC_ZIGBEE: [BUTTON] Released (held for 1234 ms)
```

### Factory Reset (5 second hold)
```
W (XXX) HVAC_ZIGBEE: [BUTTON] Long press detected! Triggering factory reset...
W (XXX) HVAC_ZIGBEE: [RESET] Performing factory reset...
I (XXX) HVAC_ZIGBEE: [RESET] Factory reset successful - device will restart
```

### Control Commands
```
I (XXX) HVAC_ZIGBEE: Received message: endpoint(1), cluster(0x201), attribute(0x1c), data size(2)
I (XXX) HVAC_ZIGBEE: System mode changed to 3
I (XXX) HVAC_ZIGBEE: Updated Zigbee attributes: Mode=3, Temp=24°C, Eco=0, Swing=0, Display=1

I (XXX) HVAC_ZIGBEE: Received message: endpoint(2), cluster(0x6), attribute(0x0), data size(1)
I (XXX) HVAC_ZIGBEE: [ECO] Mode ON

I (XXX) HVAC_ZIGBEE: Received message: endpoint(3), cluster(0x6), attribute(0x0), data size(1)
I (XXX) HVAC_ZIGBEE: [SWING] Mode ON

I (XXX) HVAC_ZIGBEE: Received message: endpoint(4), cluster(0x6), attribute(0x0), data size(1)
I (XXX) HVAC_ZIGBEE: [DISPLAY] OFF
```

## Log Categories

| Tag | Purpose | Examples |
|-----|---------|----------|
| `[START]` | Process/task starting | Zigbee task, stack |
| `[INIT]` | Initialization steps | Stack init, driver init, endpoint list |
| `[OK]` | Successful completion | Cluster added, device registered |
| `[JOIN]` | Network joining process | Steering, network info, join status |
| `[EP]` | Endpoint creation | Endpoint configuration |
| `[HVAC]` | HVAC-specific operations | Cluster creation |
| `[ECO]` | Eco mode events | On/off |
| `[SWING]` | Swing mode events | On/off |
| `[DISP]` | Display events | On/off |
| `[INFO]` | General information | Manufacturer info |
| `[REG]` | Registration steps | Device/handler registration |
| `[CFG]` | Configuration | Channel mask |
| `[BUTTON]` | Button events | Press/release/long press |
| `[RESET]` | Factory reset | Reset triggered |
| `[ERROR]` | Errors | HVAC driver failures |
| `[WARN]` | Warnings | No HVAC connected |
| `[ZDO]` | Other Zigbee events | General ZDO signals |

## Troubleshooting with New Logs

### Device Not Joining
Look for:
```
I (XXX) HVAC_ZIGBEE: [JOIN] Starting network steering (searching for coordinator)...
W (XXX) HVAC_ZIGBEE: [JOIN] Network steering failed (status: ESP_ERR_TIMEOUT)
I (XXX) HVAC_ZIGBEE: [JOIN] Retrying network steering in 1 second...
```

**Solutions:**
- Ensure coordinator is in permit join mode
- Check Zigbee channel configuration
- Verify power and antenna connection

### Endpoints Not Created
Look for:
```
I (XXX) HVAC_ZIGBEE: [OK] Endpoint 1 added to endpoint list
I (XXX) HVAC_ZIGBEE: [OK] Eco Mode switch endpoint 2 added
I (XXX) HVAC_ZIGBEE: [OK] Swing switch endpoint 3 added
I (XXX) HVAC_ZIGBEE: [OK] Display switch endpoint 4 added
```

If missing, check for errors in cluster creation steps.

### HVAC Driver Issues
Look for:
```
E (XXX) HVAC_ZIGBEE: [ERROR] Failed to initialize HVAC driver (UART connection issue?)
W (XXX) HVAC_ZIGBEE: [WARN] Continuing without HVAC - endpoints will still be created
```

**Note:** Device will still create Zigbee endpoints even if HVAC is not connected.

### Factory Reset Verification
Look for:
```
W (XXX) HVAC_ZIGBEE: [BUTTON] Long press detected! Triggering factory reset...
W (XXX) HVAC_ZIGBEE: [RESET] Performing factory reset...
I (XXX) HVAC_ZIGBEE: [RESET] Factory reset successful - device will restart
```

Then after restart:
```
I (XXX) HVAC_ZIGBEE: [JOIN] Device first start - factory new device
```

## Benefits

1. ✅ **No more corrupted characters** in PowerShell
2. ✅ **Clear visual tags** make logs easier to scan
3. ✅ **Detailed JOIN process** helps troubleshoot network issues
4. ✅ **Separate device start vs reboot** messages
5. ✅ **Network information logged** (PAN ID, channel, addresses)
6. ✅ **Button press timing** helps verify factory reset
7. ✅ **Category tags** allow log filtering

## Filtering Logs

You can filter logs by category in PowerShell:
```powershell
idf.py monitor | Select-String "\[JOIN\]"
idf.py monitor | Select-String "\[ERROR\]|\[WARN\]"
idf.py monitor | Select-String "\[ECO\]|\[SWING\]|\[DISP\]"
```

Or save to file and search:
```powershell
idf.py monitor > log.txt
Select-String "\[JOIN\]" log.txt
```

## Summary

- ✅ All emojis replaced with ASCII tags
- ✅ PowerShell display fixed
- ✅ Enhanced JOIN process logging
- ✅ Network details logged (PAN ID, channel, addresses)
- ✅ Device first start vs reboot distinguished
- ✅ Retry logging on failed joins
- ✅ Button and factory reset logging
- ✅ Easy log filtering by category

**Build and flash to see the improved logs!**
