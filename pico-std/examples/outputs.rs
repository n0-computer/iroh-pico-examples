use std::{thread, time::Duration};

// Ensure the RP2350/FreeRTOS startup glue is linked into the executable.
use pico_std as _;
use pico_std::io::{GpioPin, PwmPin};
use pico_std::outputs::AmbientLeds;

const PRESTO_BUZZER_PIN: u32 = 43;
const PRESTO_AUDIO_ENABLE_PIN: u32 = 44;
const BUZZER_DUTY_PERMILLE: u16 = 500;

fn main() {
    println!("outputs: hello baseline is alive");

    println!("outputs: initializing LED path");
    let mut leds = AmbientLeds::new();
    let off_rc = leds.clear().map_or_else(|code| code, |_| 0);
    println!("outputs: all_off rc = {off_rc}");

    let status = leds.status();
    println!("outputs: LED status code = {status}");
    if status != 0 {
        println!("outputs: LED ERROR (status={status})");
        println!("outputs: status meanings: 0=ok, -1=no free PIO SM, -2=not initialized, -3=LED init not run");
    } else {
        println!("outputs: LED init OK, writing test pattern");
        leds.set_rgb(0, 32, 0, 0);
        leds.set_rgb(1, 0, 32, 0);
        leds.set_rgb(2, 0, 0, 32);
        let show_rc = leds.show().map_or_else(|code| code, |_| 0);
        let last_error = leds.last_error();
        println!("outputs: LED show rc = {show_rc}, last_error = {last_error}");
        if show_rc != 0 {
            println!("outputs: LED ERROR during show (rc={show_rc})");
        } else {
            println!("outputs: LED test pattern sent");
        }
    }

    println!("outputs: initializing beeper on pin {PRESTO_BUZZER_PIN}");
    let audio_enable = GpioPin::new(PRESTO_AUDIO_ENABLE_PIN).init_output(true);
    println!(
        "outputs: drove potential audio-enable pin {PRESTO_AUDIO_ENABLE_PIN} high"
    );

    let buzzer = PwmPin::new(PRESTO_BUZZER_PIN);
    match buzzer.start(900, 0) {
        Ok(()) => {
            println!("outputs: beeper init OK, sending lower chirps");
            for &frequency in &[700u32, 900u32, 1100u32, 1300u32] {
                if let Err(code) = buzzer.set_frequency(frequency) {
                    println!("outputs: beeper set_frequency failed: code={code}, freq={frequency}");
                    continue;
                }
                buzzer.set_duty(BUZZER_DUTY_PERMILLE);
                thread::sleep(Duration::from_millis(220));
                buzzer.stop();
                thread::sleep(Duration::from_millis(120));
            }
            println!("outputs: beeper chirp sequence done");
        }
        Err(code) => {
            println!("outputs: beeper init failed: code={code}");
        }
    }

    audio_enable.set_low();
    println!("outputs: audio-enable pin set low");

    loop {
        println!("outputs: idle");
        thread::sleep(Duration::from_secs(5));
    }
}
