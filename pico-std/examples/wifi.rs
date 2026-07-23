use std::{ffi::CString, thread, time::Duration};

use pico_std as _;

const WIFI_CONFIG: &str = match option_env!("WIFI_CONFIG") {
    Some(value) => value,
    None => panic!("WIFI_CONFIG is not set. Build with WIFI_CONFIG='SSID:PASSWORD' cargo run --release --example wifi --features wifi"),
};

extern "C" {
    fn presto_wifi_connect(
        ssid: *const core::ffi::c_char,
        password: *const core::ffi::c_char,
    ) -> core::ffi::c_int;
}

fn main() {
    let (ssid, password) = WIFI_CONFIG
        .split_once(':')
        .expect("WIFI_CONFIG must be in the format SSID:PASSWORD");
    let ssid = CString::new(ssid).expect("SSID contains a NUL byte");
    let password = CString::new(password).expect("password contains a NUL byte");

    let result = unsafe { presto_wifi_connect(ssid.as_ptr(), password.as_ptr()) };
    if result != 0 {
        panic!("Wi-Fi connection failed with code {result}");
    }

    loop {
        println!("Wi-Fi is up");
        thread::sleep(Duration::from_secs(5));
    }
}
