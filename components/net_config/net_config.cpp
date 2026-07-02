#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_console.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "net_config.h"

#define STORAGE_NAMESPACE "netpack"
#define STORAGE_STATIC_KEY "net_static"
#define STORAGE_IP_KEY "net_ip"
#define STORAGE_NETMASK_KEY "net_mask"
#define STORAGE_GATEWAY_KEY "net_gw"

static const char *TAG = "net_config";

static void net_config_defaults(net_config_t *cfg)
{
#ifdef CONFIG_USE_STATIC_IP
    cfg->use_static = true;
#else
    cfg->use_static = false;
#endif
    strlcpy(cfg->ip, CONFIG_STATIC_IP_ADDR, sizeof(cfg->ip));
    strlcpy(cfg->netmask, CONFIG_STATIC_NETMASK, sizeof(cfg->netmask));
    strlcpy(cfg->gateway, CONFIG_STATIC_GATEWAY, sizeof(cfg->gateway));
}

void net_config_load(net_config_t *cfg)
{
    net_config_defaults(cfg);

    nvs_handle_t handle;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return;

    uint8_t use_static;
    if (nvs_get_u8(handle, STORAGE_STATIC_KEY, &use_static) == ESP_OK)
        cfg->use_static = use_static != 0;

    size_t len = sizeof(cfg->ip);
    nvs_get_str(handle, STORAGE_IP_KEY, cfg->ip, &len);
    len = sizeof(cfg->netmask);
    nvs_get_str(handle, STORAGE_NETMASK_KEY, cfg->netmask, &len);
    len = sizeof(cfg->gateway);
    nvs_get_str(handle, STORAGE_GATEWAY_KEY, cfg->gateway, &len);

    nvs_close(handle);
}

esp_err_t net_config_save(const net_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return err;
    }

    if ((err = nvs_set_u8(handle, STORAGE_STATIC_KEY, cfg->use_static ? 1 : 0)) == ESP_OK &&
        (err = nvs_set_str(handle, STORAGE_IP_KEY, cfg->ip)) == ESP_OK &&
        (err = nvs_set_str(handle, STORAGE_NETMASK_KEY, cfg->netmask)) == ESP_OK &&
        (err = nvs_set_str(handle, STORAGE_GATEWAY_KEY, cfg->gateway)) == ESP_OK)
        err = nvs_commit(handle);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "Failed to save network config (%s)", esp_err_to_name(err));

    nvs_close(handle);
    return err;
}

static void print_config(void)
{
    net_config_t cfg;
    net_config_load(&cfg);

    printf("Configured mode: %s\n", cfg.use_static ? "static" : "dhcp");
    if (cfg.use_static)
    {
        printf("Configured IP:   %s\n", cfg.ip);
        printf("Netmask:         %s\n", cfg.netmask);
        printf("Gateway:         %s\n", cfg.gateway);
    }

    esp_netif_t *eth_netif = esp_netif_get_handle_from_ifkey("ETH_0");
    esp_netif_ip_info_t ip_info;
    if (eth_netif != NULL && esp_netif_get_ip_info(eth_netif, &ip_info) == ESP_OK)
        printf("Current IP:      " IPSTR "\n", IP2STR(&ip_info.ip));

    printf("Hostname:        elrs-netpack.local\n");
}

static int cmd_netconfig(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "show") == 0)
    {
        print_config();
        return 0;
    }

    net_config_t cfg;
    net_config_load(&cfg);

    if (strcmp(argv[1], "dhcp") == 0)
    {
        cfg.use_static = false;
        if (net_config_save(&cfg) != ESP_OK)
            return 1;
        printf("Saved: DHCP mode. Type 'reboot' to apply.\n");
        return 0;
    }

    if (strcmp(argv[1], "static") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: netconfig static <ip> [netmask] [gateway]\n");
            return 1;
        }

        esp_ip4_addr_t ip, netmask, gateway;
        if (esp_netif_str_to_ip4(argv[2], &ip) != ESP_OK)
        {
            printf("Invalid IP address: %s\n", argv[2]);
            return 1;
        }

        const char *netmask_str = argc > 3 ? argv[3] : "255.255.255.0";
        if (esp_netif_str_to_ip4(netmask_str, &netmask) != ESP_OK)
        {
            printf("Invalid netmask: %s\n", netmask_str);
            return 1;
        }

        char gateway_buf[16];
        if (argc > 4)
        {
            if (esp_netif_str_to_ip4(argv[4], &gateway) != ESP_OK)
            {
                printf("Invalid gateway: %s\n", argv[4]);
                return 1;
            }
            strlcpy(gateway_buf, argv[4], sizeof(gateway_buf));
        }
        else
        {
            // Default to the first host of the subnet, e.g. 192.168.1.1 for a /24
            gateway.addr = (ip.addr & netmask.addr) | esp_netif_htonl(1);
            snprintf(gateway_buf, sizeof(gateway_buf), IPSTR, IP2STR(&gateway));
        }

        cfg.use_static = true;
        strlcpy(cfg.ip, argv[2], sizeof(cfg.ip));
        strlcpy(cfg.netmask, netmask_str, sizeof(cfg.netmask));
        strlcpy(cfg.gateway, gateway_buf, sizeof(cfg.gateway));
        if (net_config_save(&cfg) != ESP_OK)
            return 1;
        printf("Saved: static IP %s, netmask %s, gateway %s. Type 'reboot' to apply.\n",
               cfg.ip, cfg.netmask, cfg.gateway);
        return 0;
    }

    printf("Usage:\n"
           "  netconfig                                 Show current settings\n"
           "  netconfig dhcp                            Use DHCP (default)\n"
           "  netconfig static <ip> [netmask] [gateway] Use a static IP address\n");
    return 1;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("Rebooting...\n");
    esp_restart();
    return 0;
}

void net_config_start_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "netpack>";
    repl_config.max_cmdline_length = 128;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#endif

    esp_console_register_help_command();

    const esp_console_cmd_t netconfig_cmd = {
        .command = "netconfig",
        .help = "Show or set network settings: netconfig | netconfig dhcp | netconfig static <ip> [netmask] [gateway]",
        .hint = NULL,
        .func = &cmd_netconfig,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&netconfig_cmd));

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Reboot the device to apply saved network settings",
        .hint = NULL,
        .func = &cmd_reboot,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Console started. Type 'help' for commands, 'netconfig' to set network details.");
}
