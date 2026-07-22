#include <stdio.h>
#include <stdint.h>
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

void presto_wifi_countdown(void)
{
    for (int seconds = 15; seconds != 0; --seconds) {
        printf("Wi-Fi task starts in %d\n", seconds);
        stdio_flush();
        sleep_ms(1000);
    }
}

int presto_wifi_connect(const char *ssid, const char *password)
{
    printf("Initializing RM2/CYW43439...\n");
    stdio_flush();
    int result = cyw43_arch_init();
    if (result != 0) {
        printf("Wi-Fi initialization failed: %d\n", result);
        stdio_flush();
        return result;
    }

    cyw43_arch_enable_sta_mode();
    printf("Connecting to Wi-Fi network: %s\n", ssid);
    stdio_flush();
    result = cyw43_arch_wifi_connect_timeout_ms(
        ssid, password, CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (result != 0) {
        printf("Wi-Fi connection failed: %d\n", result);
        stdio_flush();
        return result;
    }

    const ip4_addr_t *address = netif_ip4_addr(netif_default);
    printf("Wi-Fi connected; IPv4 address: %s\n", ip4addr_ntoa(address));
    stdio_flush();
    return 0;
}

void presto_wifi_ipv4_octets(uint8_t octets[4])
{
    const ip4_addr_t *address = netif_ip4_addr(netif_default);
    octets[0] = ip4_addr1(address);
    octets[1] = ip4_addr2(address);
    octets[2] = ip4_addr3(address);
    octets[3] = ip4_addr4(address);
}
