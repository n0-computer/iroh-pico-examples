use std::{thread, time::Duration};

use pico_std as _;

extern "C" {
    fn presto_display_start_test_pattern() -> core::ffi::c_int;
}

fn main() {
    println!("Starting PSRAM-free full-resolution Presto display timing test");
    let result = unsafe { presto_display_start_test_pattern() };
    assert_eq!(result, 0, "display initialization failed with code {result}");
    println!("480x480 display scanout is running from an SRAM row on core 1");

    loop {
        println!("The display should show eight vertical colour bars");
        thread::sleep(Duration::from_secs(5));
    }
}
