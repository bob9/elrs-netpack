
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "espnow_server.h"
#include "tcp_server.h"
#include "net_config.h"
#include "rtc_sync.h"
#include "tasks.h"
#include "msp.h"

TaskHandle_t tcpTaskHandle = NULL;
TaskHandle_t espnowTaskHandle = NULL;

RingbufHandle_t xRingReceivedSocket, xRingReceivedEspnow;

TaskBufferParams espnow_params, tcp_server_params;

extern "C" void app_main(void)
{
    // Create the buffers used for passing data across the different interfaces
    xRingReceivedSocket = xRingbufferCreateNoSplit(sizeof(mspPacket_t), 1000);
    xRingReceivedEspnow = xRingbufferCreateNoSplit(sizeof(mspPacket_t), 50);

    // Initialize NVS before spawning the tasks that read settings from it
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());

    // Use both cores of the ESP32 for handling interfaces.
    // Assign ESPNOW to Core 0
    espnow_params = (TaskBufferParams){
        .write = xRingReceivedEspnow,
        .read = xRingReceivedSocket};
    xTaskCreatePinnedToCore(runESPNOWServer, "ESPNOWTask", 4096, (void *)&espnow_params, 10, &espnowTaskHandle, 0);

    // Assign TCP Socket server to Core 1
    tcp_server_params = (TaskBufferParams){
        .write = xRingReceivedSocket,
        .read = xRingReceivedEspnow};
    xTaskCreatePinnedToCore(run_tcp_server, "SocketManagerTask", 4096, (void *)&tcp_server_params, 10, &tcpTaskHandle, 1);

    // Sync the clock over NTP and forward the time to the backpack(s) via
    // the same buffer the TCP server uses for outgoing ESPNOW packets
    rtc_sync_start(xRingReceivedSocket);

    // Serial console on the USB port for setting network details (see 'netconfig')
    net_config_start_console();
}
