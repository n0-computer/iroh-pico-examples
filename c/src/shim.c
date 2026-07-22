#include <unistd.h>
#include <stdlib.h>
#include <malloc.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "unwind.h"
#include "pico/time.h"
#include "hardware/timer.h"
#include "FreeRTOS.h"
#include "task.h"

void usb_smoke_test(void)
{
    while (true) {
        printf("USB smoke test before FreeRTOS\n");
        stdio_flush();
        sleep_ms(1000);
    }
}

/* Bring up each layer separately and report the last successful step over USB.
 * This deliberately never enters Rust; it distinguishes pthread initialization
 * from task creation and the RP2350 FreeRTOS scheduler/exception setup. */
extern void esp_newlib_locks_init(void);
extern int esp_pthread_init(void);

static void rtos_smoke_task(void *unused)
{
    (void) unused;
    printf("FreeRTOS task started; initializing newlib locks\n");
    stdio_flush();
    esp_newlib_locks_init();
    printf("newlib locks OK; initializing pthreads\n");
    stdio_flush();
    int pthread_result = esp_pthread_init();
    printf("pthread init returned %d\n", pthread_result);
    stdio_flush();

    while (true) {
        printf("FreeRTOS task is running\n");
        stdio_flush();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void rtos_smoke_test(void)
{
    /* Give the host and terminal time to attach before advancing. */
    for (int i = 15; i != 0; --i) {
        printf("RTOS probe starts in %d\n", i);
        stdio_flush();
        sleep_ms(1000);
    }

    printf("creating task before lock initialization\n");
    stdio_flush();
    BaseType_t task_result = xTaskCreate(rtos_smoke_task, "rtos_probe", 1024,
                                         NULL, 2, NULL);
    printf("task creation returned %ld; starting scheduler\n", (long) task_result);
    stdio_flush();
    vTaskStartScheduler();

    while (true) tight_loop_contents();
}

int usleep(useconds_t us)
{
    absolute_time_t start_time = get_absolute_time();

    TickType_t ticks = us / (1000 * portTICK_PERIOD_MS);
    absolute_time_t end_time = delayed_by_us(start_time, us - 1000 * portTICK_PERIOD_MS * ticks);

    vTaskDelay(ticks);

    busy_wait_until(end_time);

    return 0;
}

char *realpath(const char *path, char *resolved_path)
{
    /* There is no filesystem */
    errno = ENOENT;
    return NULL;
}

/* Interface to a macro */
/* See https://stackoverflow.com/a/1952823 */
_Unwind_Word (_Unwind_GetIP)(struct _Unwind_Context *context)
{
    return _Unwind_GetIP(context);
}

/* Rust's std unix allocator calls posix_memalign(), which this newlib build
 * does not provide. Implement it on top of memalign(). */
int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *p = memalign(alignment, size);
    if (p == NULL) {
        return ENOMEM;
    }
    *memptr = p;
    return 0;
}

/* ESP-IDF's atomic compare-and-set, used by pthread_once(). It is not provided
 * by the Pico SDK, so implement it with a GCC atomic builtin (the Cortex-M33
 * has native exclusive load/store). Returns true if the value was updated. */
bool esp_cpu_compare_and_set(volatile uint32_t *addr, uint32_t compare, uint32_t set)
{
    return __atomic_compare_exchange_n(addr, &compare, set, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* ESP-IDF's pthread_create applies its configured core affinity after creating
 * every task. FreeRTOS omits that API entirely in a one-core build; affinity is
 * necessarily core 0, so the compatible implementation is a no-op. */
#if configNUMBER_OF_CORES == 1
void vTaskCoreAffinitySet(TaskHandle_t task, UBaseType_t affinity_mask)
{
    (void) task;
    (void) affinity_mask;
}
#endif
