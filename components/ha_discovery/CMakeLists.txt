idf_component_register(SRCS "ha_discovery.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqtt_mgr settings pzem004t pulse_counter wifi_mgr ntp_sync ota_update esp_timer esp_hw_support freertos)
 
 
