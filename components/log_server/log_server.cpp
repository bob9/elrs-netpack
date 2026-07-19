#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "log_server.h"

// 32KB keeps a few hundred lines — several race starts' worth of per-frame
// ESPNOW send logs — while staying trivial next to the S3's RAM.
#define LOG_BUF_SIZE (32 * 1024)
// Formatted-line scratch; longer lines are truncated in the ring (UART
// output is untouched — the original vprintf gets the full line).
#define LOG_LINE_MAX 256

static char s_buf[LOG_BUF_SIZE];
static size_t s_head = 0;
static bool s_wrapped = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig = NULL;

static void ring_write(const char *data, size_t len)
{
    if (len > LOG_BUF_SIZE)
    {
        data += len - LOG_BUF_SIZE;
        len = LOG_BUF_SIZE;
    }
    taskENTER_CRITICAL(&s_mux);
    size_t tail = LOG_BUF_SIZE - s_head;
    if (len < tail)
    {
        memcpy(s_buf + s_head, data, len);
        s_head += len;
    }
    else
    {
        memcpy(s_buf + s_head, data, tail);
        memcpy(s_buf, data + tail, len - tail);
        s_head = len - tail;
        s_wrapped = true;
    }
    taskEXIT_CRITICAL(&s_mux);
}

static int log_capture_vprintf(const char *fmt, va_list args)
{
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0)
        ring_write(line, (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1);
    return s_orig != NULL ? s_orig(fmt, args) : n;
}

void log_capture_init(void)
{
    if (s_orig == NULL)
        s_orig = esp_log_set_vprintf(log_capture_vprintf);
}

static esp_err_t log_get_handler(httpd_req_t *req)
{
    // Snapshot under the writers' lock so a mid-copy log line can't shear
    // the buffer, then send oldest-first outside it.
    static char snap[LOG_BUF_SIZE]; // static: too big for the httpd task stack
    static SemaphoreHandle_t snapMutex = NULL;
    if (snapMutex == NULL)
        snapMutex = xSemaphoreCreateMutex();
    xSemaphoreTake(snapMutex, portMAX_DELAY);

    taskENTER_CRITICAL(&s_mux);
    size_t head = s_head;
    bool wrapped = s_wrapped;
    memcpy(snap, s_buf, LOG_BUF_SIZE);
    taskEXIT_CRITICAL(&s_mux);

    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = ESP_OK;
    if (wrapped && err == ESP_OK)
        err = httpd_resp_send_chunk(req, snap + head, LOG_BUF_SIZE - head);
    if (head > 0 && err == ESP_OK)
        err = httpd_resp_send_chunk(req, snap, head);
    if (err == ESP_OK)
        err = httpd_resp_send_chunk(req, NULL, 0);

    xSemaphoreGive(snapMutex);
    return err;
}

void log_server_start(httpd_handle_t server)
{
    static const httpd_uri_t log_uri = {
        .uri = "/log", .method = HTTP_GET, .handler = log_get_handler, .user_ctx = NULL};
    httpd_register_uri_handler(server, &log_uri);
}
