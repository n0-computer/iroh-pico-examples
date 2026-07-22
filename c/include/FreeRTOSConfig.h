/*
 * FreeRTOS config for the RP2350 (Cortex-M33) ARM_NTZ port.
 *
 * Based on the Raspberry Pi pico-examples FreeRTOSConfig_examples_common.h
 * (MIT licensed), trimmed for this std-on-RP2350 proof of concept:
 *   - single core (configNUMBER_OF_CORES = 1)
 *   - dynamic allocation only (Heap4), so no application-provided static
 *     idle/timer task memory is required.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Scheduler Related */
#define configUSE_PREEMPTION                    1
#define configUSE_TICKLESS_IDLE                 0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )
#define configMAX_PRIORITIES                    32
#define configMINIMAL_STACK_SIZE                ( configSTACK_DEPTH_TYPE ) 512
#define configUSE_16_BIT_TICKS                  0

#define configIDLE_SHOULD_YIELD                 1

/* Synchronization Related */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_APPLICATION_TASK_TAG          0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_QUEUE_SETS                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     1
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 5

/* System */
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* Memory allocation related definitions.
 * The esp-idf locks glue requires static allocation. We also let the kernel
 * provide the idle/timer task static memory (configKERNEL_PROVIDED_STATIC_MEMORY)
 * so no application-supplied vApplicationGet*TaskMemory() hooks are needed. */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configKERNEL_PROVIDED_STATIC_MEMORY     1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (128*1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* Hook function related definitions. */
#define configCHECK_FOR_STACK_OVERFLOW          0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/* Run time and task stats gathering related definitions. */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    0

/* Co-routine related definitions. */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* Software timer related definitions. */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            1024

/* Start with a single FreeRTOS scheduler core. This keeps core 1 available for
 * explicit multicore work and avoids making basic std startup depend on the SMP
 * core-launch and doorbell path. */
#if FREE_RTOS_KERNEL_SMP /* set by the RP2xxx SMP port of FreeRTOS */
#ifndef configNUMBER_OF_CORES
#define configNUMBER_OF_CORES                   1
#endif
#define configNUM_CORES                         configNUMBER_OF_CORES
#define configTICK_CORE                         0
#define configRUN_MULTIPLE_PRIORITIES           1
#if configNUMBER_OF_CORES > 1
#define configUSE_CORE_AFFINITY                 1
#endif
#define configUSE_PASSIVE_IDLE_HOOK             0
#endif

/* Pico SDK integration */
#define configSUPPORT_PICO_SYNC_INTEROP         1
#define configSUPPORT_PICO_TIME_INTEROP         1
/* Let the FreeRTOS RP2350 port define the Pico SDK's isr_* symbols directly.
 * This avoids a second layer of RAM-vector replacement during scheduler start. */
#define configUSE_DYNAMIC_EXCEPTION_HANDLERS    0

#include <assert.h>
/* Define to trap errors during development. */
#define configASSERT(x)                         assert(x)

/* Set the following definitions to 1 to include the API function, or zero
to exclude the API function. */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_xTaskResumeFromISR              1
#define INCLUDE_xQueueGetMutexHolder            1

/* Cortex-M33 (RP2350) port specifics. */
#if PICO_RP2350
#define configENABLE_MPU                        0
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          1
/* This proof of concept builds entirely soft-float (the Rust target is
 * soft-float and the C flags select no FPU), so the port must not save/restore
 * VFP context. Revisit for on-device hardware-FPU use. */
#define configENABLE_FPU                        0
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    16
#endif

#endif /* FREERTOS_CONFIG_H */
