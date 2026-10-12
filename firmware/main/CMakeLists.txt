idf_component_register(SRCS "main.c" "controls.c" "adpcm.c" "ws_send.c" "wifi.c" "wifi_setup.c" "wifi_profiles.c" "ui.c" "fonts/ui_font_14.c" "fonts/ui_font_16.c" "fonts/ui_font_18.c" "fonts/ui_font_22.c" "assets/ui_wordmark.c" "fonts/ui_font_comic_80.c" "fonts/ui_font_comic_52.c" "fonts/ui_font_comic_44.c" "settings.c" "menu.c" "portal.c" "firmware_update.c" INCLUDE_DIRS "."
    REQUIRES esp_http_client mbedtls bsp esp_wifi esp_event esp_netif esp_http_server nvs_flash esp_websocket_client app_update esp_app_format json esp_timer tcp_transport lwip
    EMBED_TXTFILES "settings.html" "setup.html")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_transport_ws_send_raw")
