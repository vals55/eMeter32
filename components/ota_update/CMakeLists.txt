idf_component_register(SRCS "ota_update.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_http_server app_update esp_app_format esp_timer esp_system esp_hw_support mbedtls freertos)
