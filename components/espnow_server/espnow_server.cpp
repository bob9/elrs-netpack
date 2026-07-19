#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include "espnow_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "tasks.h"
#include "msptypes.h"
#include "msp.h"
#include "rtc_sync.h"

#define NO_BINDING_TIMEOUT 120000 / portTICK_PERIOD_MS
#define STORAGE_NAMESPACE "netpack"
#define STORAGE_MAC_KEY "bp_mac_addr"

static const char *TAG = "espnow_server";

const esp_app_desc_t *description = esp_app_get_description();
const TickType_t espnowDelay = CONFIG_ESPNOW_SEND_DELAY / portTICK_PERIOD_MS;

static uint8_t bindAddress[6];
static uint8_t sendAddress[6];
static nvs_handle_t bp_mac_handle;

static TaskHandle_t espnowTaskHandle = NULL;
static TaskHandle_t bindTaskHandle = NULL;
static RingbufHandle_t xRingReceivedEspnow = NULL;

static bool isBinding = false;

// --- Unresponsive-peer probing ----------------------------------------------
// A goggle that's powered off (or out of range) never acks, so one frame to it
// burns the full CONFIG_ESPNOW_MAX_SEND_ATTEMPTS retry budget — serially, on
// the single TCP→ESP-NOW queue — delaying every message queued behind it.
// But one full-budget failure is NOT proof the goggle is gone: at race start
// the 2.4GHz band is saturated by the pilots' ELRS RC links and a present
// goggle can lose a whole frame's budget to one interference burst. So:
//   - only SUPPRESS_AFTER_FAILS consecutive full-budget failures put a peer
//     into probe mode;
//   - probe mode still transmits every frame, just with a single attempt, so
//     the queue keeps moving and a recovering goggle is heard immediately;
//   - any ack fully restores the peer, and a probe window that lapses with no
//     traffic earns the next frame a full retry budget again.
#define SUPPRESS_MAX_PEERS 32
#define SUPPRESS_COOLDOWN_MS 5000
#define SUPPRESS_AFTER_FAILS 2

typedef struct
{
    uint8_t mac[6];
    uint8_t consecFails; // consecutive full-budget no-ack failures
    TickType_t until;    // probe-mode window end (refreshed on each failure)
    bool used;
} suppressed_peer_t;

static suppressed_peer_t suppressedPeers[SUPPRESS_MAX_PEERS];

static suppressed_peer_t *findPeerSlot(const uint8_t *mac, bool alloc)
{
    for (int i = 0; i < SUPPRESS_MAX_PEERS; i++)
    {
        if (suppressedPeers[i].used && memcmp(suppressedPeers[i].mac, mac, 6) == 0)
            return &suppressedPeers[i];
    }
    if (!alloc)
        return NULL;
    for (int i = 0; i < SUPPRESS_MAX_PEERS; i++)
    {
        if (!suppressedPeers[i].used)
        {
            memcpy(suppressedPeers[i].mac, mac, 6);
            suppressedPeers[i].consecFails = 0;
            suppressedPeers[i].used = true;
            return &suppressedPeers[i];
        }
    }
    // Table full: reclaim the first entry.
    memcpy(suppressedPeers[0].mac, mac, 6);
    suppressedPeers[0].consecFails = 0;
    suppressedPeers[0].used = true;
    return &suppressedPeers[0];
}

// peerProbing: true while the peer's frames should get a single attempt
// instead of the full retry budget. A lapsed window keeps the entry (and its
// failure count): the next frame gets a full budget, and one more full-budget
// failure drops the peer straight back into probing.
static bool peerProbing(const uint8_t *mac)
{
    suppressed_peer_t *p = findPeerSlot(mac, false);
    if (p == NULL || p->consecFails < SUPPRESS_AFTER_FAILS)
        return false;
    return (int32_t)(p->until - xTaskGetTickCount()) > 0;
}

// peerSendFailed records an all-attempts-unacked frame (transient local send
// errors must not be counted).
static void peerSendFailed(const uint8_t *mac)
{
    suppressed_peer_t *p = findPeerSlot(mac, true);
    if (p->consecFails < 255)
        p->consecFails++;
    p->until = xTaskGetTickCount() + pdMS_TO_TICKS(SUPPRESS_COOLDOWN_MS);
    if (p->consecFails == SUPPRESS_AFTER_FAILS)
        ESP_LOGW(TAG, "Peer [%d,%d,%d,%d,%d,%d] unacked %d frames in a row; probing with single attempts",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], SUPPRESS_AFTER_FAILS);
}

static void peerAcked(const uint8_t *mac)
{
    suppressed_peer_t *p = findPeerSlot(mac, false);
    if (p != NULL)
        p->used = false;
}

// espnowFuncName maps the MSP functions we forward to short log labels so
// the per-frame send log reads as "osd"/"rtc"/"dvr" instead of hex.
static const char *espnowFuncName(uint16_t function)
{
    switch (function)
    {
    case MSP_ELRS_SET_OSD:
        return "osd";
    case MSP_ELRS_BACKPACK_SET_RTC:
        return "rtc";
    case MSP_ELRS_BACKPACK_SET_DVR_NAME:
        return "dvr";
    case MSP_ELRS_BACKPACK_SET_CHANNEL_INDEX:
        return "channel";
    case MSP_ELRS_BACKPACK_SET_FREQUENCY:
        return "freq";
    case MSP_ELRS_BACKPACK_SET_RECORDING_STATE:
        return "rec";
    default:
        return NULL;
    }
}

// sendDeliveryReport queues a MSP_ELRS_NETPACK_SEND_REPORT back to the TCP
// client (dd-pits): the MAC-ack outcome of one forwarded frame. Non-blocking
// — a full ring drops the report rather than stalling the send loop.
static void sendDeliveryReport(const uint8_t *uid, uint16_t function, bool delivered, uint8_t retries)
{
    mspPacket_t out;
    out.reset();
    out.makeResponse();
    out.function = MSP_ELRS_NETPACK_SEND_REPORT;
    for (int i = 0; i < 6; i++)
        out.addByte(uid[i]);
    out.addByte((function >> 8) & 0xFF);
    out.addByte(function & 0xFF);
    out.addByte(delivered ? 1 : 0);
    out.addByte(retries);

    if (xRingbufferSend(xRingReceivedEspnow, &out, sizeof(mspPacket_t), 0) != pdTRUE)
        ESP_LOGD(TAG, "Delivery report dropped (ring full)");
}

static void sendInProgressResponse()
{
    mspPacket_t out;
    const uint8_t *response = (const uint8_t *)"P";
    out.reset();
    out.makeResponse();
    out.function = MSP_ELRS_BACKPACK_SET_MODE;
    for (uint32_t i = 0; i < 1; i++)
    {
        out.addByte(response[i]);
    }

    if (xRingbufferSend(xRingReceivedEspnow, &out, sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) == pdTRUE)
        ESP_LOGI(TAG, "Added progress response to ring buffer");
    else
        ESP_LOGE(TAG, "Failed to add item progress response to ring buffer");
}

static void runBindTask(void *pvParameters)
{
    vTaskDelay(NO_BINDING_TIMEOUT);
    isBinding = false;
}

static void registerPeer(uint8_t *address)
{
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, address, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK)
    {
        ESP_LOGE(TAG, "ESP-NOW failed to add peer");
        return;
    }
#if CONFIG_ESPNOW_HIGH_RATE
    // ESP-NOW's default TX rate is 1Mbps: ~2ms of airtime per frame, a huge
    // collision cross-section against the pilots' RC links duty-cycling on
    // the same band at race start. 24Mbps cuts a frame to ~0.15ms so it can
    // slot into the gaps of the RC schedule. Range margin shrinks, but
    // goggles are venue-distance, not long-range.
    esp_now_rate_config_t rate = {};
    rate.phymode = WIFI_PHY_MODE_11G;
    rate.rate = WIFI_PHY_RATE_24M;
    if (esp_now_set_peer_rate_config(address, &rate) != ESP_OK)
        ESP_LOGW(TAG, "ESP-NOW peer rate config failed; using default 1Mbps");
#endif
}

static int sendMSPViaEspnow(mspPacket_t *packet)
{
    MSP msp;
    int esp_err = -1;
    uint8_t packetSize = msp.getTotalPacketSize(packet);
    uint8_t nowDataOutput[packetSize];

    uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

    if (!result)
    {
        ESP_LOGE(TAG, "Packet could not be converted to array");
        return esp_err;
    }

    esp_now_peer_num_t pn;
    esp_now_get_peer_num(&pn);

    esp_err = esp_now_send(sendAddress, (uint8_t *)&nowDataOutput, packetSize);

    ESP_LOGI(TAG, "Sent ESPNOW message");
    return esp_err;
}

static void espnowSendCB(const wifi_tx_info_t *mac_addr, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
        xTaskNotify(espnowTaskHandle, (uint32_t)1, eSetValueWithOverwrite);
    else
        xTaskNotify(espnowTaskHandle, (uint32_t)0, eSetValueWithOverwrite);
}

static void espnowRecvCB(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    MSP msp;
    esp_now_peer_info_t peerInfo;
    for (int i = 0; i < len; i++)
    {
        if (msp.processReceivedByte(data[i]))
        {
            mspPacket_t *packet = msp.getReceivedPacket();
            switch (packet->function)
            {
            case MSP_ELRS_BIND:
            {
                if (!isBinding)
                    break;

                if (bindTaskHandle != NULL)
                {
                    vTaskDelete(bindTaskHandle);
                    bindTaskHandle = NULL;
                }

                isBinding = false;

                uint8_t recievedAddress[6];
                for (int i = 0; i < 6; i++)
                {
                    recievedAddress[i] = packet->payload[i];
                }

                recievedAddress[0] = recievedAddress[0] & ~0x01;

                if (recievedAddress[0] == 0 && recievedAddress[1] == 0 && recievedAddress[2] == 0 &&
                    recievedAddress[3] == 0 && recievedAddress[4] == 0 && recievedAddress[5] == 0)
                {
                    ESP_LOGW(TAG, "Preventing UID from being saved to default value");
                    break;
                }

                if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &bp_mac_handle) == ESP_OK)
                {
                    if (nvs_set_blob(bp_mac_handle, STORAGE_MAC_KEY, &recievedAddress, sizeof(recievedAddress)) == ESP_OK)
                    {
                        if (nvs_commit(bp_mac_handle) == ESP_OK)
                            ESP_LOGI(TAG, "UID saved to nvs");
                        else
                            ESP_LOGE(TAG, "Failed to commit nvs data");
                    }
                    else
                        ESP_LOGE(TAG, "Failed to write MAC address to nvs");
                }
                else
                    ESP_LOGE(TAG, "Error opening NVS handle!");

                nvs_close(bp_mac_handle);

                if (bindAddress[0] == sendAddress[0] && bindAddress[1] == sendAddress[1] && bindAddress[2] == sendAddress[2] &&
                    bindAddress[3] == sendAddress[3] && bindAddress[4] == sendAddress[4] && bindAddress[5] == sendAddress[5])
                {
                    memset(&sendAddress, 0, sizeof(sendAddress));
                    memcpy(sendAddress, recievedAddress, 6);

                    if (esp_now_fetch_peer(true, &peerInfo) == ESP_OK)
                        esp_now_del_peer(peerInfo.peer_addr);

                    registerPeer(sendAddress);
                }

                memset(&bindAddress, 0, sizeof(bindAddress));
                memcpy(bindAddress, recievedAddress, 6);

                ESP_LOGI(TAG, "Backpack UID set to: [%d,%d,%d,%d,%d,%d]", bindAddress[0], bindAddress[1],
                         bindAddress[2], bindAddress[3], bindAddress[4], bindAddress[5]);

                break;
            }
            case MSP_ELRS_BACKPACK_SET_RECORDING_STATE:
            {
                if (xRingbufferSend(xRingReceivedEspnow, msp.getReceivedPacket(), sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) == pdTRUE)
                    ESP_LOGI(TAG, "Recording state change added to buffer");
                else
                    ESP_LOGE(TAG, "Failed to add recieved ESPNOW data to ring buffer");
                break;
            }
            case MSP_ELRS_REQU_VTX_PKT:
            {
                // A VRX backpack spams this on boot - send it the current time
                ESP_LOGI(TAG, "VRX backpack requested startup packet, sending time");
                rtc_sync_send_now();
                break;
            }
            }

            msp.markPacketReceived();
        }
    }
}

void runESPNOWServer(void *pvParameters)
{

    TaskBufferParams *buffers = (TaskBufferParams *)pvParameters;
    xRingReceivedEspnow = buffers->write;

    espnowTaskHandle = xTaskGetCurrentTaskHandle();

    uint8_t sendAttempt = 0;
    uint32_t sendSuccess = 0;
    int sendStatus = -1;
    MSP msp;

    esp_err_t err;

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Get Backpack MAC address from NVS
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &bp_mac_handle);
    if (err == ESP_OK)
    {
        uint8_t mac_addr[6];
        size_t size = sizeof(mac_addr);
        err = nvs_get_blob(bp_mac_handle, STORAGE_MAC_KEY, bindAddress, &size);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGW(TAG, "Unable to retreive mac address from nvs");
        else
        {
            memcpy(sendAddress, bindAddress, 6);
            bindAddress[0] = bindAddress[0] & ~0x01;
            ESP_LOGI(TAG, "Backpack UID: [%d,%d,%d,%d,%d,%d]", bindAddress[0], bindAddress[1],
                     bindAddress[2], bindAddress[3], bindAddress[4], bindAddress[5]);
        }
    }
    else
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));

    nvs_close(bp_mac_handle);

    // Start WiFi for ESPNOW
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(esp_wifi_start());
    // ESP-NOW only — never associated to an AP, so modem power save (the STA
    // default) just adds send-callback latency and drops. Keep the radio on.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));

    // Start ESPNOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnowSendCB));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnowRecvCB));

    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, bindAddress));

    // Register default peer if UID set
    if (bindAddress[0] != 0 || bindAddress[1] != 0 || bindAddress[2] != 0 ||
        bindAddress[3] != 0 || bindAddress[4] != 0 || bindAddress[5] != 0)
        registerPeer(bindAddress);

    while (1)
    {
        // Send data from incoming buffer over ESPNOW
        size_t item_size;
        mspPacket_t *packet = (mspPacket_t *)xRingbufferReceive(buffers->read, &item_size, portMAX_DELAY);
        if (packet != NULL)
        {
            uint8_t packetSize = msp.getTotalPacketSize(packet);
            uint8_t nowDataOutput[packetSize];
            uint8_t result = msp.convertToByteArray(packet, nowDataOutput);

            if (result)
            {
                switch (packet->function)
                {
                case MSP_ELRS_GET_BACKPACK_VERSION:
                {
                    ESP_LOGI(TAG, "Processing MSP_ELRS_GET_BACKPACK_VERSION...");

                    mspPacket_t out;
                    out.reset();
                    out.makeResponse();
                    out.function = MSP_ELRS_GET_BACKPACK_VERSION;
                    for (size_t i = 0; i < sizeof(description->version); i++)
                    {
                        out.addByte(description->version[i]);
                    }

                    if (xRingbufferSend(xRingReceivedEspnow, &out, sizeof(mspPacket_t), pdMS_TO_TICKS(1000)) == pdTRUE)
                        ESP_LOGI(TAG, "Added device version to ring buffer");
                    else
                        ESP_LOGE(TAG, "Failed to add item to ring buffer");

                    break;
                }
                case MSP_ELRS_BACKPACK_SET_MODE:
                {
                    if (packet->payloadSize == 1)
                    {
                        if (packet->payload[0] == 'B')
                        {
                            ESP_LOGI(TAG, "Enter binding mode...");
                            isBinding = true;
                        }
                        if (bindTaskHandle != NULL)
                        {
                            vTaskDelete(bindTaskHandle);
                            bindTaskHandle = NULL;
                        }

                        xTaskCreate(runBindTask, "BindTask", 4096, NULL, 10, &bindTaskHandle);
                        sendInProgressResponse();
                    }
                    break;
                }
                case MSP_ELRS_SET_SEND_UID:
                {
                    uint8_t mode = packet->readByte();

                    uint8_t target[6];
                    if (mode == 0x01)
                    {
                        for (int i = 0; i < 6; i++)
                            target[i] = packet->readByte();
                        // A MAC can only be unicast: an odd first byte makes
                        // esp_wifi_set_mac fail, and ESP_ERROR_CHECK would
                        // reboot the netpack mid-race. Mask it like the bind
                        // and startup paths do.
                        target[0] = target[0] & ~0x01;
                    }
                    else
                    {
                        memcpy(target, bindAddress, 6);
                    }

                    // An unbound netpack has no default identity to reset to
                    // (an all-zero MAC is invalid anyway) — keep the last
                    // pilot's UID applied so the next session to the same
                    // pilot needs no MAC change at all.
                    if (target[0] == 0 && target[1] == 0 && target[2] == 0 &&
                        target[3] == 0 && target[4] == 0 && target[5] == 0)
                        break;

                    // Already targeting this UID → keep the MAC. A MAC change
                    // is disruptive: the first frame sent after one routinely
                    // loses its send callback and burns a full timeout, so
                    // set/reset pairs that land on the same UID (single-pilot
                    // sessions back to back) must not touch the radio.
                    if (memcmp(target, sendAddress, 6) == 0)
                        break;

                    // Unregister current peer
                    esp_now_del_peer(sendAddress);
                    memcpy(sendAddress, target, 6);

                    esp_err_t macErr = esp_wifi_set_mac(WIFI_IF_STA, sendAddress);
                    if (macErr != ESP_OK)
                        ESP_LOGE(TAG, "Failed to set STA MAC for send UID: %s", esp_err_to_name(macErr));

                    if (sendAddress[0] != 0 || sendAddress[1] != 0 || sendAddress[2] != 0 ||
                        sendAddress[3] != 0 || sendAddress[4] != 0 || sendAddress[5] != 0)
                        registerPeer(sendAddress);

                    // Let the driver settle so the next frame's send callback
                    // isn't lost to the MAC change — 20ms here is far cheaper
                    // than the 150ms callback timeout a lost frame costs.
                    vTaskDelay(pdMS_TO_TICKS(20));

                    ESP_LOGI(TAG, "Send UID set to: [%d,%d,%d,%d,%d,%d]", sendAddress[0], sendAddress[1],
                             sendAddress[2], sendAddress[3], sendAddress[4], sendAddress[5]);

                    break;
                }
                case MSP_ELRS_BACKPACK_SET_RTC:
                    // The TCP client (e.g. dd-pits) is sending the time: seed
                    // our own clock from it and pause the SNTP broadcasts
                    // (no-op for our own queued time packets), then send it
                    // over ESPNOW with the normal retry loop below - a single
                    // unacked attempt right after a SET_SEND_UID MAC change
                    // is too easily lost
                    rtc_sync_external_time(packet->payload, packet->payloadSize);
                    [[fallthrough]];
                default:
                {
                    // A repeatedly-unacked peer gets single-attempt probes
                    // instead of the full retry budget (see probing note
                    // above): every frame is still transmitted and reported,
                    // but an absent goggle can't head-of-line-block the live
                    // OSD queue.
                    bool probing = peerProbing(sendAddress);
                    uint8_t maxAttempts = probing ? 1 : CONFIG_ESPNOW_MAX_SEND_ATTEMPTS;
                    TickType_t sendStart = xTaskGetTickCount();

                    sendAttempt = 0;
                    sendSuccess = 0; // never inherit the previous packet's result
                    do
                    {
                        // Space retries apart so one 2.4GHz interference burst
                        // (an ELRS TX schedule slot) can't eat the whole
                        // budget. No delay before the first attempt or after
                        // the last — the happy path stays latency-free.
                        if (sendAttempt > 0)
                            vTaskDelay(espnowDelay);
                        sendStatus = sendMSPViaEspnow(packet);
                        if (sendStatus == ESP_OK)
                        {
                            // Bounded wait: a lost send callback must fail the
                            // attempt, not hang the send task forever. The
                            // callback normally lands within tens of ms (as
                            // soon as MAC retries conclude), so 150ms is
                            // already generous — a longer bound just inflates
                            // OSD latency whenever the 2.4GHz band is
                            // saturated, and a duplicate re-send after a late
                            // callback is harmless (OSD writes are
                            // idempotent).
                            if (xTaskNotifyWait(0x00, ULONG_MAX, &sendSuccess, pdMS_TO_TICKS(150)) != pdTRUE)
                                sendSuccess = 0;
                        }
                        else
                        {
                            ESP_LOGW(TAG, "ESPNOW message send status: %d", sendStatus);
                            break;
                        }
                    } while (++sendAttempt < maxAttempts && !sendSuccess);

                    // Report the concluded send back to the TCP client: one
                    // report per forwarded frame, after the whole retry cycle.
                    {
                        uint8_t attempts = sendAttempt;
                        if (sendStatus != ESP_OK)
                            attempts = sendAttempt + 1; // broke before the ++
                        sendDeliveryReport(sendAddress, packet->function,
                                           sendSuccess != 0, attempts > 0 ? attempts - 1 : 0);
                    }

                    if (sendSuccess)
                        peerAcked(sendAddress);
                    else if (sendStatus == ESP_OK)
                        peerSendFailed(sendAddress);

                    // Per-frame send log: what was sent, to whom, the radio
                    // outcome, and how long the retry cycle held the queue.
                    {
                        uint32_t elapsedMs = (xTaskGetTickCount() - sendStart) * portTICK_PERIOD_MS;
                        const char *fn = espnowFuncName(packet->function);
                        char fnBuf[8];
                        if (fn == NULL)
                        {
                            snprintf(fnBuf, sizeof(fnBuf), "0x%04X", packet->function);
                            fn = fnBuf;
                        }
                        ESP_LOGI(TAG, "ESPNOW %s to %02X:%02X:%02X:%02X:%02X:%02X: %s in %lums (%d attempt%s%s)",
                                 fn, sendAddress[0], sendAddress[1], sendAddress[2],
                                 sendAddress[3], sendAddress[4], sendAddress[5],
                                 sendSuccess ? "acked" : "FAILED", (unsigned long)elapsedMs,
                                 sendAttempt, sendAttempt == 1 ? "" : "s",
                                 probing ? ", probe" : "");
                    }
                    break;
                }
                }
            }

            vRingbufferReturnItem(buffers->read, (void *)packet);
        }
    }
}
