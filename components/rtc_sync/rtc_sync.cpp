#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "msp.h"
#include "msptypes.h"
#include "rtc_sync.h"

#define STORAGE_NAMESPACE "netpack"
#define STORAGE_TZ_KEY "time_tz"
#define STORAGE_NTP_KEY "time_ntp"

#define DEFAULT_NTP_SERVER "pool.ntp.org"
#define DEFAULT_TZ "UTC0"

// A clock reading earlier than this (1 Jan 2024) means SNTP has not synced yet
#define RTC_VALID_EPOCH 1704067200

// Once synced, resend the time to the backpack(s) to correct for drift
#define RTC_RESYNC_INTERVAL_MS 60000
// Until the first successful send, check for a synced clock more frequently
#define RTC_RETRY_INTERVAL_MS 5000
// While an external time source (the TCP client, e.g. dd-pits) is actively
// sending SET_RTC, our own broadcasts pause for this long after each one
#define RTC_EXTERNAL_HOLDOFF_US (5LL * 60 * 1000000)

static const char *TAG = "rtc_sync";

static RingbufHandle_t xRingEspnowOut = NULL;
static TaskHandle_t rtcTaskHandle = NULL;

static char ntp_server[64] = DEFAULT_NTP_SERVER;
static char tz_string[64] = DEFAULT_TZ;

// esp_timer time of the last externally-received SET_RTC, 0 = never
static volatile int64_t last_external_us = 0;

static bool external_source_active(void)
{
    int64_t seen = last_external_us;
    return seen != 0 && esp_timer_get_time() - seen < RTC_EXTERNAL_HOLDOFF_US;
}

static void load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;

    size_t len = sizeof(tz_string);
    nvs_get_str(handle, STORAGE_TZ_KEY, tz_string, &len);
    len = sizeof(ntp_server);
    nvs_get_str(handle, STORAGE_NTP_KEY, ntp_server, &len);

    nvs_close(handle);
}

static esp_err_t save_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    if ((err = nvs_set_str(handle, STORAGE_TZ_KEY, tz_string)) == ESP_OK &&
        (err = nvs_set_str(handle, STORAGE_NTP_KEY, ntp_server)) == ESP_OK)
        err = nvs_commit(handle);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to save time config (%s)", esp_err_to_name(err));

    nvs_close(handle);
    return err;
}

static void apply_timezone(void)
{
    setenv("TZ", tz_string, 1);
    tzset();
}

// Queue the current local time for the ESPNOW server task to send as an
// MSP_ELRS_BACKPACK_SET_RTC packet. The payload layout matches what the
// HDZero goggles expect: raw struct tm fields (year since 1900, month 0-11).
static bool send_rtc_packet(void)
{
    time_t now = time(NULL);
    if (now < RTC_VALID_EPOCH)
        return false;

    struct tm timeData;
    localtime_r(&now, &timeData);

    mspPacket_t packet;
    packet.reset();
    packet.makeCommand();
    packet.function = MSP_ELRS_BACKPACK_SET_RTC;
    packet.addByte(timeData.tm_year);
    packet.addByte(timeData.tm_mon);
    packet.addByte(timeData.tm_mday);
    packet.addByte(timeData.tm_hour);
    packet.addByte(timeData.tm_min);
    packet.addByte(timeData.tm_sec);

    if (xRingbufferSend(xRingEspnowOut, &packet, sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to add RTC packet to ring buffer");
        return false;
    }

    ESP_LOGI(TAG, "Queued RTC time %04d-%02d-%02d %02d:%02d:%02d for ESPNOW send",
             timeData.tm_year + 1900, timeData.tm_mon + 1, timeData.tm_mday,
             timeData.tm_hour, timeData.tm_min, timeData.tm_sec);
    return true;
}

static void runRTCTask(void *pvParameters)
{
    bool synced = false;
    while (1)
    {
        // Wake early if an immediate send was requested via rtc_sync_send_now
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(synced ? RTC_RESYNC_INTERVAL_MS : RTC_RETRY_INTERVAL_MS));

        // An external sender (dd-pits over TCP) is already broadcasting the
        // time - its packets are forwarded as-is, so stay quiet to avoid two
        // sources fighting over the goggle clocks
        if (external_source_active())
            continue;

        if (send_rtc_packet() && !synced)
        {
            synced = true;
            ESP_LOGI(TAG, "Clock synchronized, time now being sent over ESPNOW");
        }
    }
}

void rtc_sync_send_now(void)
{
    if (rtcTaskHandle != NULL)
        xTaskNotifyGive(rtcTaskHandle);
}

void rtc_sync_external_time(const uint8_t *payload, uint16_t size)
{
    if (size < 6)
        return;

    // The sender transmits local wall time; interpret it under our
    // configured TZ so localtime_r round-trips the same fields (with the
    // default UTC0 TZ the fields pass through 1:1)
    struct tm timeData = {};
    timeData.tm_year = payload[0];
    timeData.tm_mon = payload[1];
    timeData.tm_mday = payload[2];
    timeData.tm_hour = payload[3];
    timeData.tm_min = payload[4];
    timeData.tm_sec = payload[5];
    timeData.tm_isdst = -1;

    timeval tv = {mktime(&timeData), 0};
    settimeofday(&tv, NULL);

    last_external_us = esp_timer_get_time();
}

static int cmd_timeconfig(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "show") == 0)
    {
        printf("NTP server: %s\n", ntp_server);
        printf("Timezone:   %s (POSIX TZ format)\n", tz_string);

        time_t now = time(NULL);
        if (now < RTC_VALID_EPOCH)
            printf("Local time: not synced yet\n");
        else
        {
            struct tm timeData;
            char buf[32];
            localtime_r(&now, &timeData);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeData);
            printf("Local time: %s\n", buf);
        }
        return 0;
    }

    if (strcmp(argv[1], "tz") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: timeconfig tz <posix-tz>  e.g. timeconfig tz AEST-10\n");
            return 1;
        }
        strlcpy(tz_string, argv[2], sizeof(tz_string));
        apply_timezone();
        if (save_config() != ESP_OK)
            return 1;
        printf("Saved: timezone %s (applied immediately)\n", tz_string);
        return 0;
    }

    if (strcmp(argv[1], "server") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: timeconfig server <hostname-or-ip>\n");
            return 1;
        }
        strlcpy(ntp_server, argv[2], sizeof(ntp_server));
        if (save_config() != ESP_OK)
            return 1;
        printf("Saved: NTP server %s. Type 'reboot' to apply.\n", ntp_server);
        return 0;
    }

    printf("Usage:\n"
           "  timeconfig                     Show time settings and current local time\n"
           "  timeconfig tz <posix-tz>       Set the timezone, e.g. AEST-10 or AEST-10AEDT,M10.1.0,M4.1.0/3\n"
           "  timeconfig server <host|ip>    Set the NTP server (reboot to apply)\n");
    return 1;
}

void rtc_sync_start(RingbufHandle_t espnow_out)
{
    xRingEspnowOut = espnow_out;

    load_config();
    apply_timezone();

    // Start SNTP. Resolution/sync begins once the network is up and is
    // retried automatically by lwip, so this is safe to call before the
    // Ethernet link is established.
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntp_server);
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));

    xTaskCreate(runRTCTask, "RTCSyncTask", 4096, NULL, 5, &rtcTaskHandle);

    const esp_console_cmd_t timeconfig_cmd = {
        .command = "timeconfig",
        .help = "Show or set time settings: timeconfig | timeconfig tz <posix-tz> | timeconfig server <host|ip>",
        .hint = NULL,
        .func = &cmd_timeconfig,
        .argtable = NULL,
        .func_w_context = NULL,
        .context = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&timeconfig_cmd));

    ESP_LOGI(TAG, "RTC sync started (NTP server %s, TZ %s)", ntp_server, tz_string);
}
