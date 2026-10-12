idf_component_register(SRCS "captive_portal.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_wifi esp_netif esp_event esp_timer esp_driver_gpio lwip settings wifi_mgr)

 
 
