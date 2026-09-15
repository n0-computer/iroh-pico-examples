# `std` Rust on Pimoroni Presto (RP2350)

This is an experimental Rust standard-library environment for the Pimoroni
Presto. It combines:

- nightly Rust and a custom Cortex-M33 target;
- Raspberry Pi Pico SDK 2.1.1 for RP2350 startup, USB and the RM2 radio;
- FreeRTOS for threads and synchronization;
- Newlib and a small amount of ESP-IDF pthread/newlib glue;
- lwIP for Wi-Fi networking.

The currently tested configuration runs FreeRTOS on ARM core 0. Core 1 is not
started by the scheduler; the display examples use it explicitly for scanout.

## Upstream repository

This work was derived from:

- https://github.com/tana/pico-std-rust

## Repository layout

- The repository root is a virtual Cargo workspace.
- `pico-std` is the platform crate. Its `examples/` directory contains
  focused hardware and runtime tests.
- `iroh-echo-full` is the relay-enabled iroh application.
- `iroh-echo-bare` is a minimal bare-bones iroh app for the same platform.

The application depends on `pico-std` like a normal native platform
dependency. A small application build script forwards the Pico SDK linker
configuration exported by `pico-std`.

## Host prerequisites

The working setup was tested on Apple Silicon macOS. Install the small host
tools with Homebrew and Cargo:

```sh
brew install cmake ninja picotool
cargo install ldproxy
```

The repository pins its Rust nightly in `rust-toolchain.toml` and automatically
installs `rust-src` through rustup.

### Complete Arm GNU toolchain

This project needs the complete Arm GNU Toolchain for `arm-none-eabi`,
including Newlib. Homebrew's compiler-only `arm-none-eabi-gcc` installation is
not sufficient.

Download Arm GNU Toolchain 15.2.Rel1 from Arm's download page. On Apple
Silicon the archive is named:

```text
arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi.tar.xz
```

Unpack it so the compiler has this exact project-local path:

```text
.tools/arm-gnu-toolchain/bin/arm-none-eabi-gcc
```

Alternatively, keep it elsewhere and provide its `bin` directory for every
Cargo invocation:

```sh
PICO_ARM_TOOLCHAIN_BIN=/absolute/path/to/bin cargo build --release -p pico-std --example hello
```

The complete toolchain occupies roughly 1 GB. `.tools/` is ignored by Git.

### Pico SDK and FreeRTOS sources

Populate the ignored dependency directory with the revisions used during
bring-up:

```sh
mkdir -p .tools

git clone https://github.com/raspberrypi/pico-sdk .tools/pico-sdk-presto
git -C .tools/pico-sdk-presto checkout 9a4113fbbae65ee82d8cd6537963bc3d3b14bcca
git -C .tools/pico-sdk-presto submodule update --init --recursive

git clone https://github.com/FreeRTOS/FreeRTOS-Kernel.git .tools/FreeRTOS-Kernel
git -C .tools/FreeRTOS-Kernel checkout 78069a79ea8f9d17c0eae88c417fd41e2c54a2cd
```

The Pico SDK commit matches the revision used by Pimoroni's working Presto
firmware. Its submodules include TinyUSB, lwIP and the CYW43 driver.

## Build, flash and monitor

The Cargo runner uses `picotool`, so a normal release build and flash is:

```sh
cargo run --release -p pico-std --example hello
```

On the first flash, place the Presto in BOOTSEL mode manually. Subsequent
flashes can normally ask Pico SDK's USB reset interface to reboot into BOOTSEL.
Close any process holding the serial device before flashing.

On macOS, allow terminal/picotool access if the removable-device permission
dialog appears. A lost or denied dialog looks like a device that picotool can
identify but cannot access; `sudo` does not fix that permission.

For output-only monitoring, avoid a full terminal emulator:

```sh
cat /dev/cu.usbmodem*
```

Stop it with `Ctrl-C`, and close it before the next `cargo run`.

## Wi-Fi

The `wifi` example initializes Presto's RM2/CYW43439, joins a WPA2 network and
prints its DHCP address. Credentials follow the same build-time environment
pattern used by the ESP32 examples:

```sh
WIFI_CONFIG='SSID:PASSWORD' cargo run --release -p pico-std --example wifi --features wifi
```

`WIFI_CONFIG` is embedded in the firmware image. It is not written to this
repository, but the resulting ELF contains the password and must be treated as
sensitive. This simple `SSID:PASSWORD` format does not support a colon in the
SSID.

## Display smoke test

The display example initializes Presto's 480x480 ST7701 panel and shows eight
vertical colour bars:

```sh
cargo run --release -p pico-std --example display --features display
```

The panel has no useful onboard framebuffer and must receive a continuous RGB
scanout. This known-working control uses no PSRAM: core 1 repeats one
480-pixel RGB565 row from internal SRAM while FreeRTOS/std remains on core 0.

The full-frame test initializes PSRAM before Rust and scans a 480x480 RGB565
framebuffer from it. In the current workspace this is the same `psram` example,
with the heap variant enabled by the `psram-heap` feature:

```sh
cargo run --release -p pico-std --example psram --features psram
cargo run --release -p pico-std --example psram --features psram-heap
```

## PSRAM probe

PSRAM bring-up is isolated from the display and all other examples:

```sh
cargo run --release -p pico-std --example psram --features psram
```

It initializes the 8 MiB QMI PSRAM before USB and FreeRTOS, checks ordinary
access at both ends of the device, then tests an atomic read-modify-write using
the Cortex-M33 local exclusive monitor. PSRAM setup uses fixed QMI timing for
Presto's 200 MHz system clock so its SRAM-resident initialization path does not
call flash-resident 64-bit division helpers while reconfiguring QMI.

`psram-heap` additionally moves Newlib's heap—and therefore Rust `std`
allocations—to PSRAM. FreeRTOS's Heap4 arena, task stacks and scheduler objects
remain in internal SRAM. PSRAM-backed iroh builds expand Heap4 to 256 KiB and
give the main task a 128 KiB stack for TLS/HTTPS processing.

## Diagnostics

Two feature-gated probes are retained because failures before USB enumeration
otherwise look identical:

```sh
cargo run --release -p pico-std --example hello --features usb-smoke
cargo run --release -p pico-std --example hello --features rtos-smoke
```

- `usb-smoke` initializes USB and loops without starting FreeRTOS.
- `rtos-smoke` reports newlib/pthread/task/scheduler bring-up one stage at a
time.

The CMake build also produces a pure Pico SDK `presto_usb_control` image as a
link/startup control. Cargo never flashes that image automatically.

It also produces `presto_psram_control.uf2`, a pure C PSRAM probe with no Rust
or FreeRTOS. It starts USB first, prints a ten-second countdown, initializes
QMI PSRAM, and checks the first, middle and last words. Build it through Cargo,
then find and flash the generated image:

```sh
cargo build --release -p pico-std --example psram --features psram
find target -name presto_psram_control.uf2
picotool load -f /path/printed/by/find/presto_psram_control.uf2
picotool reboot
```

The probe also enables UART0 on GPIO 0/1 at 115200 baud as a fallback if USB
stops during PSRAM initialization.

## Iroh application

The full application enables the default iroh relays and n0 DNS discovery,
and uses PSRAM for the Rust/Newlib heap:

```sh
WIFI_CONFIG='SSID:PASSWORD' cargo run --release -p iroh-echo-full
```

The Pico compatibility layer supplies the entropy, clock, `poll` and `eventfd`
interfaces required by Rust `std`, Tokio and mio. Its eventfd integration polls
lwIP in 10 ms slices; this is intentionally simple and prioritizes correctness
and bring-up over high-throughput networking.

## Why this is unusual

Rust does not ship a `std` target for the RP2350. This project builds `std` from
`rust-src` for a custom target that describes Cortex-M33/Newlib while borrowing
the existing `espidf` OS identity expected by Rust's Unix `std` implementation.
The target name is therefore intentionally odd:

```text
thumbv8m_main-none-espidf-eabi
```

See [docs/porting-notes.md](docs/porting-notes.md) for the startup, linker,
FreeRTOS and Wi-Fi details discovered during the port.

## Status

Working on a Pimoroni Presto:

- `println!` over USB CDC;
- sleeping through FreeRTOS; the pthread-backed `thread::spawn` example builds
  but still needs an on-device smoke test;
- RM2/CYW43439 Wi-Fi association and DHCP;
- ST7701 scanout from either a repeated SRAM row or a full PSRAM framebuffer;
- relay-enabled iroh QUIC echo application;
- `cargo run --release` flashing through picotool.

This remains a proof of concept, not a production-supported Rust target.

## License

Original portions are licensed under MIT or Apache-2.0. Vendored ESP-IDF code
is Apache-2.0. Pico SDK, FreeRTOS and their submodules retain their upstream
licenses.
