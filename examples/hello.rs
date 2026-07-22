use std::{thread, time::Duration};

// Ensure the RP2350/FreeRTOS startup glue is linked into the executable.
use pico_std as _;

fn main() {
    loop {
        println!("Hello from std Rust on RP2350!");
        thread::sleep(Duration::from_secs(1));
    }
}
