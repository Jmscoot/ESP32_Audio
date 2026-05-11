idf_component_register(
    SRCS "iot.c"
    INCLUDE_DIRS "."
    REQUIRES  fatfs esp_driver_uart esp_driver_gpio esp_http_client esp_driver_dac esp_wifi mqtt esp_driver_i2s
    PRIV_REQUIRES  esp_adc esp_ringbuf esp_event esp_netif nvs_flash
)
