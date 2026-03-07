set(ZCL_UTILITY_OLD_BASE "${IDF_PATH}/examples/zigbee/common/zcl_utility")
set(ZCL_UTILITY_NEW_BASE "${IDF_PATH}/examples/zigbee/zb_common_components/examples_utils")

idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_driver_uart ieee802154 app_update
)

if(EXISTS "${ZCL_UTILITY_OLD_BASE}/src" AND EXISTS "${ZCL_UTILITY_OLD_BASE}/include")
    target_sources(${COMPONENT_LIB} PRIVATE "${ZCL_UTILITY_OLD_BASE}/src/zcl_utility.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${ZCL_UTILITY_OLD_BASE}/include")
elseif(EXISTS "${ZCL_UTILITY_NEW_BASE}/zcl_utility.c" AND EXISTS "${ZCL_UTILITY_NEW_BASE}/include")
    target_sources(${COMPONENT_LIB} PRIVATE "${ZCL_UTILITY_NEW_BASE}/zcl_utility.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${ZCL_UTILITY_NEW_BASE}/include")
else()
    message(FATAL_ERROR "Unable to locate Zigbee zcl_utility in ESP-IDF. Checked ${ZCL_UTILITY_OLD_BASE} and ${ZCL_UTILITY_NEW_BASE}")
endif()
