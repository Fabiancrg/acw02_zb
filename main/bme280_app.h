#pragma once
#include "bme280.h"
#include "i2c_bus.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Handle for BME280 sensor
extern bme280_handle_t g_bme280;

// Initialize BME280 sensor (returns ESP_OK or error)
esp_err_t bme280_app_init(i2c_bus_handle_t i2c_bus);

// Read temperature (Celsius)
esp_err_t bme280_app_read_temperature(float *temperature);

// Read humidity (%)
esp_err_t bme280_app_read_humidity(float *humidity);

// Read pressure (hPa)
esp_err_t bme280_app_read_pressure(float *pressure);

#ifdef __cplusplus
}
#endif
