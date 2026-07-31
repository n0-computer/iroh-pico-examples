/*
 * Minimal ST7701 scanout for Pimoroni Presto.
 *
 * The panel initialization and PIO timing are derived from Pimoroni's
 * MIT-licensed Presto driver. The basic smoke test repeats one RGB565 row.
 * PICO_STD_DISPLAY_PSRAM instead scans a full 480x480 framebuffer from PSRAM.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "hardware/structs/dma.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "st7701_parallel.pio.h"
#include "st7701_timing.pio.h"

#define LCD_WIDTH 480u
#define PANEL_WIDTH 480u
#define PANEL_HEIGHT 480u
#define FRAMEBUFFER_BYTES (LCD_WIDTH * PANEL_HEIGHT * sizeof(uint16_t))
#define PSRAM_BASE 0x11000000u
#define PSRAM_DMA_BASE \
    (XIP_NOCACHE_NOALLOC_BASE + (PSRAM_BASE - XIP_BASE))
#define PREFETCH_LINES 8u

#define LCD_D0 1u
#define LCD_HSYNC 19u
#define LCD_VSYNC 20u
#define LCD_DE 21u
#define LCD_DOT_CLK 22u
#define LCD_CLK 26u
#define LCD_DAT 27u
#define LCD_CS 28u
#define LCD_BACKLIGHT 45u

#define TIMING_V_PULSE 8u
#define TIMING_V_BACK (5u + TIMING_V_PULSE)
#define TIMING_V_DISPLAY (PANEL_HEIGHT + TIMING_V_BACK)
#define TIMING_V_FRONT (5u + TIMING_V_DISPLAY)
#define TIMING_H_FRONT 4u
#define TIMING_H_PULSE 16u
#define TIMING_H_BACK 40u
#define TIMING_H_DISPLAY PANEL_WIDTH

#define BACKLIGHT_PWM_TOP 6200u

static PIO display_pio = pio1;
static uint parallel_sm;
static uint timing_sm;
static uint parallel_offset;
static int pixel_dma;
static int line_dma;
#ifdef PICO_STD_DISPLAY_PSRAM
static int prefetch_dma;
#endif

static uint16_t timing_row;
static uint16_t timing_phase;
static uint16_t display_row;
#ifdef PICO_STD_DISPLAY_PSRAM
static uint16_t *dma_framebuffer;
static bool fill_framebuffer_on_start;
static uint16_t prefetched_lines[PREFETCH_LINES][LCD_WIDTH] __attribute__((aligned(4)));
#endif
static uint16_t sram_control_line[LCD_WIDTH] __attribute__((aligned(4)));
static uint16_t *volatile next_line_addr;

static volatile int display_status;
static volatile size_t detected_psram_size;
static volatile uint16_t psram_sample_first;
static volatile uint16_t psram_sample_middle;
static volatile uint16_t psram_sample_last;
static uint32_t core1_stack[1024] __attribute__((aligned(8)));

#ifdef PICO_STD_DISPLAY_PSRAM
size_t presto_psram_size(void);
int presto_psram_init_status(void);

static uint16_t *frame_line(unsigned row) {
    return dma_framebuffer + row * LCD_WIDTH;
}

static bool framebuffer_is_valid(uint16_t *framebuffer, size_t pixel_count) {
    uintptr_t address = (uintptr_t)framebuffer;
    uintptr_t psram_end = PSRAM_BASE + presto_psram_size();
    return pixel_count >= LCD_WIDTH * PANEL_HEIGHT &&
        address >= PSRAM_BASE &&
        address <= psram_end &&
        FRAMEBUFFER_BYTES <= psram_end - address &&
        (address & 3u) == 0;
}

static void __not_in_flash_func(prefetch_line_blocking)(unsigned row) {
    uint32_t *dst = (uint32_t *)prefetched_lines[row & (PREFETCH_LINES - 1u)];
    uint32_t *src = (uint32_t *)frame_line(row);
    for (unsigned i = 0; i < (LCD_WIDTH >> 1); ++i) {
        dst[i] = src[i];
    }
}

static void __not_in_flash_func(arm_prefetch_for_row)(unsigned row) {
    if (row >= PANEL_HEIGHT) {
        dma_channel_abort(prefetch_dma);
        return;
    }

    if (dma_channel_is_busy(prefetch_dma)) {
        dma_channel_abort(prefetch_dma);
        prefetch_line_blocking(row);
        return;
    }

    dma_channel_set_read_addr(prefetch_dma, frame_line(row), false);
    dma_channel_set_write_addr(prefetch_dma,
        prefetched_lines[row & (PREFETCH_LINES - 1u)], false);
    dma_channel_set_trans_count(prefetch_dma, LCD_WIDTH >> 1, true);
}
#endif

static void panel_command(uint8_t cmd, size_t len, const uint8_t *data) {
    uint16_t words[20];
    uint16_t command = cmd;

    gpio_put(LCD_CS, 0);
    spi_write16_blocking(spi1, &command, 1);
    for (size_t i = 0; i < len; ++i) {
        words[i] = 0x100u | data[i];
    }
    if (len != 0) {
        spi_write16_blocking(spi1, words, len);
    }
    gpio_put(LCD_CS, 1);
}

#define CMD0(cmd) panel_command((cmd), 0, NULL)
#define CMD(cmd, ...) do { \
    const uint8_t data[] = {__VA_ARGS__}; \
    panel_command((cmd), sizeof(data), data); \
} while (0)

static void panel_init(void) {
    pwm_config pwm = pwm_get_default_config();
    pwm_config_set_wrap(&pwm, BACKLIGHT_PWM_TOP);
    pwm_init(pwm_gpio_to_slice_num(LCD_BACKLIGHT), &pwm, true);
    gpio_set_function(LCD_BACKLIGHT, GPIO_FUNC_PWM);
    pwm_set_gpio_level(LCD_BACKLIGHT, 0);

    CMD0(0x01);
    sleep_ms(150);
    CMD(0xff, 0x77, 0x01, 0x00, 0x00, 0x10);
    CMD(0x36, 0x00);
    CMD(0xc0, 0x3b, 0x00);
    CMD(0xc1, 0x0d, 0x02);
    CMD(0xc2, 0x31, 0x01);
    CMD(0xcd, 0x08);
    CMD(0xb0, 0x00, 0x11, 0x18, 0x0e, 0x11, 0x06, 0x07, 0x08,
              0x07, 0x22, 0x04, 0x12, 0x0f, 0xaa, 0x31, 0x18);
    CMD(0xb1, 0x00, 0x11, 0x19, 0x0e, 0x12, 0x07, 0x08, 0x08,
              0x08, 0x22, 0x04, 0x11, 0x11, 0xa9, 0x32, 0x18);
    CMD(0xc3, 0x80, 0x2e, 0x0e);

    CMD(0xff, 0x77, 0x01, 0x00, 0x00, 0x11);
    CMD(0xb0, 0x60);
    CMD(0xb1, 0x32);
    CMD(0xb2, 0x07);
    CMD(0xb3, 0x80);
    CMD(0xb5, 0x49);
    CMD(0xb7, 0x85);
    CMD(0xb8, 0x21);
    CMD(0xc1, 0x78);
    CMD(0xc2, 0x78);

    CMD(0xe0, 0x00, 0x1b, 0x02);
    CMD(0xe1, 0x08, 0xa0, 0x00, 0x00, 0x07, 0xa0, 0x00, 0x00, 0x00, 0x44, 0x44);
    CMD(0xe2, 0x11, 0x11, 0x44, 0x44, 0xed, 0xa0, 0x00, 0x00, 0xec, 0xa0, 0x00, 0x00);
    CMD(0xe3, 0x00, 0x00, 0x11, 0x11);
    CMD(0xe4, 0x44, 0x44);
    CMD(0xe5, 0x0a, 0xe9, 0xd8, 0xa0, 0x0c, 0xeb, 0xd8, 0xa0,
              0x0e, 0xed, 0xd8, 0xa0, 0x10, 0xef, 0xd8, 0xa0);
    CMD(0xe6, 0x00, 0x00, 0x11, 0x11);
    CMD(0xe7, 0x44, 0x44);
    CMD(0xe8, 0x09, 0xe8, 0xd8, 0xa0, 0x0b, 0xea, 0xd8, 0xa0,
              0x0d, 0xec, 0xd8, 0xa0, 0x0f, 0xee, 0xd8, 0xa0);
    CMD(0xeb, 0x02, 0x00, 0xe4, 0xe4, 0x88, 0x00, 0x40);
    CMD(0xec, 0x3c, 0x00);
    CMD(0xed, 0xab, 0x89, 0x76, 0x54, 0x02, 0xff, 0xff, 0xff,
              0xff, 0xff, 0xff, 0x20, 0x45, 0x67, 0x98, 0xba);
    CMD(0x36, 0x00);

    CMD(0xff, 0x77, 0x01, 0x00, 0x00, 0x13);
    CMD(0xe5, 0xe4);
    CMD(0xff, 0x77, 0x01, 0x00, 0x00, 0x00);
    CMD(0x3a, 0x66);
    CMD0(0x21);
    sleep_ms(1);
    CMD0(0x11);
    sleep_ms(120);
    CMD0(0x29);
    sleep_ms(100);
    pwm_set_gpio_level(LCD_BACKLIGHT, BACKLIGHT_PWM_TOP);
}

static void __not_in_flash_func(start_line_transfer)(void) {
    hw_clear_bits(&display_pio->irq, 0x1);
    ++display_row;
#ifdef PICO_STD_DISPLAY_PSRAM
    if (display_row == PANEL_HEIGHT) {
        next_line_addr = NULL;
        dma_channel_abort(prefetch_dma);
        return;
    }

    next_line_addr =
        prefetched_lines[(display_row + 1u) & (PREFETCH_LINES - 1u)];
    arm_prefetch_for_row(display_row + (PREFETCH_LINES - 1u));
#else
    next_line_addr = display_row == PANEL_HEIGHT
        ? NULL
        : sram_control_line;
#endif
}

static void __not_in_flash_func(start_frame_transfer)(void) {
    hw_clear_bits(&display_pio->irq, 0x2);
    next_line_addr = NULL;
    dma_channel_abort(pixel_dma);
    dma_channel_abort(line_dma);
    pio_sm_set_enabled(display_pio, parallel_sm, false);
    pio_sm_clear_fifos(display_pio, parallel_sm);
    pio_sm_exec_wait_blocking(display_pio, parallel_sm, pio_encode_mov(pio_osr, pio_null));
    pio_sm_exec_wait_blocking(display_pio, parallel_sm, pio_encode_out(pio_null, 32));
    pio_sm_exec_wait_blocking(display_pio, parallel_sm, pio_encode_jmp(parallel_offset));
    pio_sm_set_enabled(display_pio, parallel_sm, true);
    display_row = 0;
#ifdef PICO_STD_DISPLAY_PSRAM
    dma_channel_abort(prefetch_dma);
    for (unsigned row = 0; row < PREFETCH_LINES; ++row) {
        prefetch_line_blocking(row);
    }
    next_line_addr = prefetched_lines[1];
    dma_channel_set_read_addr(pixel_dma, prefetched_lines[0], true);
#else
    next_line_addr = sram_control_line;
    dma_channel_set_read_addr(pixel_dma, sram_control_line, true);
#endif
}

static void __isr __not_in_flash_func(line_isr)(void) {
    if (display_pio->irq & 0x2) {
        start_frame_transfer();
    } else {
        start_line_transfer();
    }
}

static void __isr __not_in_flash_func(timing_isr)(void) {
    while (!pio_sm_is_tx_fifo_full(display_pio, timing_sm)) {
        uint32_t instruction;
        switch (timing_phase) {
        case 0:
            instruction = 0x4000b042u | ((TIMING_H_FRONT - 3u) << 16);
            if (timing_row >= TIMING_V_PULSE) instruction |= 0x80000000u;
            break;
        case 1:
            instruction = 0x0000b042u | ((TIMING_H_PULSE - 3u) << 16);
            if (timing_row >= TIMING_V_PULSE) instruction |= 0x80000000u;
            break;
        case 2:
            instruction = 0x40000000u | ((TIMING_H_BACK - 3u) << 16);
            if (timing_row >= TIMING_V_PULSE) instruction |= 0x80000000u;
            instruction |= timing_row >= TIMING_V_BACK && timing_row < TIMING_V_DISPLAY
                ? 0xd004u : 0xb042u;
            break;
        default:
            instruction = 0x40000000u | ((TIMING_H_DISPLAY - 3u) << 16);
            if (timing_row >= TIMING_V_PULSE) instruction |= 0x80000000u;
            if (timing_row == TIMING_V_DISPLAY) instruction |= 0xd001u;
            else if (timing_row >= TIMING_V_BACK - 1u && timing_row < TIMING_V_DISPLAY)
                instruction |= 0xd000u;
            else instruction |= 0xb042u;
            if (++timing_row >= TIMING_V_FRONT) timing_row = 0;
            break;
        }
        pio_sm_put(display_pio, timing_sm, instruction);
        timing_phase = (timing_phase + 1u) & 3u;
    }
}

static void fill_test_pattern(void) {
    static const uint16_t colours[] = {
        0xf800, 0xffe0, 0x07e0, 0x07ff, 0x001f, 0xf81f, 0xffff, 0x0000
    };
#ifdef PICO_STD_DISPLAY_PSRAM
    for (unsigned y = 0; y < PANEL_HEIGHT; ++y) {
        for (unsigned x = 0; x < LCD_WIDTH; ++x) {
            uint16_t colour = colours[x / (LCD_WIDTH / 8u)];
            /* Horizontal black lines prove that DMA is advancing through a
             * full framebuffer rather than repeating one scanline. */
            if (y % 60u == 0) colour = 0;
            dma_framebuffer[y * LCD_WIDTH + x] = colour;
        }
    }
    psram_sample_first = dma_framebuffer[0];
    psram_sample_middle =
        dma_framebuffer[(PANEL_HEIGHT / 2u + 1u) * LCD_WIDTH + LCD_WIDTH / 2u];
    psram_sample_last = dma_framebuffer[LCD_WIDTH * PANEL_HEIGHT - 1u];
#else
    for (unsigned x = 0; x < LCD_WIDTH; ++x) {
        sram_control_line[x] = colours[x / (LCD_WIDTH / 8u)];
    }
#endif
}

static void display_core1(void) {
#ifdef PICO_STD_DISPLAY_PSRAM
    if (fill_framebuffer_on_start) fill_test_pattern();
#else
    fill_test_pattern();
#endif

    display_pio = pio1;
    parallel_sm = pio_claim_unused_sm(display_pio, true);
    timing_sm = pio_claim_unused_sm(display_pio, true);
    uint timing_offset = pio_add_program(display_pio, &st7701_timing_program);
    parallel_offset = pio_add_program(display_pio, &st7701_parallel_program);

    spi_init(spi1, 8000000);
    gpio_set_function(LCD_CS, GPIO_FUNC_SIO);
    gpio_set_dir(LCD_CS, GPIO_OUT);
    gpio_put(LCD_CS, 1);
    gpio_set_function(LCD_DAT, GPIO_FUNC_SPI);
    gpio_set_function(LCD_CLK, GPIO_FUNC_SPI);
    spi_set_format(spi1, 9, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    pio_gpio_init(display_pio, LCD_HSYNC);
    pio_gpio_init(display_pio, LCD_VSYNC);
    pio_gpio_init(display_pio, LCD_DE);
    pio_gpio_init(display_pio, LCD_DOT_CLK);
    for (unsigned pin = 0; pin < 16; ++pin) pio_gpio_init(display_pio, LCD_D0 + pin);
    for (unsigned pin = 16; pin < 18; ++pin) {
        gpio_init(LCD_D0 + pin);
        gpio_set_dir(LCD_D0 + pin, GPIO_OUT);
        gpio_put(LCD_D0 + pin, false);
    }
    pio_sm_set_consecutive_pindirs(display_pio, parallel_sm, LCD_D0, 16, true);
    pio_sm_set_consecutive_pindirs(display_pio, parallel_sm, LCD_HSYNC, 4, true);

    uint32_t divider = (clock_get_hz(clk_sys) + 34000000u - 1u) / 34000000u;
    // A full-resolution RGB565 scanout consumes two PSRAM bytes per pixel.
    // Run the PSRAM variant near 31 Hz at Presto's 200 MHz clock. Slower
    // settings caused the ST7701 to lose vertical synchronization and repeat
    // rows, so additional QMI margin must come from buffering rather than a
    // lower panel dot clock. The repeated-SRAM-row variant remains near 63 Hz.
#ifdef PICO_STD_DISPLAY_PSRAM
    if (divider < 30u) divider = 30u;
#else
    if (divider < 12u) divider = 12u;
#endif
    if (divider & 1u) ++divider;
    pio_sm_config config = st7701_parallel_program_get_default_config(parallel_offset);
    sm_config_set_out_pins(&config, LCD_D0, 16);
    sm_config_set_sideset_pins(&config, LCD_DE);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&config, true, true, 32);
    sm_config_set_in_shift(&config, false, false, 32);
    sm_config_set_clkdiv(&config, divider >> 1);
    pio_sm_init(display_pio, parallel_sm, parallel_offset, &config);
    pio_sm_exec(display_pio, parallel_sm, pio_encode_out(pio_y, 32));
    pio_sm_put(display_pio, parallel_sm, (LCD_WIDTH >> 1) - 1u);
    pio_sm_set_enabled(display_pio, parallel_sm, true);

    config = st7701_timing_program_get_default_config(timing_offset);
    sm_config_set_out_pins(&config, LCD_HSYNC, 2);
    sm_config_set_sideset_pins(&config, LCD_DOT_CLK);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&config, false, true, 32);
    sm_config_set_clkdiv(&config, divider);
    pio_sm_init(display_pio, timing_sm, timing_offset, &config);
    pio_sm_set_enabled(display_pio, timing_sm, true);

    pixel_dma = dma_claim_unused_channel(true);
    line_dma = dma_claim_unused_channel(true);
#ifdef PICO_STD_DISPLAY_PSRAM
    prefetch_dma = dma_claim_unused_channel(true);
#endif
    dma_channel_config dma = dma_channel_get_default_config(pixel_dma);
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_dreq(&dma, pio_get_dreq(display_pio, parallel_sm, true));
    channel_config_set_bswap(&dma, true);
    channel_config_set_chain_to(&dma, line_dma);
    channel_config_set_high_priority(&dma, true);
    dma_channel_configure(pixel_dma, &dma, &display_pio->txf[parallel_sm], NULL,
                          LCD_WIDTH >> 1, false);
    dma = dma_channel_get_default_config(line_dma);
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_read_increment(&dma, false);
    channel_config_set_high_priority(&dma, true);
    dma_channel_configure(line_dma, &dma,
        &dma_hw->ch[pixel_dma].al3_read_addr_trig, &next_line_addr, 1, false);

#ifdef PICO_STD_DISPLAY_PSRAM
    dma = dma_channel_get_default_config(prefetch_dma);
    channel_config_set_transfer_data_size(&dma, DMA_SIZE_32);
    channel_config_set_read_increment(&dma, true);
    channel_config_set_write_increment(&dma, true);
    channel_config_set_high_priority(&dma, true);
    dma_channel_configure(prefetch_dma, &dma, NULL, NULL, 0, false);
#endif

    panel_init();

    hw_set_bits(&display_pio->inte1, 0x010u << timing_sm);
    irq_set_exclusive_handler(pio_get_irq_num(display_pio, 1), timing_isr);
    irq_set_priority(pio_get_irq_num(display_pio, 1), 0x80);
    irq_set_enabled(pio_get_irq_num(display_pio, 1), true);
    hw_set_bits(&display_pio->inte0, 0x300u);
    irq_set_exclusive_handler(pio_get_irq_num(display_pio, 0), line_isr);
    irq_set_priority(pio_get_irq_num(display_pio, 0), 0x20);
    irq_set_enabled(pio_get_irq_num(display_pio, 0), true);

    display_status = 1;
    __sev();
    while (true) tight_loop_contents();
}

static int start_display(void) {
    if (display_status != 0) return display_status > 0 ? 0 : display_status;
    display_status = -1;
#ifdef PICO_STD_DISPLAY_PSRAM
    detected_psram_size = presto_psram_size();
    int psram_status = presto_psram_init_status();
    if (psram_status <= 0 || detected_psram_size < FRAMEBUFFER_BYTES) {
        printf("Display: PSRAM unavailable (status=%d size=%lu)\n",
               psram_status, (unsigned long)detected_psram_size);
        display_status = -2;
        return display_status;
    }
#endif
    multicore_reset_core1();
    multicore_launch_core1_with_stack(display_core1, core1_stack, sizeof(core1_stack));
    while (display_status == -1) __wfe();
#ifdef PICO_STD_DISPLAY_PSRAM
    printf("Display: %lu-byte framebuffer in %lu-byte PSRAM"
           " (samples=%04x,%04x,%04x)\n",
           (unsigned long)FRAMEBUFFER_BYTES,
           (unsigned long)detected_psram_size,
           psram_sample_first, psram_sample_middle, psram_sample_last);
#else
    printf("Display: no PSRAM; full-resolution timing from an SRAM scanline\n");
#endif
    return display_status > 0 ? 0 : display_status;
}

int presto_display_start_test_pattern(void) {
#ifdef PICO_STD_DISPLAY_PSRAM
    dma_framebuffer = (uint16_t *)PSRAM_DMA_BASE;
    fill_framebuffer_on_start = true;
#endif
    return start_display();
}

#ifdef PICO_STD_DISPLAY_PSRAM
int presto_display_start_framebuffer(uint16_t *framebuffer, size_t pixel_count) {
    if (!framebuffer_is_valid(framebuffer, pixel_count)) {
        return -3;
    }
    uintptr_t address = (uintptr_t)framebuffer;

    /*
     * Newlib returns the cached PSRAM alias. Commit its initial contents
     * before DMA starts, then scan and update the buffer through the
     * non-caching/non-allocating alias.
     */
    xip_cache_clean_all();
    dma_framebuffer = (uint16_t *)
        (XIP_NOCACHE_NOALLOC_BASE + (address - XIP_BASE));
    fill_framebuffer_on_start = false;
    return start_display();
}

int presto_display_present_framebuffer(uint16_t *framebuffer, size_t pixel_count) {
    (void)framebuffer;
    (void)pixel_count;
    return display_status > 0 ? 0 : -2;
}
#endif
