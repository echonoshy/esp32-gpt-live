idf_component_register(SRCS "main.c" "wifi.c" "ui.c" INCLUDE_DIRS "."
    REQUIRES bsp esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_websocket_client json esp_timer tcp_transport lwip)
