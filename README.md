# `std` Rust on Pimoroni Presto (RP2350)

This is an experimental Rust standard-library environment for the Pimoroni
Presto. It combines:

- nightly Rust and a custom Cortex-M33 target;
- Raspberry Pi Pico SDK 2.1.1 for RP2350 startup, USB and the RM2 radio;
- FreeRTOS for threads and synchronization;
- Newlib and a small amount of ESP-IDF pthread/newlib glue;
- lwIP for Wi-Fi networking.

The currently tested configuration runs FreeRTOS on ARM core 0. Core 1 is not
started by the scheduler and remains available for future explicit multicore
work.

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

This project needs the **complete Arm GNU Toolchain for `arm-none-eabi`**,
including Newlib. Homebrew's compiler-only `arm-none-eabi-gcc` installation is
not sufficient.

Download Arm GNU Toolchain **15.2.Rel1** from Arm's download page. On Apple
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
PICO_ARM_TOOLCHAIN_BIN=/absolute/path/to/bin cargo build --release --example hello
```

The complete toolchain occupies roughly 1 GB. `.tools/` is ignored by Git.

### Pico SDK and FreeRTOS sources

Populate the ignored dependency directory with the revisions used during
bring-up:

```sh
mkdir -p .tools

git clone https://github.com/raspberrypi/pico-sdk .tools/pico-sdk
git -C .tools/pico-sdk checkout bddd20f928ce76142793bef434d4f75f4af6e433
git -C .tools/pico-sdk submodule update --init --recursive

git clone https://github.com/FreeRTOS/FreeRTOS-Kernel.git .tools/FreeRTOS-Kernel
git -C .tools/FreeRTOS-Kernel checkout 78069a79ea8f9d17c0eae88c417fd41e2c54a2cd
```

The Pico SDK checkout is tag `2.1.1`. Its submodules include TinyUSB, lwIP and
the CYW43 driver. Together with FreeRTOS and the Arm toolchain, `.tools/` is
about 1.8 GB.

## Build, flash and monitor

The Cargo runner uses `picotool`, so a normal release build and flash is:

```sh
cargo run --release --example hello
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
WIFI_CONFIG='SSID:PASSWORD' cargo run --release --example wifi --features wifi
```

`WIFI_CONFIG` is embedded in the firmware image. It is not written to this
repository, but the resulting ELF contains the password and must be treated as
sensitive. This simple `SSID:PASSWORD` format does not support a colon in the
SSID.

## Diagnostics

Two feature-gated probes are retained because failures before USB enumeration
otherwise look identical:

```sh
cargo run --release --example hello --features usb-smoke
cargo run --release --example hello --features rtos-smoke
```

- `usb-smoke` initializes USB and loops without starting FreeRTOS.
- `rtos-smoke` reports newlib/pthread/task/scheduler bring-up one stage at a
  time.

The CMake build also produces a pure Pico SDK `presto_usb_control` image as a
link/startup control. Cargo never flashes that image automatically.

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
- `cargo run --release` flashing through picotool.

This remains a proof of concept, not a production-supported Rust target.

## License

Original portions are licensed under MIT or Apache-2.0. Vendored ESP-IDF code
is Apache-2.0. Pico SDK, FreeRTOS and their submodules retain their upstream
licenses.
