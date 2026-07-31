#include <stdint.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"

#define PRESTO_PWM_TOP 65535u

static float presto_pwm_divider_for_freq(uint32_t frequency_hz) {
    if (frequency_hz < 50u) {
        return 0.0f;
    }

    uint32_t sys_hz = clock_get_hz(clk_sys);
    float div = (float)sys_hz / ((float)frequency_hz * (float)(PRESTO_PWM_TOP + 1u));

    if (div < 1.0f) div = 1.0f;
    if (div > 255.0f) div = 255.0f;
    return div;
}

void presto_io_gpio_init(uint32_t pin) {
    gpio_init(pin);
}

void presto_io_gpio_set_dir(uint32_t pin, uint8_t output) {
    gpio_set_dir(pin, output != 0u);
}

void presto_io_gpio_put(uint32_t pin, uint8_t value) {
    gpio_put(pin, value != 0u);
}

uint8_t presto_io_gpio_get(uint32_t pin) {
    return (uint8_t)(gpio_get(pin) ? 1u : 0u);
}

void presto_io_gpio_set_pull(uint32_t pin, int8_t mode) {
    // mode: -1 pull-down, 0 no pull, 1 pull-up
    switch (mode) {
        case -1:
            gpio_pull_down(pin);
            break;
        case 1:
            gpio_pull_up(pin);
            break;
        default:
            gpio_disable_pulls(pin);
            break;
    }
}

int32_t presto_io_pwm_init(uint32_t pin, uint32_t frequency_hz, uint16_t duty_permille) {
    float div = presto_pwm_divider_for_freq(frequency_hz);
    if (div == 0.0f) {
        return -1;
    }

    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(pin);

    pwm_set_wrap(slice, PRESTO_PWM_TOP);
    pwm_set_clkdiv(slice, div);

    if (duty_permille > 1000u) duty_permille = 1000u;
    uint32_t level = ((uint32_t)(PRESTO_PWM_TOP + 1u) * duty_permille) / 1000u;
    if (level > PRESTO_PWM_TOP) level = PRESTO_PWM_TOP;
    pwm_set_gpio_level(pin, (uint16_t)level);
    pwm_set_enabled(slice, true);

    return 0;
}

int32_t presto_io_pwm_set_frequency(uint32_t pin, uint32_t frequency_hz) {
    float div = presto_pwm_divider_for_freq(frequency_hz);
    if (div == 0.0f) {
        return -1;
    }

    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_set_clkdiv(slice, div);

    return 0;
}

void presto_io_pwm_set_duty(uint32_t pin, uint16_t duty_permille) {
    if (duty_permille > 1000u) duty_permille = 1000u;
    uint32_t level = ((uint32_t)(PRESTO_PWM_TOP + 1u) * duty_permille) / 1000u;
    if (level > PRESTO_PWM_TOP) level = PRESTO_PWM_TOP;
    pwm_set_gpio_level(pin, (uint16_t)level);
}

void presto_io_pwm_stop(uint32_t pin) {
    pwm_set_gpio_level(pin, 0u);
}
