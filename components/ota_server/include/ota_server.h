#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server on CONFIG_OTA_SERVER_PORT (default 80) serving the
   firmware update page (GET /) and the OTA upload endpoint (POST /api/update).
   Returns the server handle so other components can register additional pages
   on the same port, or NULL if the server failed to start. */
httpd_handle_t ota_server_start(void);

#ifdef __cplusplus
}
#endif
