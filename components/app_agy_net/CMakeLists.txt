idf_component_register(SRCS "app_agy_net.c"
    INCLUDE_DIRS "include"
    REQUIRES app_agy_ui esp_iris esp_mosaico_app_recovery esp_wifi esp_event esp_netif esp_http_client lwip nvs_flash esp_timer log)
