#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

// Starts SNTP time synchronization and the task that periodically sends
// MSP_ELRS_BACKPACK_SET_RTC packets to the bound backpack(s) over ESPNOW.
// espnow_out is the ring buffer drained by the ESPNOW server task.
// Also registers the 'timeconfig' console command; call before the console
// REPL is started.
void rtc_sync_start(RingbufHandle_t espnow_out);

// Request an immediate time send, e.g. when a goggle backpack has just
// booted. Safe to call from any task context. A no-op until the clock has
// been synchronized.
void rtc_sync_send_now(void);

// Seed the local clock from an MSP_ELRS_BACKPACK_SET_RTC payload received
// from the TCP client (e.g. dd-pits). The external sender is treated as
// authoritative: periodic SNTP-based broadcasts pause while it keeps
// sending, so the two sources never fight over the goggle clocks.
void rtc_sync_external_time(const uint8_t *payload, uint16_t size);
