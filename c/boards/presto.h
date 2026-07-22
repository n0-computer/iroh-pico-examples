/* Minimal Pimoroni Presto board definition for Pico SDK.
 * Derived from pimoroni/presto boards/presto/presto.h (BSD-3-Clause). */

#ifndef _BOARDS_PRESTO_H
#define _BOARDS_PRESTO_H

#define PIMORONI_PRESTO 1
#define PICO_RP2350B 1

#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 0
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 40
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 41
#endif

#define PIMORONI_PRESTO_PSRAM_CS_PIN 47

/* Onboard Raspberry Pi RM2 (CYW43439). These are the assignments from
 * Pimoroni's official Presto board header; DATA is bidirectional PIO-SPI. */
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 24u
#define CYW43_DEFAULT_PIN_WL_DATA_IN 24u
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 24u
#define CYW43_DEFAULT_PIN_WL_CLOCK 29u
#define CYW43_DEFAULT_PIN_WL_CS 25u

/* Pimoroni deliberately slows PIO-SPI on Presto for adequate timing margin. */
#define CYW43_PIO_CLOCK_DIV_INT 3

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

/* Presto does not have Pico 2's SMPS mode, VBUS or VSYS GPIO wiring. */

#endif
