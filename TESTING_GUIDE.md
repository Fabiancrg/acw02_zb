# Quick Test Guide for New Endpoints

## Build and Flash

```bash
cd c:\Devel\Repositories\acw02_zb
idf.py build
idf.py -p COM15 flash monitor
```

## Expected Console Output

During device creation, you should see:
```
[NIGHT] Creating Night Mode switch endpoint 5...
[OK] Night Mode switch endpoint 5 added
[PURIF] Creating Purifier switch endpoint 6...
[OK] Purifier switch endpoint 6 added
[CLEAN] Creating Clean status binary sensor endpoint 7...
[OK] Clean status binary sensor endpoint 7 added
[MUTE] Creating Mute switch endpoint 8...
[OK] Mute switch endpoint 8 added
[INFO] Adding manufacturer info (Espressif, esp32c6)...
[OK] Manufacturer info added to all endpoints
```

## Zigbee2MQTT Configuration

1. Copy `zigbee2mqtt_converter.js` to your Z2M data directory
2. Edit `configuration.yaml`:
```yaml
external_converters:
  - zigbee2mqtt_converter.js
```
3. Restart Zigbee2MQTT
4. Re-pair the device or click "Reconfigure"

## Expected Z2M Device View

You should see 8 endpoints:
```
┌─ Endpoint 1 (Thermostat)
│  ├─ System Mode: off/auto/cool/heat/dry/fan_only
│  ├─ Cooling Setpoint: 16-31°C
│  ├─ Local Temperature: (current room temp)
│  └─ Fan Mode: quiet/low/low-med/medium/med-high/high/auto
│
├─ Endpoint 2 (Eco Mode)
│  └─ eco_mode: ON/OFF
│
├─ Endpoint 3 (Swing)
│  └─ swing_mode: ON/OFF
│
├─ Endpoint 4 (Display)
│  └─ display: ON/OFF
│
├─ Endpoint 5 (Night Mode) ← NEW
│  └─ night_mode: ON/OFF
│
├─ Endpoint 6 (Purifier) ← NEW
│  └─ purifier: ON/OFF
│
├─ Endpoint 7 (Clean Status) ← NEW
│  └─ clean_status: ON/OFF (read-only)
│
└─ Endpoint 8 (Mute) ← NEW
   └─ mute: ON/OFF
```

## Testing Commands (from Z2M)

### Test Night Mode
```
Payload: {"night_mode": "ON"}
Expected: [NIGHT] Mode ON
Expected TX frame byte 15: 0x02 bit set
```

### Test Purifier
```
Payload: {"purifier": "ON"}
Expected: [PURIFIER] ON
Expected TX frame byte 15: 0x40 bit set
```

### Test Mute
```
Payload: {"mute": "ON"}
Expected: [MUTE] ON
Expected TX frame byte 16: 0x01
Expected: AC doesn't beep on next command
```

### Test Clean Status (Read-Only)
```
Action: When AC detects dirty filter, it will send status frame
Expected RX: clean_status changes to ON in Z2M
Expected: Endpoint 7 state updates automatically
```

## Debugging TX Frames

Look for lines like:
```
TX [24 bytes]: 7A 7A 21 D5 18 00 00 A1 00 00 00 00 18 25 00 42 01 00 00 00 00 00 XX XX
                                                                  ^^    ^^
                                                                  |     └─ Byte 16 (mute)
                                                                  └─ Byte 15 (options)
```

**Byte 15 decoding:**
- 0x01 = Eco ON
- 0x02 = Night ON
- 0x40 = Purifier ON
- 0x80 = Display ON
- 0x43 = Eco + Night + Purifier ON

**Byte 16 decoding:**
- 0x00 = Not muted
- 0x01 = Muted

## Debugging RX Frames

Look for lines like:
```
RX [34 bytes]: 7A 7A D5 21 ...
  Options: Eco=ON, Night=OFF, Display=ON, Purifier=OFF, Clean=NO, Swing=OFF
```

## Common Issues

### Issue: Endpoint not appearing in Z2M
**Solution:** 
1. Click "Reconfigure" in Z2M device page
2. Wait 30 seconds
3. Refresh page

### Issue: Clean status never changes
**Solution:**
- Clean status comes FROM the AC, not TO the AC
- It will only show ON when the AC's filter is actually dirty
- This is a read-only status indicator

### Issue: Commands not working
**Solution:**
1. Check console logs for command reception
2. Verify TX frame byte 15 & 16 are correct
3. Check if HVAC is responding (look for RX frames)

### Issue: Night mode doesn't seem to work
**Solution:**
- Night mode only works in specific AC modes (COOL/DRY/HEAT)
- Not available in AUTO or FAN_ONLY modes
- Check AC's actual mode first

## UART Frame Verification

With physical ACW02 connected, send a command and verify:

1. **TX Frame (ESP → AC):**
   ```
   7A 7A 21 D5 18 00 00 A1 00 00 00 00 18 25 00 42 01 00 00 00 00 00 XX XX
   ```

2. **RX Frame (AC → ESP):**
   ```
   7A 7A D5 21 22 00 00 D2 00 00 19 00 00 18 25 00 42 00 00 00 00 00 ... XX XX
   ```

3. **Verify bytes match:**
   - Byte 15 (options) should reflect your switches
   - Byte 16 (mute) should be 0x01 when muted
   - CRC should be valid

## Success Criteria

✅ All 8 endpoints appear in Z2M
✅ Night mode toggles ON/OFF
✅ Purifier toggles ON/OFF  
✅ Mute toggles ON/OFF
✅ Clean status shows correctly (when AC reports it)
✅ No errors in ESP32 console
✅ No errors in Z2M logs
✅ TX frames show correct byte 15 & 16 values
✅ RX frames correctly decoded

## Rollback (if needed)

If you need to revert to 4 endpoints only:
1. Checkout previous git commit before these changes
2. Rebuild and reflash
3. Remove device from Z2M
4. Re-pair device
