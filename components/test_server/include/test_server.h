#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_http_server.h"

// Registers the goggle test page at /test on an existing HTTP server (the
// one the OTA update page starts on port 80): enter a pilot's ELRS bind
// phrase and fire test messages at their goggles - OSD text, OSD clear,
// channel change, time sync and DVR-name. Packets are injected into
// espnow_out, the same ring buffer the TCP server feeds, so they use the
// normal ESPNOW send path.
void test_server_start(httpd_handle_t server, RingbufHandle_t espnow_out);
