#pragma once
#include "esp_transport.h"
// Bind the sole voice client before it starts. Its TX mutex serializes every call.
void ws_send_bind(esp_transport_handle_t websocket, esp_transport_handle_t parent);
