#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        bool use_static;
        char ip[16];
        char netmask[16];
        char gateway[16];
    } net_config_t;

    /* Load network settings from NVS, falling back to menuconfig defaults */
    void net_config_load(net_config_t *cfg);

    /* Persist network settings to NVS (applied on next boot) */
    esp_err_t net_config_save(const net_config_t *cfg);

    /* Start the serial console REPL exposing the netconfig/reboot commands */
    void net_config_start_console(void);

#ifdef __cplusplus
}
#endif
