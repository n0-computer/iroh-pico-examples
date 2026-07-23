use std::{thread, time::Duration};

use pico_std as _;

extern "C" {
    fn presto_display_start_test_pattern() -> core::ffi::c_int;
}

fn main() {
    println!("Starting full-frame Presto display test from PSRAM");
    let result = unsafe { presto_display_start_test_pattern() };
    assert_eq!(result, 0, "display initialization failed with code {result}");
    println!("480x480 RGB565 display scanout is running from PSRAM on core 1");
    println!("The display should show eight colour bars with horizontal black lines");

    loop {
        // Keep core 0 quiet: flash instruction fetches and PSRAM scanout share
        // QMI, so periodic formatting and USB logging would perturb this test.
        thread::sleep(Duration::from_secs(60));
    }
}
