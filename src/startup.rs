use core::ffi::{c_char, c_int, c_void};

extern "C" {
    fn vTaskStartScheduler();
    fn xTaskCreate(
        pvTaskCode: extern "C" fn(*mut c_void),
        pcName: *const c_char,
        usStackDepth: u32,
        pvParameters: *mut c_void,
        uxPriority: u32,
        pxCreatedTask: *mut c_void,
    ) -> i32;

    fn esp_newlib_locks_init();
    fn esp_pthread_init() -> c_int;

    fn stdio_init_all() -> bool;

    #[cfg(feature = "usb-smoke")]
    fn usb_smoke_test() -> !;

    #[cfg(feature = "rtos-smoke")]
    fn rtos_smoke_test() -> !;

    #[cfg(feature = "wifi")]
    fn presto_wifi_countdown();

    fn __real_main(argc: c_int, argv: *const *const c_void) -> c_int;
}

extern "C" fn main_task_wrapper(_parameters: *mut c_void) {
    unsafe {
        // The RP2350 SMP port's mutex implementation requires a running
        // scheduler. Initialize the newlib and pthread layers from the first
        // task, not from the pre-scheduler C main context.
        esp_newlib_locks_init();
        esp_pthread_init();
        __real_main(0, core::ptr::null_mut());
    }
}

// This function is called by the CRT startup routine in place of the C main
// function, because of the "-Wl,--wrap=main" linker flag.
#[no_mangle]
extern "C" fn __wrap_main() {
    unsafe {
        stdio_init_all();

        // Diagnostic mode: prove clocks, USB CDC and Pico SDK stdio before
        // initializing pthreads or starting the FreeRTOS scheduler.
        #[cfg(feature = "usb-smoke")]
        usb_smoke_test();

        #[cfg(feature = "rtos-smoke")]
        rtos_smoke_test();

        // Run before task creation so USB output remains useful even if the
        // larger networking image cannot start its first FreeRTOS task.
        #[cfg(all(
            feature = "wifi",
            not(any(feature = "usb-smoke", feature = "rtos-smoke"))
        ))]
        presto_wifi_countdown();

        #[cfg(not(any(feature = "usb-smoke", feature = "rtos-smoke")))]
        {
            // Run the user's `main` in a FreeRTOS task. The stack depth is in
            // words; give std code plenty of room.
            xTaskCreate(
                main_task_wrapper,
                "main_task\0".as_ptr() as *const c_char,
                4096,
                core::ptr::null_mut() as *mut c_void,
                2,
                core::ptr::null_mut(),
            );

            vTaskStartScheduler(); // Start the FreeRTOS scheduler
        }
    }
}
