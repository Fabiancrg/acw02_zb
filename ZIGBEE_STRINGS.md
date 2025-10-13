# Zigbee String Attribute Format

## Important: Length-Prefixed Strings

Zigbee string attributes (type `OCTET_STRING` or `CHAR_STRING`) require **length-prefixed** format:
- First byte = length of the string (not including the length byte itself)
- Followed by the actual string characters

### ❌ Wrong (Plain C strings):
```c
char date_code[] = "20251013";      // Missing length prefix!
char sw_build_id[] = "v1.0.0";      // Will cause scrambled data
```

### ✅ Correct (Length-prefixed):
```c
char date_code[] = "\x08""20251013";     // \x08 = 8 characters
char sw_build_id[] = "\x06""v1.0.0";     // \x06 = 6 characters
```

## String Attributes in This Project

### esp_zb_hvac.h
These are **already correct** with length prefixes:
```c
#define ESP_MANUFACTURER_NAME "\x09""ESPRESSIF"      // 9 chars
#define ESP_MODEL_IDENTIFIER "\x07"CONFIG_IDF_TARGET // 7 chars ("esp32c6")
```

### esp_zb_hvac.c
Basic cluster optional attributes (manually added):
```c
char date_code[] = "\x08""20251013";     // 8 chars = "20251013"
char sw_build_id[] = "\x06""v1.0.0";     // 6 chars = "v1.0.0"
```

## Symptoms of Missing Length Prefix

When you use plain C strings without length prefix:
- ✗ Zigbee2MQTT shows scrambled text in device info
- ✗ Some characters may be missing or garbled
- ✗ Attributes may fail to read correctly
- ✗ Device interview may have issues

## References

From Zigbee Cluster Library (ZCL):
- **OCTET_STRING**: First byte = length, then data bytes
- **CHAR_STRING**: First byte = length, then UTF-8 characters

ESP-IDF Zigbee uses this format for:
- `manufacturerName` (Basic cluster, 0x0004)
- `modelIdentifier` (Basic cluster, 0x0005)
- `dateCode` (Basic cluster, 0x0006)
- `swBuildId` (Basic cluster, 0x4000)
- Any custom string attributes

## Quick Check

To verify a length prefix is correct:
1. Count the characters in your string
2. Convert count to hex (e.g., 8 → \x08)
3. Prefix with `"\x??"`
4. Add actual string: `"\x08""20251013"`

**Note**: The double quote `""` between length and string is C string literal concatenation, not required but improves readability.
