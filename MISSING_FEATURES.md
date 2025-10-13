# Adding Eco, Swing, Display, and Error Text Features

## Current Situation

Your original requirements included:
- ✅ **Mode control** (Off/Auto/Cool/Heat/Dry/Fan) - **WORKING** via Thermostat cluster
- ✅ **Temperature control** - **WORKING** via Thermostat cluster  
- ✅ **Fan speed** - **WORKING** via Fan Control cluster
- ❌ **Eco Mode** on/off - **REMOVED** (was causing errors)
- ❌ **Swing** on/off - **REMOVED** (was causing errors)
- ❌ **Display** on/off - **REMOVED** (was causing errors)
- ❌ **Error text message** - **REMOVED** (was causing errors)

These features were removed because manufacturer-specific custom attributes (0xF000-0xF003) in the standard Thermostat cluster caused Z2M to reject the device.

## Why They're Not Visible

The Zigbee Thermostat cluster (0x0201) has a **fixed set of standard attributes**. Custom attributes need:
1. Proper manufacturer code
2. Special attribute definitions with full configuration
3. Z2M converter support

Simply adding them with `esp_zb_thermostat_cluster_add_attr()` doesn't work - the stack rejects them as "incorrect/unsupported".

## Solution Options

### 🎯 Option 1: Additional Endpoints with Standard Clusters (RECOMMENDED)

Create **3 additional endpoints** using standard Zigbee clusters that Z2M already understands:

**Endpoint 2: Eco Mode Switch**
- Cluster: On/Off (0x0006)
- Device Type: On/Off Switch (0x0000)
- Control: Binary switch for Eco mode

**Endpoint 3: Swing Mode Switch**  
- Cluster: On/Off (0x0006)
- Device Type: On/Off Switch (0x0000)
- Control: Binary switch for Swing

**Endpoint 4: Display Switch**
- Cluster: On/Off (0x0006)
- Device Type: On/Off Switch (0x0000)
- Control: Binary switch for Display

**For Error Text:**
- Use attributes already in Basic cluster:
  - `deviceEnabled` (0x0012) - boolean to indicate error state
  - Or add endpoint 5 with Binary Input cluster

### Advantages:
✅ Uses standard Zigbee clusters - guaranteed Z2M support
✅ No need for custom converters
✅ Each feature gets its own entity in Home Assistant
✅ Clean separation of concerns
✅ Easy to control via Z2M UI

### Implementation:

```c
/* Endpoint 2: Eco Mode Switch */
esp_zb_cluster_list_t *eco_clusters = esp_zb_zcl_cluster_list_create();
esp_zb_attribute_list_t *eco_basic = esp_zb_basic_cluster_create(&basic_cfg);
esp_zb_cluster_list_add_basic_cluster(eco_clusters, eco_basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

esp_zb_on_off_cluster_cfg_t eco_cfg = {
    .on_off = false,
};
esp_zb_attribute_list_t *eco_on_off = esp_zb_on_off_cluster_create(&eco_cfg);
esp_zb_cluster_list_add_on_off_cluster(eco_clusters, eco_on_off, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

esp_zb_endpoint_config_t eco_config = {
    .endpoint = 2,
    .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
    .app_device_id = ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID,
};
esp_zb_ep_list_add_ep(esp_zb_ep_list, eco_clusters, eco_config);
```

---

### 🔧 Option 2: Manufacturer-Specific Cluster (ADVANCED)

Create a **completely separate custom cluster** (not modify standard Thermostat):

**Custom Cluster ID:** 0xFC00 (manufacturer-specific range)

**Attributes:**
- 0x0000: Eco Mode (boolean)
- 0x0001: Swing Mode (boolean)  
- 0x0002: Display On (boolean)
- 0x0003: Error Text (string)

### Advantages:
✅ All features in one cluster
✅ Proper manufacturer-specific implementation
✅ Follows Zigbee spec

### Disadvantages:
❌ Requires Z2M external converter (more complex)
❌ Won't auto-discover in Z2M
❌ More code to maintain

---

### 🛠️ Option 3: Use Existing Thermostat Attributes Creatively

Map your features to **existing but unused** thermostat attributes:

**For Eco Mode:**
- Use `occupancy` attribute (0x0002) in Thermostat cluster
- Or `unoccupiedHeatingSetpoint` as a flag

**For Display:**
- Use `temperatureDisplayMode` (0x0000 in Thermostat UI Config cluster 0x0204)

**For Swing:**
- Could use an additional Fan Control attribute like `fanModeSequence`

### Advantages:
✅ Standard attributes = automatic Z2M support
✅ No extra endpoints needed

### Disadvantages:
❌ Not semantically correct (misusing attributes)
❌ Confusing for users
❌ May conflict with Z2M expectations

---

### 📱 Option 4: Control via Dev Console (CURRENT STATE)

You can **manually control** eco/swing/display by:

1. Sending UART commands directly from ESP32
2. Create a simple web server on ESP32 for configuration
3. Use ESP-IDF console commands via serial

The HVAC driver **already has these functions**:
```c
hvac_set_eco_mode(bool eco);
hvac_set_display(bool on);
hvac_set_swing(bool on);
```

### Advantages:
✅ Quickest solution - no Zigbee changes needed
✅ Features work, just not exposed to Z2M

### Disadvantages:
❌ Not controllable from Home Assistant/Z2M
❌ Less user-friendly

---

## 🎖️ RECOMMENDED APPROACH

**Use Option 1: Additional Endpoints**

This gives you:
- ✅ 4 entities in Home Assistant:
  1. Climate control (endpoint 1) - temp, mode, fan
  2. Eco mode switch (endpoint 2)
  3. Swing switch (endpoint 3)
  4. Display switch (endpoint 4)
- ✅ Full Z2M compatibility
- ✅ Clean, standard implementation
- ✅ Easy to use

For error text, you have two sub-options:
- **A)** Add endpoint 5 with a text sensor (using Multistate Value cluster 0x0014)
- **B)** Just log errors to ESP32 console and monitor via serial/MQTT

---

## Implementation Status

**Currently in your code:**
- The `hvac_driver.c` **already has** functions for eco/swing/display
- They just aren't exposed via Zigbee
- The HVAC state structure tracks them
- UART commands include them

**To add back with Option 1:**
- I can add the 3 extra endpoints (5-10 minutes work)
- Update attribute handler to map On/Off to HVAC commands
- Test in Z2M

---

## Would You Like Me To:

1. **Implement Option 1** - Add the 3 switch endpoints (eco, swing, display)?
2. **Implement Option 2** - Create custom manufacturer cluster?
3. **Keep as-is** - Control eco/swing/display via serial console only?
4. **Show me how to test** - Demo controlling these via Dev Console manually?

Let me know which approach you prefer!
