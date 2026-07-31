/*
 * Persist a 32-byte secret in the last flash sector so the iroh endpoint id
 * survives restarts.
 *
 * Reads are plain memory-mapped XIP reads. Writing stalls XIP, so the store
 * must run while nothing else executes from flash: call it on core 0, before
 * core 1 (display scanout) is launched, and before the WiFi/lwIP tasks start.
 * Interrupts are disabled for the duration of the erase/program; the Pico SDK
 * flash routines themselves run from SRAM. In the intended flow the store
 * happens at most once per device, on first boot.
 */
#include <stdint.h>
#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"

#define SECRET_SIZE 32u
#define SECRET_MAGIC "pico-std-secret1"
#define SECRET_MAGIC_SIZE 16u
#define SECRET_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

int presto_secret_load(uint8_t out[SECRET_SIZE])
{
    const uint8_t *stored = (const uint8_t *)(XIP_BASE + SECRET_FLASH_OFFSET);
    if (memcmp(stored, SECRET_MAGIC, SECRET_MAGIC_SIZE) != 0) return -1;
    memcpy(out, stored + SECRET_MAGIC_SIZE, SECRET_SIZE);
    return 0;
}

int presto_secret_store(const uint8_t secret[SECRET_SIZE])
{
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xff, sizeof(page));
    memcpy(page, SECRET_MAGIC, SECRET_MAGIC_SIZE);
    memcpy(page + SECRET_MAGIC_SIZE, secret, SECRET_SIZE);

    uint32_t interrupt_state = save_and_disable_interrupts();
    flash_range_erase(SECRET_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(SECRET_FLASH_OFFSET, page, sizeof(page));
    restore_interrupts(interrupt_state);

    uint8_t check[SECRET_SIZE];
    if (presto_secret_load(check) != 0) return -1;
    if (memcmp(check, secret, SECRET_SIZE) != 0) return -2;
    return 0;
}
