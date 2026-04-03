"""ZHA Quirk for ACW02-ZB HVAC Thermostat Controller.

This quirk adds proper ZHA support for the ACW02-ZB, a custom DIY Zigbee HVAC
controller designed to replace the ACW02 WiFi module in Airton air conditioning units.

Installation:
    Copy this file to your ZHA custom quirks directory and configure it in
    configuration.yaml:

        zha:
          custom_quirks_path: /config/custom_zha_quirks

Device endpoints:
    EP1: Main thermostat (Thermostat + FanControl + Basic + Identify + OTA client)
    EP2: Eco mode switch (OnOff)
    EP3: Swing mode switch (OnOff)
    EP4: Display switch (OnOff)
    EP5: Night mode switch (OnOff)
    EP6: Purifier/ionizer switch (OnOff)
    EP7: Filter clean status - read-only (OnOff, appears as a switch - do not toggle)
    EP8: Mute switch (OnOff)
    EP9: AC error status - read-only (OnOff, appears as a switch - do not toggle)

Fan modes:
    The ACW02 firmware uses non-standard fan mode values that differ from the
    Zigbee spec. This quirk overrides the Fan cluster attribute type so ZHA
    displays the correct symbolic names in the attribute panel.

    Value  ACW02 label   Zigbee spec label (incorrect without quirk)
    0x00   Auto          Off
    0x01   Low (P20)     Low           <- coincidentally correct
    0x02   LowMed (P40)  Medium        <- close
    0x03   Medium (P60)  High          <- wrong
    0x04   MedHigh (P80) On            <- wrong
    0x05   High (P100)   Auto          <- wrong
    0x06   Quiet/SILENT  Smart         <- wrong

    Important: The HA climate entity derives its fan_mode display from ZHA's
    HVAC cluster handler, which uses the standard FanMode enum. The custom enum
    defined here corrects the ZHA attribute inspector view but does NOT fix the
    climate entity fan mode labels. To use fan mode correctly from HA, use the
    zha.set_attribute_value service to write the raw integer value to the
    fan_mode attribute on EP1.

Error text (locationDescription attribute, EP1 Basic cluster, attribute 0x0010):
    ZHA does not automatically create a sensor entity for this attribute.
    To surface it in HA, add a periodic automation that calls
    zha.get_attribute_value with cluster_id=0, attribute=16 on EP1,
    then stores the result in a helper entity.

EP7 / EP9 read-only behavior:
    ZHA creates switch entities for these endpoints because they use the standard
    genOnOff cluster. They are hardware read-only (the AC unit drives them).
    Disable the switch entities in HA or protect them with a guard automation
    to prevent accidental writes.

Polling (runningMode, fanMode, locationDesc):
    The ESP-Zigbee stack cannot auto-report runningMode and fanMode; they must
    be polled. In ZHA, add a periodic automation that calls
    zha.issue_zigbee_cluster_command or zha.get_attribute_value for these
    attributes on EP1 every 60 seconds.
"""

import zigpy.types as t
from zigpy.profiles import zha
from zigpy.quirks import CustomCluster, CustomDevice
from zigpy.zcl.clusters.general import Basic, Identify, OnOff, Ota
from zigpy.zcl.clusters.hvac import Fan, Thermostat

from zhaquirks.const import (
    DEVICE_TYPE,
    ENDPOINTS,
    INPUT_CLUSTERS,
    MODELS_INFO,
    OUTPUT_CLUSTERS,
    PROFILE_ID,
)

# HA device type IDs (from ESP-IDF Zigbee SDK defines)
THERMOSTAT_DEVICE_TYPE = 0x0301   # ESP_ZB_HA_THERMOSTAT_DEVICE_ID
ON_OFF_OUTPUT_DEVICE_TYPE = 0x0010  # ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID

MANUFACTURER = "Custom devices (DiY)"
MODEL = "acw02-z"


class ACW02FanMode(t.enum8):
    """ACW02-specific fan mode enum.

    Differs from the standard Zigbee FanMode enum.  Values are those written
    to / read from the hvacFanCtrl fanMode attribute (cluster 0x0202, attr 0x0000).
    """

    Auto = 0x00     # AUTO - automatic speed selection
    Low = 0x01      # P20  - 20 % fan speed
    LowMed = 0x02   # P40  - 40 % fan speed
    Medium = 0x03   # P60  - 60 % fan speed
    MedHigh = 0x04  # P80  - 80 % fan speed
    High = 0x05     # P100 - 100 % fan speed
    Quiet = 0x06    # SILENT mode
    Turbo = 0x0D    # TURBO mode (treated as Quiet by the AC firmware)


class ACW02FanCluster(CustomCluster, Fan):
    """Fan Control cluster with ACW02-specific fan mode enum.

    Replaces the standard FanMode enum so that ZHA's attribute inspector
    shows meaningful labels instead of raw integers or incorrect Zigbee spec names.
    """

    attributes = Fan.attributes.copy()
    attributes.update(
        {
            # Attribute 0x0000 - fan_mode - override type to ACW02FanMode
            0x0000: ("fan_mode", ACW02FanMode, True),
        }
    )


class ACW02ZB(CustomDevice):
    """ACW02-ZB HVAC Thermostat Controller - ZHA quirk."""

    signature = {
        MODELS_INFO: [(MANUFACTURER, MODEL)],
        ENDPOINTS: {
            # EP1 - Main thermostat
            1: {
                PROFILE_ID: zha.PROFILE_ID,   # 0x0104
                DEVICE_TYPE: THERMOSTAT_DEVICE_TYPE,  # 0x0301
                INPUT_CLUSTERS: [
                    Basic.cluster_id,       # 0x0000
                    Identify.cluster_id,    # 0x0003
                    Thermostat.cluster_id,  # 0x0201
                    Fan.cluster_id,         # 0x0202
                ],
                OUTPUT_CLUSTERS: [
                    Ota.cluster_id,         # 0x0019 - OTA client
                ],
            },
            # EP2 - Eco mode
            2: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,  # 0x0010
                INPUT_CLUSTERS: [
                    Basic.cluster_id,  # 0x0000
                    OnOff.cluster_id,  # 0x0006
                ],
                OUTPUT_CLUSTERS: [],
            },
            # EP3 - Swing mode
            3: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    OnOff.cluster_id,
                ],
                OUTPUT_CLUSTERS: [],
            },
            # EP4 - Display
            4: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    OnOff.cluster_id,
                ],
                OUTPUT_CLUSTERS: [],
            },
            # EP5 - Night mode
            5: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    OnOff.cluster_id,
                ],
                OUTPUT_CLUSTERS: [],
            },
            # EP6 - Purifier / ionizer
            6: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    OnOff.cluster_id,
                ],
                OUTPUT_CLUSTERS: [],
            },
            # EP7 - Filter clean status (read-only binary sensor)
            7: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    OnOff.cluster_id,
                ],
                OUTPUT_CLUSTERS: [],
            },
            # EP8 - Mute
            8: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    OnOff.cluster_id,
                ],
                OUTPUT_CLUSTERS: [],
            },
            # EP9 - AC error status (read-only binary sensor)
            9: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,
                    OnOff.cluster_id,
                ],
                OUTPUT_CLUSTERS: [],
            },
        },
    }

    replacement = {
        ENDPOINTS: {
            # EP1 - replace Fan cluster with custom enum version
            1: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: THERMOSTAT_DEVICE_TYPE,
                INPUT_CLUSTERS: [
                    Basic.cluster_id,       # 0x0000
                    Identify.cluster_id,    # 0x0003
                    Thermostat.cluster_id,  # 0x0201
                    ACW02FanCluster,        # 0x0202 - custom fan mode enum
                ],
                OUTPUT_CLUSTERS: [
                    Ota.cluster_id,         # 0x0019
                ],
            },
            # EP2-9 are unchanged - pass-through to standard clusters
            2: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
            3: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
            4: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
            5: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
            6: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
            7: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
            8: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
            9: {
                PROFILE_ID: zha.PROFILE_ID,
                DEVICE_TYPE: ON_OFF_OUTPUT_DEVICE_TYPE,
                INPUT_CLUSTERS: [Basic.cluster_id, OnOff.cluster_id],
                OUTPUT_CLUSTERS: [],
            },
        },
    }
