#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"
#include "pico/time.h"

#include "ws2812.pio.h"

#define PRESTO_LED_PIN 33u
#define PRESTO_LED_COUNT 7u
#define WS2812_FREQ_HZ 800000.0f

static bool outputs_initialized;
static bool leds_initialized;
static bool leds_available;
static PIO led_pio = pio0;
static uint led_sm;
static uint led_offset;
static uint32_t led_pixels[PRESTO_LED_COUNT];
static int32_t last_error;

static void outputs_init_leds(void) {
    if (leds_initialized) return;

    // Use a dedicated PIO instance to avoid interfering with Wi-Fi/display.
#if NUM_PIOS > 2
    int sm = pio_claim_unused_sm(pio2, false);
    if (sm < 0) {
        leds_available = false;
        return;
    }
    led_pio = pio2;
    led_sm = (uint)sm;
#else
    leds_available = false;
    return;
#endif

    // RP2350 PIO needs the GPIO base set for pin banks above 31.
    pio_set_gpio_base(led_pio, PRESTO_LED_PIN >= 32u ? 16u : 0u);

    led_offset = pio_add_program(led_pio, &ws2812_program);
    ws2812_program_init(led_pio, led_sm, led_offset, PRESTO_LED_PIN, WS2812_FREQ_HZ);
    leds_available = true;
    leds_initialized = true;
}

void presto_outputs_init(void) {
    if (outputs_initialized) return;
    outputs_initialized = true;

    outputs_init_leds();
}

void presto_outputs_set_led_rgb(uint32_t index, uint8_t red, uint8_t green, uint8_t blue) {
    if (index >= PRESTO_LED_COUNT) return;
    led_pixels[index] = ((uint32_t)green << 16) | ((uint32_t)red << 8) | (uint32_t)blue;
}

int32_t presto_outputs_show(void) {
    if (!leds_initialized) {
        last_error = -3;
        return last_error;
    }
    if (!leds_available) {
        last_error = -1;
        return last_error;
    }

    // On Presto we sometimes observe a stuck TX-full condition on subsequent
    // frames unless the state machine is explicitly re-armed.
    pio_sm_set_enabled(led_pio, led_sm, false);
    pio_sm_clear_fifos(led_pio, led_sm);
    pio_sm_restart(led_pio, led_sm);
    pio_sm_clkdiv_restart(led_pio, led_sm);
    pio_sm_set_enabled(led_pio, led_sm, true);

    for (uint i = 0; i < PRESTO_LED_COUNT; ++i) {
        // Never block forever: if TX FIFO remains full, report an error so
        // callers can log/handle it instead of hanging the whole app.
        uint32_t spins = 0;
        while (pio_sm_is_tx_fifo_full(led_pio, led_sm)) {
            if (++spins > 2000000u) {
                last_error = -4;
                return last_error;
            }
        }
        pio_sm_put(led_pio, led_sm, led_pixels[i] << 8u);
    }

    // Give SK6812/WS2812 line-reset/latch time between frames.
    sleep_us(80);

    last_error = 0;
    return 0;
}

int32_t presto_outputs_all_off(void) {
    for (uint i = 0; i < PRESTO_LED_COUNT; ++i) {
        led_pixels[i] = 0;
    }
    return presto_outputs_show();
}

int32_t presto_outputs_status(void) {
    if (!outputs_initialized) return -2;
    if (!leds_initialized) return -3;
    return leds_available ? 0 : -1;
}

int32_t presto_outputs_last_error(void) {
    return last_error;
}
