# Fix: Duplicate Entities (display vs display_ep4)

## Problem

When using the IR remote to change the display setting, Z2M shows **two separate entities**:
- `display` - Updated automatically by remote
- `display_ep4` - NOT updated by remote

**Z2M log when remote is pressed:**
```
z2m: Received Zigbee message from 'Office_HVAC', type 'attributeReport', cluster 'genOnOff', data '{"onOff":0}' from endpoint 4 with groupID 0
```

When controlling via Z2M UI, both entities update correctly.

## Root Cause

**Duplicate converters:**
1. **Z2M's standard `fz.on_off`** - Creates generic `display` entity (no suffix)
2. **Custom `fzLocal.display`** - Creates `display_ep4` entity (with endpoint filter)

When the remote triggers an attributeReport from endpoint 4:
- Standard `fz.on_off` processes it → Updates `display` ✅
- Custom `fzLocal.display` doesn't get called → `display_ep4` stays stale ❌

## Solution

**Use Z2M's standard on/off converters for all switch endpoints** - they handle multi-endpoint devices automatically when you provide the `endpoint()` mapping function.

### Changes Made

#### 1. Removed Custom fromZigbee Converters (Lines 168-213)

**Deleted:**
```javascript
// ❌ REMOVED: Custom converters creating _ep# suffixes
fzLocal.eco_mode: { ... },
fzLocal.swing_mode: { ... },
fzLocal.display: { ... },
fzLocal.night_mode: { ... },
fzLocal.purifier: { ... },
fzLocal.clean_status: { ... },
fzLocal.mute: { ... },
```

**Why:** These created duplicate entities with `_ep#` suffixes that didn't update from remote events.

#### 2. Removed Custom toZigbee Converters (Lines 42-147)

**Deleted:**
```javascript
// ❌ REMOVED: Custom toZigbee for switches
tzLocal.eco_mode: { ... },
tzLocal.swing_mode: { ... },
tzLocal.display: { ... },
tzLocal.night_mode: { ... },
tzLocal.purifier: { ... },
tzLocal.mute: { ... },
```

**Why:** Z2M's standard `tz.on_off` already handles multi-endpoint on/off commands.

#### 3. Updated fromZigbee/toZigbee Arrays

**Before:**
```javascript
fromZigbee: [
    fzLocal.thermostat_ep1,
    fzLocal.fan_mode,
    fzLocal.eco_mode,      // ❌ Custom
    fzLocal.swing_mode,    // ❌ Custom
    fzLocal.display,       // ❌ Custom
    fzLocal.night_mode,    // ❌ Custom
    fzLocal.purifier,      // ❌ Custom
    fzLocal.clean_status,  // ❌ Custom
    fzLocal.mute,          // ❌ Custom
    fzLocal.error_text,
],
toZigbee: [
    tz.thermostat_local_temperature,
    tz.thermostat_occupied_heating_setpoint,
    tz.thermostat_occupied_cooling_setpoint,
    tz.thermostat_system_mode,
    tzLocal.fan_mode,
    tzLocal.eco_mode,      // ❌ Custom
    tzLocal.swing_mode,    // ❌ Custom
    tzLocal.display,       // ❌ Custom
    tzLocal.night_mode,    // ❌ Custom
    tzLocal.purifier,      // ❌ Custom
    tzLocal.mute,          // ❌ Custom
],
```

**After:**
```javascript
fromZigbee: [
    fzLocal.thermostat_ep1,  // Custom (for _ep1 suffix on climate)
    fzLocal.fan_mode,         // Custom (for fan speed mapping)
    fz.on_off,                // ✅ Standard (handles all switch endpoints)
    fzLocal.error_text,       // Custom (for error text parsing)
],
toZigbee: [
    tz.thermostat_local_temperature,
    tz.thermostat_occupied_heating_setpoint,
    tz.thermostat_occupied_cooling_setpoint,
    tz.thermostat_system_mode,
    tzLocal.fan_mode,         // Custom (for fan speed mapping)
    tz.on_off,                // ✅ Standard (handles all switch endpoints)
],
```

#### 4. Kept Endpoint Mapping (Essential!)

```javascript
endpoint: (device) => {
    return {
        'ep1': 1,  // Main thermostat
        'ep2': 2,  // Eco mode
        'ep3': 3,  // Swing
        'ep4': 4,  // Display
        'ep5': 5,  // Night mode
        'ep6': 6,  // Purifier
        'ep7': 7,  // Clean status
        'ep8': 8,  // Mute
    };
},
```

**Critical:** This mapping tells Z2M which physical endpoint each feature uses. With this + standard `fz.on_off`/`tz.on_off`, Z2M automatically:
- Routes `display` commands to endpoint 4
- Processes endpoint 4 attributeReports as `display` updates
- Avoids creating duplicate entities

## How Z2M Multi-Endpoint Works

### With endpoint() Mapping:

1. **User sets `display: OFF` in Z2M UI**
   ```
   tz.on_off → Looks up endpoint('ep4') → Sends to endpoint 4
   ```

2. **Remote presses display button**
   ```
   ESP32 endpoint 4 → AttributeReport → fz.on_off → display: OFF
   ```

3. **Single entity:** `display` (no `_ep4` suffix)

### Without endpoint() Mapping:

- Z2M creates `state_l1`, `state_l2`, etc. for multi-gang switches
- With mapping, Z2M uses your custom names: `eco_mode`, `display`, `mute`

## Entity Naming

**Current (with mapping):**
- ✅ `eco_mode` - Endpoint 2
- ✅ `swing_mode` - Endpoint 3
- ✅ `display` - Endpoint 4
- ✅ `night_mode` - Endpoint 5
- ✅ `purifier` - Endpoint 6
- ✅ `clean_status` - Endpoint 7
- ✅ `mute` - Endpoint 8

**Previous (with custom converters):**
- ❌ `eco_mode` + `eco_mode_ep2` (duplicates)
- ❌ `display` + `display_ep4` (duplicates)
- etc.

## Testing

### 1. Restart Z2M
```bash
sudo systemctl restart zigbee2mqtt
```

### 2. Re-pair Device (Optional but Recommended)
In Z2M UI:
1. **Devices** → **Office_HVAC**
2. **Remove** device
3. **Permit join** → Re-pair device
4. Entities should appear with clean names (no duplicates)

### 3. Test Remote Control
1. **Press display button on IR remote**
2. **Check Z2M UI** → `display` should toggle
3. **Expected log:**
   ```
   z2m: Received Zigbee message from 'Office_HVAC', type 'attributeReport', cluster 'genOnOff', data '{"onOff":0}' from endpoint 4
   ```

### 4. Test Z2M Control
1. **Toggle `display` in Z2M UI**
2. **AC display should toggle**
3. **Z2M entity should update**

### 5. Test All Switches
Repeat for all switch endpoints:
- `eco_mode` (EP2)
- `swing_mode` (EP3)
- `display` (EP4)
- `night_mode` (EP5)
- `purifier` (EP6)
- `clean_status` (EP7) - Read-only, check it reports correctly
- `mute` (EP8)

## Before vs After

### Before Fix:
```yaml
# Z2M Entities (duplicates)
eco_mode: OFF           # ✅ Updated by remote
eco_mode_ep2: ON        # ❌ Stale from Z2M control

display: OFF            # ✅ Updated by remote
display_ep4: ON         # ❌ Stale from Z2M control

swing_mode: ON          # ✅ Updated by remote
swing_mode_ep3: OFF     # ❌ Stale from Z2M control
```

### After Fix:
```yaml
# Z2M Entities (single source of truth)
eco_mode: OFF           # ✅ Updated by remote AND Z2M
display: OFF            # ✅ Updated by remote AND Z2M
swing_mode: ON          # ✅ Updated by remote AND Z2M
```

## Why This Works

### Standard fz.on_off Converter:
```javascript
// From zigbee-herdsman-converters/converters/fromZigbee.js
on_off: {
    cluster: 'genOnOff',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const property = postfixWithEndpointName('state', msg, model, meta);
        return {[property]: msg.data['onOff'] === 1 ? 'ON' : 'OFF'};
    },
}
```

**Key function:** `postfixWithEndpointName()`
- **With endpoint mapping:** Returns `'display'` (from mapping: ep4 → display)
- **Without mapping:** Returns `'state_4'` (generic endpoint number)

### Your endpoint() Mapping:
```javascript
endpoint: (device) => {
    return {
        'ep1': 1,
        'ep2': 2,  // eco_mode
        'ep3': 3,  // swing_mode
        'ep4': 4,  // display
        // ...
    };
},
```

**Z2M logic:**
1. Looks up endpoint 4 → Finds `'ep4'` key
2. Checks exposes for `withEndpoint('ep4')`
3. Finds `display` expose with `withEndpoint('ep4')`
4. Uses property name: `'display'` (NOT `'display_ep4'`)

## Additional Notes

### Why Keep Custom Converters for Some Features?

**Kept:**
- ✅ `fzLocal.thermostat_ep1` - Adds `_ep1` suffix to climate controls (needed for multi-thermostat support in future)
- ✅ `fzLocal.fan_mode` - Maps numeric fan speeds to readable names (quiet, low, medium, high)
- ✅ `fzLocal.error_text` - Parses Zigbee string format from locationDesc

**Removed:**
- ❌ `fzLocal.eco_mode`, `display`, `swing_mode`, etc. - Standard on/off works perfectly

### Can I Remove _ep1 Suffix from Climate?

**Yes**, but requires removing `withEndpoint('ep1')` from climate expose:

```javascript
// Current:
e.climate()
    .withSystemMode(['off', 'auto', 'cool', 'heat', 'dry', 'fan_only'])
    .withEndpoint('ep1'),  // ← Remove this

// Entities: system_mode_ep1, local_temperature_ep1, etc.
```

**After removal:**
```javascript
e.climate()
    .withSystemMode(['off', 'auto', 'cool', 'heat', 'dry', 'fan_only']),
    // No withEndpoint

// Entities: system_mode, local_temperature, etc. (no _ep1)
```

**Trade-off:**
- ✅ Cleaner entity names
- ❌ Can't add second thermostat in future (e.g., for multi-zone AC)

For now, **keeping `_ep1` suffix on climate is fine** - it's future-proof if you ever add a second HVAC unit.

## Summary

**Problem:** Duplicate entities (`display` + `display_ep4`) caused by custom converters.

**Solution:** Use Z2M's standard `fz.on_off` and `tz.on_off` with `endpoint()` mapping.

**Result:** 
- ✅ Single entity per feature
- ✅ Remote control updates Z2M
- ✅ Z2M control updates AC
- ✅ Cleaner UI (no confusing duplicates)

**Files changed:**
- Removed ~100 lines of custom converter code
- Simplified fromZigbee/toZigbee arrays
- No changes to endpoint mapping or exposes
