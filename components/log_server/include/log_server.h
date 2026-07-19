#pragma once

#include "esp_http_server.h"

// Captures all ESP_LOG output into a RAM ring buffer (UART logging is
// unaffected) and serves it as plain text at GET /log on an existing HTTP
// server — so the per-frame ESPNOW send timings can be read at the field
// without a USB serial cable.

// Install the log capture hook. Call as early as possible in app_main so
// startup lines are captured too.
void log_capture_init(void);

// Register GET /log on the shared port-80 server.
void log_server_start(httpd_handle_t server);
