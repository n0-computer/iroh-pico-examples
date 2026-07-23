cmake_minimum_required(VERSION 3.19)

include(FetchContent)

# Once dependencies have been populated, builds must not require network access.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# --- Target chip ----------------------------------------------------------
# RP2350 (dual Cortex-M33), as used on the Pimoroni Presto. "rp2350-arm-s"
# selects the Arm cores in secure mode (the default boot mode).
set(PICO_PLATFORM rp2350-arm-s)
# Project-local board header based on Pimoroni's official Presto definition.
set(PICO_BOARD presto CACHE STRING "Pico board" FORCE)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_NUM_GPIOS 48)
# Presto carries an RM2/CYW43439 even though it is not an official Pico W board.
set(PICO_CYW43_SUPPORTED 1)

# Reuse the project-local, ignored dependency cache for all Cargo profiles and
# feature combinations. This avoids one SDK checkout per build-script hash.
get_filename_component(PICO_STD_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../.tools" ABSOLUTE)
set(PICO_SDK_PATH "${PICO_STD_TOOLS_DIR}/pico-sdk-presto")
include(pico_sdk_import.cmake)

set(FREERTOS_KERNEL_PATH "${PICO_STD_TOOLS_DIR}/FreeRTOS-Kernel")

project(pico-std-rust C CXX ASM)

pico_sdk_init()

# The Pico CYW43/FreeRTOS convenience target assumes a freestanding C program
# and asks lwIP to provide a Linux-numbered global `errno`. Rust's Newlib std
# backend instead reads Newlib's task-local __errno(), whose error-number table
# also differs. Make lwIP use Newlib's <errno.h> so socket failures are visible
# to Rust with the correct values (especially EWOULDBLOCK on nonblocking I/O).
get_target_property(PICO_CYW43_DEFINITIONS
    pico_cyw43_arch_lwip_sys_freertos_headers INTERFACE_COMPILE_DEFINITIONS)
if(PICO_CYW43_DEFINITIONS)
    list(REMOVE_ITEM PICO_CYW43_DEFINITIONS LWIP_PROVIDE_ERRNO=1)
    set_property(TARGET pico_cyw43_arch_lwip_sys_freertos_headers
        PROPERTY INTERFACE_COMPILE_DEFINITIONS "${PICO_CYW43_DEFINITIONS}")
endif()

# Pull in FreeRTOS. This must come after pico_sdk_init() because the port links
# against Pico SDK targets. It defines FreeRTOS-Kernel and FreeRTOS-Kernel-Heap4.
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/include)
include(FreeRTOS_Kernel_import.cmake)

# --- C support library ----------------------------------------------------
# The esp-idf std glue (pthread / locks / 64-bit atomics) plus small shims.
add_library(pico-std-rust-c STATIC
    src/shim.c
    src/platform.c

    esp-idf/src/pthread.c
    esp-idf/src/pthread_cond_var.c
    esp-idf/src/pthread_local_storage.c
    esp-idf/src/pthread_rwlock.c
    # Although ESP-IDF pthread has a semaphore implementation, we use the
    # pico-sdk one instead.
    esp-idf/src/locks.c
    esp-idf/src/stdatomic.c
)

if(DEFINED ENV{CARGO_FEATURE_WIFI})
    target_sources(pico-std-rust-c PRIVATE src/wifi.c)
endif()
if(DEFINED ENV{CARGO_FEATURE_DISPLAY})
    target_sources(pico-std-rust-c PRIVATE src/display.c)
    pico_generate_pio_header(pico-std-rust-c
        ${CMAKE_CURRENT_LIST_DIR}/src/st7701_parallel.pio)
    pico_generate_pio_header(pico-std-rust-c
        ${CMAKE_CURRENT_LIST_DIR}/src/st7701_timing.pio)
endif()
if(DEFINED ENV{CARGO_FEATURE_DISPLAY_PSRAM})
    target_compile_definitions(pico-std-rust-c PRIVATE
        PICO_STD_DISPLAY_PSRAM=1)
endif()
if(DEFINED ENV{CARGO_FEATURE_PSRAM})
    target_sources(pico-std-rust-c PRIVATE src/psram.c)
endif()
target_include_directories(pico-std-rust-c PUBLIC esp-idf/include include)
# The vendored ESP-IDF glue predates GCC 14, which promotes these to errors.
# clock_gettime resolves from newlib at link time; esp_cpu_compare_and_set is
# provided by shim.c.
target_compile_options(pico-std-rust-c PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=implicit-int
)
target_link_libraries(pico-std-rust-c PUBLIC
    pico_stdlib
    pico_rand
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

if(DEFINED ENV{CARGO_FEATURE_DISPLAY})
    target_link_libraries(pico-std-rust-c PUBLIC
        hardware_dma
        hardware_pio
        hardware_pwm
        hardware_spi
        hardware_xip_cache
        pico_multicore
    )
endif()

if(DEFINED ENV{CARGO_FEATURE_WIFI})
    target_link_libraries(pico-std-rust-c PUBLIC
        pico_cyw43_arch_lwip_sys_freertos
        pico_lwip_sntp
    )
    target_compile_definitions(pico-std-rust-c PUBLIC
        PICO_STD_WIFI=1
        NO_SYS=0
        LWIP_SOCKET=1
        LWIP_ERRNO_STDINCLUDE=1
    )
endif()
target_compile_definitions(pico-std-rust-c PUBLIC
    _POSIX_THREADS _POSIX_BARRIERS _POSIX_READER_WRITER_LOCKS _UNIX98_THREAD_MUTEX_ATTRIBUTES

    CONFIG_PTHREAD_STACK_MIN=256
    CONFIG_PTHREAD_TASK_CORE_DEFAULT=-1
    CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT=1024
    CONFIG_PTHREAD_TASK_PRIO_DEFAULT=5
    CONFIG_PTHREAD_TASK_NAME_DEFAULT="task"
)
if(DEFINED ENV{CARGO_FEATURE_PSRAM_HEAP})
    # This must reach the FreeRTOS kernel sources as well as our support
    # library because FreeRTOSConfig.h sizes the internal Heap4 arena from it.
    add_compile_definitions(PICO_STD_PSRAM_HEAP=1)
endif()

# Enable stdio drivers specified by cargo features
# (environment variables are inherited from build.rs)
if(DEFINED ENV{CARGO_FEATURE_STDIO_UART})
    pico_enable_stdio_uart(pico-std-rust-c 1)
else()
    pico_enable_stdio_uart(pico-std-rust-c 0)
endif()

if(DEFINED ENV{CARGO_FEATURE_STDIO_USB})
    pico_enable_stdio_usb(pico-std-rust-c 1)
else()
    pico_enable_stdio_usb(pico-std-rust-c 0)
endif()

if(DEFINED ENV{CARGO_FEATURE_STDIO_SEMIHOSTING})
    pico_enable_stdio_semihosting(pico-std-rust-c 1)
else()
    pico_enable_stdio_semihosting(pico-std-rust-c 0)
endif()

# Create an executable with empty main function for retrieving linker flags
add_executable(exe src/empty_main.c)
target_link_libraries(exe pico-std-rust-c)
# Cargo receives this target's libraries and linker options, but not CMake's
# object-file list.  Pull the Pico runtime-init translation units out of the
# resulting static archive explicitly; their only otherwise-visible references
# live in .preinit_array and therefore do not trigger archive extraction.
# Forcing one symbol from runtime_init.c retains all of that file's ordered init
# entries.  The clock setup has its own translation unit.
target_link_options(exe PRIVATE
    -Wl,-u,runtime_init_bootrom_reset
    -Wl,-u,runtime_init_clocks
    -Wl,-u,runtime_init_boot_locks_reset
    -Wl,-u,runtime_init_bootrom_locking_enable
)
if(DEFINED ENV{CARGO_FEATURE_IROH})
    target_link_options(exe PRIVATE
        -Wl,--wrap=read
        -Wl,--wrap=write
        -Wl,--wrap=close
        -Wl,--wrap=fcntl
    )
endif()

# Pure Pico SDK control image for separating board/USB bring-up from the custom
# Rust/FreeRTOS final link. It is built alongside `exe`, but never flashed
# automatically by Cargo.
add_executable(presto_usb_control src/usb_control.c)
target_link_libraries(presto_usb_control pico_stdlib)
pico_enable_stdio_uart(presto_usb_control 0)
pico_enable_stdio_usb(presto_usb_control 1)
pico_add_extra_outputs(presto_usb_control)
add_dependencies(exe presto_usb_control)

# Pure Pico SDK PSRAM probe. This deliberately excludes Rust and FreeRTOS so
# QMI hardware bring-up can be tested independently of the custom std runtime.
add_executable(presto_psram_control
    src/psram_control.c
    src/psram.c
)
target_link_libraries(presto_psram_control pico_stdlib)
pico_enable_stdio_uart(presto_psram_control 1)
pico_enable_stdio_usb(presto_psram_control 1)
pico_add_extra_outputs(presto_psram_control)
add_dependencies(exe presto_psram_control)
