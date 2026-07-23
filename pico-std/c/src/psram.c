/*
 * RP2350 PSRAM initialization.
 *
 * Adapted from MicroPython's MIT-licensed ports/rp2/rp2_psram.c:
 * Copyright (c) 2025 Phil Howard, Mike Bell and Kirk D. Benell.
 */

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico/platform.h"

static volatile int init_status;

#define PSRAM_WAIT_WHILE(condition, error_code) do { \
    uint32_t timeout = 10000000u; \
    while ((condition) && --timeout != 0) { \
    } \
    if (timeout == 0) { \
        init_status = (error_code); \
        qmi_hw->direct_csr = 0; \
        return 0; \
    } \
} while (0)

static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
    /*
     * The caller may have just branched here from flash. Let the final XIP
     * transaction and prefetch complete before taking ownership of QMI direct
     * mode. This matters for larger images even with interrupts disabled.
     */
    PSRAM_WAIT_WHILE(
        qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS, -10);

    qmi_hw->direct_csr =
        30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    PSRAM_WAIT_WHILE(
        qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS, -11);

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx =
        QMI_DIRECT_TX_OE_BITS |
        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
        0xf5u;
    PSRAM_WAIT_WHILE(
        qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS, -12);
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;
    for (size_t i = 0; i < 7; ++i) {
        qmi_hw->direct_tx = i == 0 ? 0x9fu : 0xffu;
        PSRAM_WAIT_WHILE(
            !(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS), -13);
        PSRAM_WAIT_WHILE(
            qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS, -14);
        uint8_t value = qmi_hw->direct_rx;
        if (i == 5) kgd = value;
        if (i == 6) eid = value;
    }

    qmi_hw->direct_csr &=
        ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
    if (kgd != 0x5d) {
        init_status = -15;
        return 0;
    }

    size_t size = 1024u * 1024u;
    uint8_t size_id = eid >> 5;
    if (eid == 0x26 || size_id == 2) return size * 8u;
    if (size_id == 0) return size * 2u;
    if (size_id == 1) return size * 4u;
    return size;
}

static size_t detected_size;

static size_t __no_inline_not_in_flash_func(psram_init)(void) {
    const uint cs_pin = 47;
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    uint32_t interrupt_state = save_and_disable_interrupts();
    size_t size = psram_detect();
    if (size == 0) {
        restore_interrupts(interrupt_state);
        return 0;
    }

    const int clock_hz = clock_get_hz(clk_sys);
    if (clock_hz != 200000000) {
        init_status = -20;
        restore_interrupts(interrupt_state);
        return 0;
    }

    /*
     * Presto runs clk_sys at 200 MHz. These are the exact results of the
     * MicroPython timing formulas at that frequency. Keeping them constant
     * avoids calling a final-link-dependent 64-bit division implementation
     * from the SRAM-resident QMI transition.
     */
    const int divisor = 2;
    const int rxdelay = 2;
    const int max_select = 25;
    const int min_deselect = 3;

    qmi_hw->direct_csr =
        10u << QMI_DIRECT_CSR_CLKDIV_LSB |
        QMI_DIRECT_CSR_EN_BITS |
        QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    uint32_t timeout = 10000000u;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) &&
           --timeout != 0) {
    }
    if (timeout == 0) {
        init_status = -21;
        qmi_hw->direct_csr = 0;
        restore_interrupts(interrupt_state);
        return 0;
    }
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u;
    timeout = 10000000u;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) &&
           --timeout != 0) {
    }
    if (timeout == 0) {
        init_status = -22;
        qmi_hw->direct_csr = 0;
        restore_interrupts(interrupt_state);
        return 0;
    }

    qmi_hw->m[1].timing =
        1u << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        (uint32_t)max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
        (uint32_t)min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
        (uint32_t)rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
        (uint32_t)divisor << QMI_M1_TIMING_CLKDIV_LSB;

    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
        6u << QMI_M0_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xeb;

    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;

    qmi_hw->direct_csr = 0;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    restore_interrupts(interrupt_state);
    return size;
}

void __no_inline_not_in_flash_func(presto_psram_early_init)(void) {
    init_status = 0;
    detected_size = psram_init();
    if (detected_size != 0) init_status = 1;
}

size_t presto_psram_size(void) {
    return detected_size;
}

int presto_psram_init_status(void) {
    return init_status;
}

#ifdef PICO_STD_PSRAM_HEAP

#define PSRAM_BASE ((uintptr_t)0x11000000u)

static uintptr_t psram_heap_end;

/*
 * Newlib malloc is Rust std's system allocator on this target. Override Pico
 * SDK's weak internal-SRAM _sbrk so only that heap moves to external PSRAM;
 * FreeRTOS task stacks and kernel objects remain in the internal Heap4 arena.
 */
void *_sbrk(int increment) {
    uintptr_t heap_start = PSRAM_BASE;
    uintptr_t heap_limit = PSRAM_BASE + detected_size;
    uintptr_t current = psram_heap_end != 0 ? psram_heap_end : heap_start;
    uintptr_t next;

    if (detected_size == 0) {
        errno = ENOMEM;
        return (void *)-1;
    }

    if (increment >= 0) {
        uintptr_t amount = (uintptr_t)increment;
        if (amount > heap_limit - current) {
            errno = ENOMEM;
            return (void *)-1;
        }
        next = current + amount;
    } else {
        uintptr_t amount = (uintptr_t)(-(int64_t)increment);
        if (amount > current - heap_start) {
            errno = EINVAL;
            return (void *)-1;
        }
        next = current - amount;
    }

    psram_heap_end = next;
    return (void *)current;
}

size_t presto_psram_heap_free(void) {
    uintptr_t current =
        psram_heap_end != 0 ? psram_heap_end : PSRAM_BASE;
    return PSRAM_BASE + detected_size - current;
}

#endif
