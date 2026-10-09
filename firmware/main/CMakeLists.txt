idf_component_register(SRCS "main.c" "controls.c" "adpcm.c" "ws_send.c" "wifi.c" "ui.c" "fonts/ui_font_18.c" "fonts/ui_font_22.c" "settings.c" "menu.c" "portal.c" INCLUDE_DIRS "."
    REQUIRES bsp esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_websocket_client app_update esp_app_format json esp_timer tcp_transport lwip
    EMBED_TXTFILES "settings.html")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_ws_send_raw")
