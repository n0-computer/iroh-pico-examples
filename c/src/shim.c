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

extern char __StackLimit;

static void fatal_raw(const char *message)
{
    stdio_puts_raw(message);
    stdio_puts_raw("\n");
    stdio_flush();
}

void vApplicationMallocFailedHook(void)
{
    fatal_raw("FATAL: FreeRTOS heap allocation failed");
    while (true) tight_loop_contents();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void) task;
    fatal_raw("FATAL: FreeRTOS task stack overflow");
    if (name != NULL) {
        stdio_puts_raw("task: ");
        stdio_puts_raw(name);
        stdio_puts_raw("\n");
        stdio_flush();
    }
    while (true) tight_loop_contents();
}

static uint32_t fault_register(uintptr_t address)
{
    return *(volatile uint32_t *) address;
}

static void fault_hex(const char *label, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char output[9];
    for (unsigned i = 0; i < 8; ++i) {
        output[i] = digits[(value >> (28 - 4 * i)) & 0xf];
    }
    output[8] = '\0';
    stdio_puts_raw(label);
    stdio_puts_raw(output);
    stdio_puts_raw("\n");
}

__attribute__((used, noreturn))
static void hardfault_report(uint32_t *frame, uint32_t exc_return)
{
    /* Armv8-M Secure SCB fault registers. Other configurable faults are routed
     * to HardFault by the Pico startup vector table, so CFSR holds the useful
     * reason even though there is only one installed handler. */
    uint32_t cfsr = fault_register(0xe000ed28u);
    uint32_t hfsr = fault_register(0xe000ed2cu);
    uint32_t mmfar = fault_register(0xe000ed34u);
    uint32_t bfar = fault_register(0xe000ed38u);

    fatal_raw("FATAL: Cortex-M HardFault");
    fault_hex("FAULT exc_return=", exc_return);
    fault_hex("FAULT frame=", (uint32_t) (uintptr_t) frame);
    fault_hex("FAULT CFSR=", cfsr);
    fault_hex("FAULT HFSR=", hfsr);
    fault_hex("FAULT MMFAR=", mmfar);
    fault_hex("FAULT BFAR=", bfar);

    /* Avoid causing a second fault if exception entry itself failed to stack a
     * frame. RP2350 SRAM occupies 0x20000000..0x20082000. */
    uintptr_t frame_address = (uintptr_t) frame;
    if ((frame_address & 3u) == 0 && frame_address >= 0x20000000u &&
        frame_address <= 0x20081fe0u) {
        fault_hex("FAULT r0=", frame[0]);
        fault_hex("FAULT r1=", frame[1]);
        fault_hex("FAULT r2=", frame[2]);
        fault_hex("FAULT r3=", frame[3]);
        fault_hex("FAULT r12=", frame[4]);
        fault_hex("FAULT lr=", frame[5]);
        fault_hex("FAULT pc=", frame[6]);
        fault_hex("FAULT xpsr=", frame[7]);
    } else {
        stdio_puts_raw("FAULT exception frame is outside SRAM\n");
    }
    stdio_flush();
    while (true) tight_loop_contents();
}

/* Select the stack active before exception entry. FreeRTOS tasks use PSP while
 * handler mode uses MSP. Keep this wrapper naked so its prologue cannot obscure
 * the exception frame we are trying to inspect. */
__attribute__((naked)) void isr_hardfault(void)
{
    __asm volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mov r1, lr\n"
        "b hardfault_report\n");
}

void presto_memory_report(const char *label)
{
    struct mallinfo info = mallinfo();
    char *brk = sbrk(0);
    ptrdiff_t unclaimed = &__StackLimit - brk;
    size_t stack_free = uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);

    printf("MEM %s: newlib-used=%ld newlib-free-chunks=%ld "
           "newlib-unclaimed=%ld freertos-free=%lu freertos-min=%lu "
           "task-stack-min-free=%lu\n",
           label,
           (long) info.uordblks,
           (long) info.fordblks,
           (long) unclaimed,
           (unsigned long) xPortGetFreeHeapSize(),
           (unsigned long) xPortGetMinimumEverFreeHeapSize(),
           (unsigned long) stack_free);
    stdio_flush();
}

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
        if (size != 0) fatal_raw("FATAL: Newlib aligned allocation failed");
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
