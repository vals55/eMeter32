idf_component_register(SRCS "stats_push.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client esp-tls esp_timer settings wifi_mgr pzem004t pulse_counter ntp_sync)
 
 
