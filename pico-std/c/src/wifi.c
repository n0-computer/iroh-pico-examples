#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include "pico/cyw43_arch.h"
#include "lwip/apps/sntp.h"
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
    const int max_attempts = 5;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        printf("Connecting to Wi-Fi network: %s (attempt %d/%d)\n",
               ssid, attempt, max_attempts);
        stdio_flush();

        result = cyw43_arch_wifi_connect_timeout_ms(
            ssid, password, CYW43_AUTH_WPA2_AES_PSK, 30000);
        if (result == 0) break;

        printf("Wi-Fi connection attempt %d failed: %d\n", attempt, result);
        stdio_flush();
        if (attempt != max_attempts) sleep_ms(2000);
    }
    if (result != 0) {
        printf("Wi-Fi connection failed after %d attempts\n", max_attempts);
        stdio_flush();
        return result;
    }

    const ip4_addr_t *address = netif_ip4_addr(netif_default);
    printf("Wi-Fi connected; IPv4 address: %s\n", ip4addr_ntoa(address));
    stdio_flush();
    return 0;
}

int presto_time_sync(void)
{
    printf("Synchronizing wall clock with SNTP...\n");
    stdio_flush();
    ip_addr_t ntp_server;
    if (!ipaddr_aton("162.159.200.1", &ntp_server)) {
        printf("Invalid built-in NTP server address\n");
        stdio_flush();
        return -1;
    }
    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setserver(0, &ntp_server);
    sntp_init();
    cyw43_arch_lwip_end();

    struct timespec now;
    for (int attempt = 0; attempt < 300; ++attempt) {
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec >= 1577836800) {
            cyw43_arch_lwip_begin();
            sntp_stop();
            cyw43_arch_lwip_end();
            printf("Wall clock synchronized: Unix %lld\n", (long long)now.tv_sec);
            stdio_flush();
            return 0;
        }
        sleep_ms(100);
    }

    printf("SNTP synchronization timed out\n");
    stdio_flush();
    return -1;
}

void presto_wifi_ipv4_octets(uint8_t octets[4])
{
    const ip4_addr_t *address = netif_ip4_addr(netif_default);
    octets[0] = ip4_addr1(address);
    octets[1] = ip4_addr2(address);
    octets[2] = ip4_addr3(address);
    octets[3] = ip4_addr4(address);
}
