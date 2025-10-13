# 🚀 Build & Test Checklist

## Pre-Build Checklist

- [ ] ESP-IDF version 5.5.1 or later is installed
- [ ] ESP32-C6 board is available
- [ ] HVAC unit with UART interface is available
- [ ] UART wiring prepared (TX/RX crossover)
- [ ] Zigbee coordinator is ready (Zigbee2MQTT, ZHA, etc.)

## Build Process

### Step 1: Navigate to Project
```powershell
cd c:\Devel\Repositories\acw02_zb
```

### Step 2: Clean Previous Build (if any)
```powershell
idf.py fullclean
```

### Step 3: Set Target
```powershell
idf.py set-target esp32c6
```
- [ ] Command completes successfully
- [ ] Target set to ESP32-C6

### Step 4: Build Project
```powershell
idf.py build
```
- [ ] Build completes without errors
- [ ] All source files compile
- [ ] hvac_driver.c compiles
- [ ] esp_zb_hvac.c compiles
- [ ] Firmware binary created

### Step 5: Check for Warnings
Review build output for any warnings:
- [ ] No critical warnings
- [ ] No deprecated API usage warnings
- [ ] No undefined reference errors

## Flash & Monitor

### Step 1: Connect ESP32-C6
- [ ] USB cable connected
- [ ] Device recognized (check Device Manager)
- [ ] COM port identified (e.g., COM3, COM4)

### Step 2: Flash Firmware
```powershell
idf.py -p COMx flash
```
Replace `COMx` with your actual port (e.g., COM3)

- [ ] Flash process starts
- [ ] Bootloader flashed
- [ ] Partition table flashed
- [ ] Application flashed
- [ ] Flash verification passes

### Step 3: Monitor Serial Output
```powershell
idf.py -p COMx monitor
```
- [ ] Serial monitor starts
- [ ] Boot messages appear
- [ ] No boot errors
- [ ] HVAC driver initialization message
- [ ] Zigbee stack initialization message

## Zigbee Network Joining

### Step 1: Put Coordinator in Pairing Mode
- [ ] Coordinator ready to accept new devices
- [ ] Pairing mode active (check coordinator interface)

### Step 2: Monitor Joining Process
Look for these log messages:
```
I (xxxxx) HVAC_ZIGBEE: Initialize Zigbee stack
I (xxxxx) HVAC_ZIGBEE: Start network steering
I (xxxxx) HVAC_ZIGBEE: Joined network successfully
```

- [ ] Device starts network steering
- [ ] Device joins network
- [ ] Extended PAN ID logged
- [ ] Short address assigned
- [ ] No steering failures

### Step 3: Verify in Coordinator
- [ ] New device appears in coordinator
- [ ] Device type is "Thermostat"
- [ ] Endpoint 1 is visible
- [ ] Clusters are recognized

## UART Communication Test

### Step 1: Monitor UART Traffic
Look for these in serial log:
```
I (xxxxx) HVAC_DRIVER: TX [12 bytes]: 7A 7A ...
I (xxxxx) HVAC_DRIVER: RX [13 bytes]: Valid frame received
```

- [ ] Keepalive frames sent
- [ ] Status request frames sent
- [ ] Response frames received (if HVAC connected)
- [ ] CRC validation passes
- [ ] No UART errors

### Step 2: Connect HVAC Unit
Wire connections:
- ESP32 GPIO 16 (TX) → HVAC RX
- ESP32 GPIO 17 (RX) → HVAC TX
- Common ground

- [ ] HVAC unit powered on
- [ ] UART wiring confirmed
- [ ] Frames appear in both directions

## Zigbee Control Test

### Test 1: System Mode Control
From coordinator, set system mode:

**Cool Mode:**
- [ ] Send system_mode = 3 (Cool)
- [ ] Check log: "System mode changed to 3"
- [ ] HVAC unit responds
- [ ] Cooling starts

**Heat Mode:**
- [ ] Send system_mode = 4 (Heat)
- [ ] Check log: "System mode changed to 4"
- [ ] HVAC switches to heat

**Off:**
- [ ] Send system_mode = 0 (Off)
- [ ] Check log: "Power off"
- [ ] HVAC turns off

### Test 2: Temperature Control
**Set Temperature to 24°C:**
- [ ] Send occupied_cooling_setpoint = 2400
- [ ] Check log: "Temperature setpoint changed to 24°C"
- [ ] HVAC adjusts target temperature

**Try Different Temperatures:**
- [ ] 20°C (2000): Works
- [ ] 28°C (2800): Works
- [ ] 16°C (1600): Min limit
- [ ] 31°C (3100): Max limit

### Test 3: Fan Control
- [ ] Set fan_mode = 0 (Auto)
- [ ] Set fan_mode = 1 (Low)
- [ ] Set fan_mode = 2 (Medium)
- [ ] Set fan_mode = 3 (High)
- [ ] Check each change in logs

### Test 4: Eco Mode
Prerequisites: System mode must be Cool
- [ ] Enable eco mode (attr 0xF000 = true)
- [ ] Check log: "Eco mode changed to ON"
- [ ] HVAC enters eco mode
- [ ] Fan forced to Auto
- [ ] Disable eco mode (attr 0xF000 = false)
- [ ] Check log: "Eco mode changed to OFF"

### Test 5: Swing Control
- [ ] Enable swing (attr 0xF001 = true)
- [ ] Check log: "Swing mode changed to ON"
- [ ] HVAC swing activates
- [ ] Disable swing
- [ ] HVAC swing stops

### Test 6: Display Control
- [ ] Turn off display (attr 0xF002 = false)
- [ ] HVAC display dims/turns off
- [ ] Turn on display
- [ ] HVAC display returns

## State Synchronization Test

### Test 1: Read Current State
- [ ] Read local_temperature attribute
- [ ] Read system_mode attribute
- [ ] Read cooling_setpoint attribute
- [ ] All values match HVAC state

### Test 2: Periodic Updates
Wait 30 seconds:
- [ ] Log shows periodic status request
- [ ] Attributes update automatically
- [ ] Temperature refreshes

### Test 3: Power Cycle Test
- [ ] Power off ESP32-C6
- [ ] Wait 10 seconds
- [ ] Power on ESP32-C6
- [ ] Device rejoins network automatically
- [ ] Previous settings restored

## Error Handling Test

### Test 1: HVAC Disconnected
- [ ] Disconnect HVAC UART
- [ ] Commands still logged
- [ ] No crashes
- [ ] Reconnect works when HVAC back online

### Test 2: Invalid Commands
- [ ] Try eco mode in Heat mode → Should fail/warn
- [ ] Try temperature out of range → Clamped
- [ ] Check error_text attribute for messages

### Test 3: Network Loss
- [ ] Power off coordinator
- [ ] ESP32 continues running
- [ ] Power on coordinator
- [ ] ESP32 reconnects

## Integration Test (Full Workflow)

### Scenario: Cool Room to 22°C with Eco
1. [ ] Set system_mode = Cool (3)
2. [ ] Set temperature = 2200 (22°C)
3. [ ] Enable eco_mode
4. [ ] Enable swing
5. [ ] Verify all settings applied
6. [ ] Check HVAC is cooling at 22°C with eco and swing

### Scenario: Switch to Heat Mode
1. [ ] Set system_mode = Heat (4)
2. [ ] Set temperature = 2400 (24°C)
3. [ ] Verify eco disabled (only works in Cool)
4. [ ] Check HVAC is heating to 24°C

### Scenario: Turn Off
1. [ ] Set system_mode = Off (0)
2. [ ] Verify HVAC powers down
3. [ ] Check all options reset

## Performance Test

- [ ] Monitor memory usage: `idf.py monitor` → check free heap
- [ ] Check stack usage: No stack overflow warnings
- [ ] UART buffer: No overflow messages
- [ ] Zigbee queue: No dropped messages
- [ ] Run continuously for 1 hour: No crashes
- [ ] Run continuously for 24 hours: No memory leaks

## Documentation Verification

- [ ] README_HVAC.md is accurate
- [ ] MIGRATION.md matches actual changes
- [ ] ATTRIBUTES.md lists all attributes correctly
- [ ] Pin numbers in docs match code
- [ ] Examples work as described

## Final Sign-Off

### Build Quality
- [ ] Code compiles cleanly
- [ ] No warnings in production build
- [ ] Binary size reasonable (<1MB)

### Functionality
- [ ] All HVAC modes work
- [ ] Temperature control accurate
- [ ] Fan control functional
- [ ] Custom features work (eco, swing, display)

### Reliability
- [ ] No crashes during testing
- [ ] Handles errors gracefully
- [ ] Recovers from power loss
- [ ] Network rejoin works

### Integration
- [ ] Works with coordinator
- [ ] Visible in Zigbee network
- [ ] Controls respond immediately
- [ ] Status updates reliably

### Documentation
- [ ] Installation steps clear
- [ ] Troubleshooting helps
- [ ] API reference complete
- [ ] Examples work

---

## 🎉 Testing Complete!

If all items checked:
✅ **Project is ready for production use**

If issues found:
1. Document the issue
2. Check relevant section of README_HVAC.md
3. Review serial logs
4. Consult MIGRATION.md for context

## 📝 Notes Section

Use this space to record observations during testing:

**Build Notes:**
- Build time: ___________
- Binary size: ___________
- Warnings: ___________

**Functionality Notes:**
- HVAC model: ___________
- Response time: ___________
- Issues found: ___________

**Performance Notes:**
- Free heap: ___________
- CPU usage: ___________
- Stability: ___________

**Integration Notes:**
- Coordinator: ___________
- Network type: ___________
- Other devices: ___________

---

**Tested By:** ___________  
**Date:** ___________  
**Status:** ⬜ Pass ⬜ Fail ⬜ Partial
