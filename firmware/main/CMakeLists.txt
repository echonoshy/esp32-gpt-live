idf_component_register(SRCS "main.c" "wifi.c" "ui.c" "settings.c" "portal.c" INCLUDE_DIRS "."
    REQUIRES bsp esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_websocket_client app_update esp_app_format json esp_timer tcp_transport lwip
    EMBED_TXTFILES "settings.html")
