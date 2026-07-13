#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

// Starts the HTTP test page (port 80): enter a pilot's ELRS bind phrase and
// fire test messages at their goggles - OSD text, OSD clear, channel change,
// time sync and DVR-name. Packets are injected into espnow_out, the same
// ring buffer the TCP server feeds, so they use the normal ESPNOW send path.
void test_server_start(RingbufHandle_t espnow_out);
