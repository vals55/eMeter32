# Имя mqtt_mgr, а не mqtt: компонент "mqtt" уже есть в ESP-IDF
idf_component_register(SRCS "mqtt_mgr.c"
                       INCLUDE_DIRS "include"
                       REQUIRES mqtt esp_event settings esp_hw_support)
 
 
