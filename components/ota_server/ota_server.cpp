#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "ota_server.h"

static const char *TAG = "ota_server";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* Every ESP32 app image starts with esp_image_header_t (24 bytes) and the
   first segment header (8 bytes), followed by esp_app_desc_t. Buffering this
   much of the upload lets us reject non-firmware files and images built for a
   different project before anything is written to flash. */
#define IMAGE_HEADER_MAGIC 0xE9
#define APP_DESC_OFFSET 32
#define VALIDATE_HEADER_LEN (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

#define RECV_BUFFER_LEN 4096

static bool update_in_progress = false;

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    char json[352];
    snprintf(json, sizeof(json),
             "{\"project\":\"%s\",\"version\":\"%s\",\"idf\":\"%s\",\"built\":\"%s %s\","
             "\"slot\":\"%s\",\"slot_size\":%u}",
             app->project_name, app->version, app->idf_ver, app->date, app->time,
             running ? running->label : "?",
             next ? (unsigned)next->size : 0);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGE(TAG, "OTA rejected: %s", msg);
    update_in_progress = false;
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char json[192];
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", msg);
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

static esp_err_t update_post_handler(httpd_req_t *req)
{
    if (update_in_progress)
        return update_fail(req, "409 Conflict", "Another update is already in progress");

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL)
        return update_fail(req, "500 Internal Server Error", "No OTA partition available");

    size_t total = req->content_len;
    if (total < VALIDATE_HEADER_LEN)
        return update_fail(req, "400 Bad Request", "Upload is too small to be a firmware image");
    if (total > target->size)
        return update_fail(req, "413 Payload Too Large", "Image is larger than the OTA slot");

    update_in_progress = true;
    ESP_LOGI(TAG, "OTA update started: %u bytes -> partition %s", (unsigned)total, target->label);

    esp_ota_handle_t handle = 0;
    bool ota_started = false;
    uint8_t hdr[VALIDATE_HEADER_LEN];
    char buf[RECV_BUFFER_LEN];
    size_t received = 0;

    while (received < total)
    {
        int n = httpd_req_recv(req, buf, MIN(sizeof(buf), total - received));
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (n <= 0)
        {
            if (ota_started)
                esp_ota_abort(handle);
            return update_fail(req, "400 Bad Request", "Connection lost during upload");
        }

        size_t consumed = 0;
        if (!ota_started)
        {
            size_t c = MIN((size_t)n, VALIDATE_HEADER_LEN - received);
            memcpy(hdr + received, buf, c);
            consumed = c;
            if (received + c < VALIDATE_HEADER_LEN)
            {
                received += n;
                continue;
            }

            const esp_app_desc_t *desc = (const esp_app_desc_t *)(hdr + APP_DESC_OFFSET);
            if (hdr[0] != IMAGE_HEADER_MAGIC)
                return update_fail(req, "400 Bad Request", "Not an ESP32 firmware image");
            if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD)
                return update_fail(req, "400 Bad Request", "Not an OTA app image - use the ...-ota.bin release asset, not the merged image");
            if (strncmp(desc->project_name, "elrs-netpack", sizeof(desc->project_name)) != 0)
                return update_fail(req, "400 Bad Request", "Image was built for a different project (expected elrs-netpack)");

            if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK)
                return update_fail(req, "500 Internal Server Error", "Failed to start flash write");
            ota_started = true;

            if (esp_ota_write(handle, hdr, VALIDATE_HEADER_LEN) != ESP_OK)
            {
                esp_ota_abort(handle);
                return update_fail(req, "500 Internal Server Error", "Flash write failed");
            }
        }

        if ((size_t)n > consumed &&
            esp_ota_write(handle, buf + consumed, n - consumed) != ESP_OK)
        {
            esp_ota_abort(handle);
            return update_fail(req, "500 Internal Server Error", "Flash write failed");
        }

        received += n;
    }

    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK)
        return update_fail(req, "400 Bad Request", "Image verification failed - firmware not changed");

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK)
        return update_fail(req, "500 Internal Server Error", "Failed to activate new firmware");

    const esp_app_desc_t *desc = (const esp_app_desc_t *)(hdr + APP_DESC_OFFSET);
    char version[sizeof(desc->version) + 1] = {0};
    memcpy(version, desc->version, sizeof(desc->version));

    ESP_LOGI(TAG, "OTA update written and verified (version %s), rebooting into %s", version, target->label);

    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"version\":\"%s\"}", version);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

httpd_handle_t ota_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_OTA_SERVER_PORT;
    config.stack_size = 8192;
    config.core_id = 1; // keep off the ESP-NOW core
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start OTA web server: %s", esp_err_to_name(err));
        return NULL;
    }

    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_get_handler, .user_ctx = NULL};
    const httpd_uri_t info_uri = {.uri = "/api/info", .method = HTTP_GET, .handler = info_get_handler, .user_ctx = NULL};
    const httpd_uri_t update_uri = {.uri = "/api/update", .method = HTTP_POST, .handler = update_post_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &info_uri);
    httpd_register_uri_handler(server, &update_uri);

    ESP_LOGI(TAG, "Firmware update page listening on port %d", CONFIG_OTA_SERVER_PORT);
    return server;
}
