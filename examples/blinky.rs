use embedded_hal::digital::OutputPin;
use pico_std as _;
use rp235x_hal as hal;
/// Blink an LED on GPIO 25 at 1 Hz, using std threads for timing on top of the
/// RP2350 std environment. Peripheral access is via rp235x-hal.
use std::{thread, time::Duration}; // pulls in the startup glue (__wrap_main); always import.

fn main() {
    let mut pac = hal::pac::Peripherals::take().unwrap();
    let sio = hal::Sio::new(pac.SIO);
    let pins = hal::gpio::Pins::new(
        pac.IO_BANK0,
        pac.PADS_BANK0,
        sio.gpio_bank0,
        &mut pac.RESETS,
    );

    let mut led = pins.gpio25.into_push_pull_output();

    loop {
        led.set_high().unwrap();
        thread::sleep(Duration::from_millis(500));

        led.set_low().unwrap();
        thread::sleep(Duration::from_millis(500));
    }
}
