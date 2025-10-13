#include "bme280_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BME280_APP";
bme280_handle_t g_bme280 = NULL;

esp_err_t bme280_app_init(i2c_bus_handle_t i2c_bus)
{
    if (!i2c_bus) {
        ESP_LOGE(TAG, "i2c_bus handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    g_bme280 = bme280_create(i2c_bus, BME280_I2C_ADDRESS_DEFAULT);
    if (!g_bme280) {
        ESP_LOGE(TAG, "Failed to create BME280 handle");
        return ESP_FAIL;
    }
    esp_err_t err = bme280_default_init(g_bme280);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BME280 default init failed");
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for sensor to settle
    ESP_LOGI(TAG, "BME280 sensor initialized");
    return ESP_OK;
}

esp_err_t bme280_app_read_temperature(float *temperature)
{
    if (!g_bme280 || !temperature) return ESP_ERR_INVALID_ARG;
    return bme280_read_temperature(g_bme280, temperature);
}

esp_err_t bme280_app_read_humidity(float *humidity)
{
    if (!g_bme280 || !humidity) return ESP_ERR_INVALID_ARG;
    return bme280_read_humidity(g_bme280, humidity);
}

esp_err_t bme280_app_read_pressure(float *pressure)
{
    if (!g_bme280 || !pressure) return ESP_ERR_INVALID_ARG;
    return bme280_read_pressure(g_bme280, pressure);
}
