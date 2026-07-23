use core::sync::atomic::{AtomicUsize, Ordering};
#[cfg(not(feature = "psram-heap"))]
use std::ptr;
use std::{thread, time::Duration};

use pico_std as _;

extern "C" {
    fn presto_psram_size() -> usize;
    fn presto_psram_init_status() -> core::ffi::c_int;
}

const PSRAM_BASE: usize = 0x1100_0000;

fn main() {
    let size = unsafe { presto_psram_size() };
    let status = unsafe { presto_psram_init_status() };
    println!("PSRAM initialization: status={status}, size={size}");
    assert_eq!(size, 8 * 1024 * 1024, "expected 8 MiB PSRAM");

    #[cfg(not(feature = "psram-heap"))]
    test_raw_psram(size);

    #[cfg(feature = "psram-heap")]
    test_psram_heap();

    loop {
        println!("PSRAM ordinary and atomic access passed");
        thread::sleep(Duration::from_secs(5));
    }
}

#[cfg(not(feature = "psram-heap"))]
fn test_raw_psram(size: usize) {
    let first = PSRAM_BASE as *mut u32;
    let last = (PSRAM_BASE + size - size_of::<u32>()) as *mut u32;
    unsafe {
        ptr::write_volatile(first, 0x1357_9bdf);
        ptr::write_volatile(last, 0x2468_ace0);
        let first_value = ptr::read_volatile(first);
        let last_value = ptr::read_volatile(last);
        println!(
            "ordinary readback: first=0x{:08x}, last=0x{:08x}",
            first_value,
            last_value
        );
        assert_eq!(first_value, 0x1357_9bdf, "first PSRAM word mismatch");
        assert_eq!(last_value, 0x2468_ace0, "last PSRAM word mismatch");

        let atomic = &*(first as *const AtomicUsize);
        atomic.store(41, Ordering::Relaxed);
        let previous = atomic.fetch_add(1, Ordering::SeqCst);
        let current = atomic.load(Ordering::SeqCst);
        println!(
            "atomic fetch_add: previous={previous}, current={}",
            current
        );
        assert_eq!(previous, 41, "PSRAM atomic returned the wrong old value");
        assert_eq!(current, 42, "PSRAM atomic update did not stick");
    }
}

#[cfg(feature = "psram-heap")]
fn test_psram_heap() {
    let mut allocation = vec![0u32; 256 * 1024];
    let address = allocation.as_ptr() as usize;
    assert!(
        (PSRAM_BASE..PSRAM_BASE + 8 * 1024 * 1024).contains(&address),
        "Rust allocation is outside PSRAM: 0x{address:08x}"
    );
    let last = allocation.len() - 1;
    allocation[0] = 0x1357_9bdf;
    allocation[last] = 0x2468_ace0;
    assert_eq!(allocation[0], 0x1357_9bdf);
    assert_eq!(allocation[last], 0x2468_ace0);

    let atomic = Box::new(AtomicUsize::new(41));
    let atomic_address = (&*atomic as *const AtomicUsize) as usize;
    assert!(
        (PSRAM_BASE..PSRAM_BASE + 8 * 1024 * 1024).contains(&atomic_address),
        "Rust atomic allocation is outside PSRAM: 0x{atomic_address:08x}"
    );
    assert_eq!(atomic.fetch_add(1, Ordering::SeqCst), 41);
    assert_eq!(atomic.load(Ordering::SeqCst), 42);
    println!(
        "Rust PSRAM heap passed: 1 MiB at 0x{address:08x}, atomic at 0x{atomic_address:08x}"
    );
}
