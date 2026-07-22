# RP2350 porting notes

These are the non-obvious pieces required to make Rust `std`, Pico SDK and
FreeRTOS coexist in one RP2350 image.

## Custom Rust target

Rust has no built-in RP2350 target with `std`. The custom JSON target selects:

- LLVM target `thumbv8m.main-none-eabi` (Cortex-M33);
- soft-float EABI, matching the C build;
- Newlib as the C environment;
- `os = "espidf"` and `target-family = "unix"` to select Rust's existing Unix
  standard-library implementation;
- aborting panics and static relocation.

Cargo's unstable `build-std` builds `std` and `panic_abort` from the pinned
nightly's `rust-src`. JSON target specifications also require unstable Cargo
support, enabled in `.cargo/config.toml`.

`ldproxy` translates Cargo's native link invocation into the GCC/Newlib link
assembled from CMake's file API.

## Why the complete Arm toolchain is mandatory

The final link uses `arm-none-eabi-gcc`, Newlib, `libgcc` and the matching
multilib layout. A compiler-only package can compile the Pico SDK but fails once
Rust `std` needs the C runtime. `build.rs` deliberately checks for the complete
project-local toolchain and prepends it to CMake's `PATH`; it also accepts
`PICO_ARM_TOOLCHAIN_BIN` for an external installation.

## Pico SDK startup objects hidden in a static archive

CMake normally links Pico SDK object libraries directly into its executable.
Cargo cannot consume that object-file list through `embuild`; it receives a
combined static archive instead.

Several RP2350 startup translation units are referenced only by entries in
ordered `.preinit_array` sections. A normal archive search does not extract an
object merely because it contains such an entry. The first Rust ELF therefore
lost clock/reset/boot-lock/vector-table initialization, and USB never
enumerated even though the same sources worked in a pure C executable.

`c/CMakeLists.txt` passes targeted `-Wl,-u,...` options for representative
symbols in those translation units. This forces archive extraction while
avoiding `--whole-archive`, which would both bloat the image and introduce
duplicate atomic implementations.

The retained initializer sequence includes bootrom reset, early peripheral
resets, USB power-down state, clocks, post-clock resets, boot locks, spin locks,
mutexes, the RAM vector table, alarm pools and per-core setup.

## FreeRTOS startup

The selected FreeRTOS community port is
`GCC/RP2350_ARM_NTZ/non_secure`. Despite the directory name, the application is
built for Pico SDK's `rp2350-arm-s` platform with FreeRTOS configured for secure
execution and without TrustZone partitioning.

Three details mattered:

1. The proof of concept schedules only core 0 (`configNUMBER_OF_CORES = 1`).
   Core 1 remains untouched.
2. `configUSE_DYNAMIC_EXCEPTION_HANDLERS = 0` makes the port define Pico SDK's
   `isr_svcall`, `isr_pendsv` and `isr_systick` symbols directly. An additional
   CMSIS-name bridge caused scheduler startup to stop before the first task.
3. Newlib and ESP pthread mutex initialization runs inside the first FreeRTOS
   task. Initializing those locks before the RP2350 scheduler was running hung
   during `esp_newlib_locks_init()`.
4. ESP-IDF's pthread glue always calls `vTaskCoreAffinitySet`, while FreeRTOS
   omits that symbol from a one-core build. The shim supplies the only sensible
   single-core implementation: a no-op, because every task already runs on
   core 0.

The Rust entry point is wrapped with `--wrap=main`. `__wrap_main` initializes
stdio, creates the initial task and starts FreeRTOS; that task initializes the C
threading layer before calling Rust's real `main`.

## USB CDC and picotool

Pico SDK's TinyUSB stdio implementation supplies both CDC serial and the vendor
reset interface used by picotool. The Cargo runner is:

```text
picotool load -f -u -v -x -t elf
```

It loads and verifies the ELF, then executes it. A terminal holding the CDC
device can interfere with automatic reboot, so output readers should be closed
before flashing.

## Presto board and Wi-Fi

The local board header is based on Pimoroni's Presto definition rather than the
generic Pico 2 board. Relevant differences include RP2350B/48 GPIOs, 16 MB
flash, no Pico 2 SMPS/VBUS/VSYS wiring, and the onboard RM2 module.

The official Presto CYW43 PIO-SPI assignment is:

| Signal | GPIO |
| --- | ---: |
| `WL_REG_ON` | 23 |
| bidirectional data / host wake | 24 |
| chip select | 25 |
| clock | 29 |

Presto also uses `CYW43_PIO_CLOCK_DIV_INT = 3`. Using Pimoroni's generic SP/CE
GPIO 32–35 assignment caused `[CYW43] Failed to start CYW43`.

The `wifi` feature links `pico_cyw43_arch_lwip_sys_freertos`, so the CYW43
driver and full lwIP stack are serviced by FreeRTOS tasks. `c/include/lwipopts.h`
is based on Raspberry Pi's FreeRTOS Wi-Fi examples and enables DHCP and the
socket API.

## Reproducing failures

The retained diagnostic modes intentionally stop at useful boundaries:

- pure C `presto_usb_control`: Pico SDK board/clocks/USB control image;
- `usb-smoke`: Rust final link plus Pico USB, before FreeRTOS;
- `rtos-smoke`: staged task/scheduler/newlib/pthread probe;
- Wi-Fi's pre-scheduler countdown: preserves time to attach a USB reader before
  touching the networking stack.

They are verbose by design; without them, a crash before USB enumeration is
indistinguishable from a host permission or flashing problem.
