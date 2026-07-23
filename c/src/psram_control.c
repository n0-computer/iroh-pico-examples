#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"

#define PSRAM_BASE 0x11000000u
#define PSRAM_EXPECTED_SIZE (8u * 1024u * 1024u)

void presto_psram_early_init(void);
size_t presto_psram_size(void);
int presto_psram_init_status(void);

static bool check_word(volatile uint32_t *address, uint32_t value,
                       const char *label)
{
    *address = value;
    uint32_t actual = *address;
    printf("%s @ %p: wrote %08lx, read %08lx: %s\n",
           label, (void *)address, (unsigned long)value, (unsigned long)actual,
           actual == value ? "PASS" : "FAIL");
    stdio_flush();
    return actual == value;
}

int main(void)
{
    /*
     * Bring USB up first so a failure inside QMI setup does not look like a
     * completely dead image. PSRAM initialization disables interrupts while
     * flash is unavailable in QMI direct mode.
     */
    stdio_init_all();
    for (int seconds = 10; seconds != 0; --seconds) {
        printf("Pure C PSRAM probe starts in %d\n", seconds);
        stdio_flush();
        sleep_ms(1000);
    }

    puts("C PSRAM: entering QMI initialization");
    stdio_flush();
    presto_psram_early_init();

    size_t size = presto_psram_size();
    int status = presto_psram_init_status();
    printf("C PSRAM: QMI returned status=%d size=%lu\n",
           status, (unsigned long)size);
    stdio_flush();

    bool passed = status == 1 && size == PSRAM_EXPECTED_SIZE;
    if (size >= 3 * sizeof(uint32_t)) {
        volatile uint32_t *first = (volatile uint32_t *)PSRAM_BASE;
        volatile uint32_t *middle =
            (volatile uint32_t *)(PSRAM_BASE + size / 2);
        volatile uint32_t *last =
            (volatile uint32_t *)(PSRAM_BASE + size - sizeof(uint32_t));
        passed &= check_word(first, 0x13579bdfu, "first");
        passed &= check_word(middle, 0xa5a55a5au, "middle");
        passed &= check_word(last, 0x2468ace0u, "last");
    } else {
        passed = false;
    }

    while (true) {
        printf("Pure C PSRAM probe: %s (status=%d size=%lu)\n",
               passed ? "PASS" : "FAIL", status, (unsigned long)size);
        stdio_flush();
        sleep_ms(1000);
    }
}
