pub mod startup;

pub mod io {
    use core::ffi::c_int;

    #[derive(Clone, Copy, Debug)]
    pub enum Pull {
        None,
        Up,
        Down,
    }

    #[derive(Clone, Copy, Debug)]
    pub struct GpioPin {
        pin: u32,
    }

    impl GpioPin {
        pub const fn new(pin: u32) -> Self {
            Self { pin }
        }

        pub fn init_input(self, pull: Pull) -> Self {
            unsafe {
                presto_io_gpio_init(self.pin);
                presto_io_gpio_set_dir(self.pin, 0);
                presto_io_gpio_set_pull(self.pin, pull.as_raw());
            }
            self
        }

        pub fn init_output(self, initial_high: bool) -> Self {
            unsafe {
                presto_io_gpio_init(self.pin);
                presto_io_gpio_set_dir(self.pin, 1);
                presto_io_gpio_put(self.pin, if initial_high { 1 } else { 0 });
            }
            self
        }

        pub fn set_high(&self) {
            unsafe { presto_io_gpio_put(self.pin, 1) }
        }

        pub fn set_low(&self) {
            unsafe { presto_io_gpio_put(self.pin, 0) }
        }

        pub fn set_level(&self, high: bool) {
            unsafe { presto_io_gpio_put(self.pin, if high { 1 } else { 0 }) }
        }

        pub fn is_high(&self) -> bool {
            unsafe { presto_io_gpio_get(self.pin) != 0 }
        }

        pub fn is_low(&self) -> bool {
            !self.is_high()
        }

        pub const fn number(&self) -> u32 {
            self.pin
        }
    }

    impl Pull {
        const fn as_raw(self) -> i8 {
            match self {
                Self::None => 0,
                Self::Up => 1,
                Self::Down => -1,
            }
        }
    }

    #[derive(Clone, Copy, Debug)]
    pub struct PwmPin {
        pin: u32,
    }

    impl PwmPin {
        pub const fn new(pin: u32) -> Self {
            Self { pin }
        }

        pub fn start(&self, frequency_hz: u32, duty_permille: u16) -> Result<(), i32> {
            let result = unsafe { presto_io_pwm_init(self.pin, frequency_hz, duty_permille) };
            if result == 0 {
                Ok(())
            } else {
                Err(result)
            }
        }

        pub fn set_frequency(&self, frequency_hz: u32) -> Result<(), i32> {
            let result = unsafe { presto_io_pwm_set_frequency(self.pin, frequency_hz) };
            if result == 0 {
                Ok(())
            } else {
                Err(result)
            }
        }

        pub fn set_duty(&self, duty_permille: u16) {
            unsafe { presto_io_pwm_set_duty(self.pin, duty_permille) }
        }

        pub fn stop(&self) {
            unsafe { presto_io_pwm_stop(self.pin) }
        }

        pub const fn number(&self) -> u32 {
            self.pin
        }
    }

    unsafe extern "C" {
        fn presto_io_gpio_init(pin: u32);
        fn presto_io_gpio_set_dir(pin: u32, output: u8);
        fn presto_io_gpio_put(pin: u32, value: u8);
        fn presto_io_gpio_get(pin: u32) -> u8;
        fn presto_io_gpio_set_pull(pin: u32, mode: i8);

        fn presto_io_pwm_init(pin: u32, frequency_hz: u32, duty_permille: u16) -> c_int;
        fn presto_io_pwm_set_frequency(pin: u32, frequency_hz: u32) -> c_int;
        fn presto_io_pwm_set_duty(pin: u32, duty_permille: u16);
        fn presto_io_pwm_stop(pin: u32);
    }
}

pub mod outputs {
    pub const AMBIENT_LED_COUNT: usize = 7;

    #[derive(Clone, Copy, Debug)]
    pub struct AmbientLeds;

    impl AmbientLeds {
        pub fn new() -> Self {
            unsafe { presto_outputs_init() };
            Self
        }

        pub fn status(&self) -> i32 {
            unsafe { presto_outputs_status() }
        }

        pub fn set_rgb(&mut self, index: usize, red: u8, green: u8, blue: u8) {
            unsafe { presto_outputs_set_led_rgb(index as u32, red, green, blue) }
        }

        pub fn clear(&mut self) -> Result<(), i32> {
            let code = unsafe { presto_outputs_all_off() };
            if code == 0 {
                Ok(())
            } else {
                Err(code)
            }
        }

        pub fn show(&mut self) -> Result<(), i32> {
            let code = unsafe { presto_outputs_show() };
            if code == 0 {
                Ok(())
            } else {
                Err(code)
            }
        }

        pub fn last_error(&self) -> i32 {
            unsafe { presto_outputs_last_error() }
        }
    }

    extern "C" {
        fn presto_outputs_init();
        fn presto_outputs_set_led_rgb(index: u32, red: u8, green: u8, blue: u8);
        fn presto_outputs_show() -> i32;
        fn presto_outputs_all_off() -> i32;
        fn presto_outputs_status() -> i32;
        fn presto_outputs_last_error() -> i32;
    }
}

#[cfg(feature = "display-psram")]
pub mod display {
    use core::{convert::Infallible, marker::PhantomData, ptr, ptr::NonNull, slice};
    use embedded_graphics::{
        geometry::{OriginDimensions, Point, Size},
        mono_font::{ascii::FONT_8X13_BOLD, MonoTextStyleBuilder},
        pixelcolor::{IntoStorage, Rgb565},
        prelude::{DrawTarget, Drawable, Pixel},
        text::{Baseline, Text},
    };

    pub const WIDTH: usize = 480;
    pub const HEIGHT: usize = 480;
    pub const PIXELS: usize = WIDTH * HEIGHT;

    const PSRAM_BASE: usize = 0x1100_0000;
    const PSRAM_END: usize = 0x1180_0000;
    const NOCACHE_ALIAS_OFFSET: usize = 0x0400_0000;

    extern "C" {
        fn presto_display_start_framebuffer(
            framebuffer: *mut u16,
            pixel_count: usize,
        ) -> core::ffi::c_int;
    }

    /// A running 480x480 RGB565 display backed by a heap allocation in PSRAM.
    ///
    /// The scanout engine runs on core 1. The allocation is intentionally
    /// retained for the remainder of the process because stopping core 1 and
    /// releasing its DMA channels is not currently supported.
    pub struct Display {
        pixels_uncached: NonNull<u16>,
        _not_send_or_sync: PhantomData<*mut ()>,
    }

    impl Display {
        pub fn start() -> Result<Self, i32> {
            let framebuffer = vec![0u16; PIXELS].into_boxed_slice();
            let cached = framebuffer.as_ptr() as usize;
            let bytes = PIXELS * size_of::<u16>();
            assert!(
                cached >= PSRAM_BASE && cached <= PSRAM_END - bytes,
                "display framebuffer was not allocated in PSRAM: 0x{cached:08x}"
            );

            let cached_ptr = Box::into_raw(framebuffer) as *mut u16;
            let result =
                unsafe { presto_display_start_framebuffer(cached_ptr, PIXELS) };
            if result != 0 {
                // The display never started, so reclaiming the allocation is safe.
                unsafe {
                    drop(Box::from_raw(ptr::slice_from_raw_parts_mut(
                        cached_ptr, PIXELS,
                    )));
                }
                return Err(result);
            }

            let uncached_ptr =
                (cached_ptr as usize + NOCACHE_ALIAS_OFFSET) as *mut u16;
            Ok(Self {
                pixels_uncached: NonNull::new(uncached_ptr).unwrap(),
                _not_send_or_sync: PhantomData,
            })
        }

        /// Compatibility no-op for the single-buffer display path.
        pub fn present(&mut self) -> Result<(), i32> {
            Ok(())
        }

        pub fn pixels(&self) -> &[u16] {
            unsafe { slice::from_raw_parts(self.pixels_uncached.as_ptr(), PIXELS) }
        }

        pub fn pixels_mut(&mut self) -> &mut [u16] {
            unsafe { slice::from_raw_parts_mut(self.pixels_uncached.as_ptr(), PIXELS) }
        }

        pub fn fill(&mut self, colour: u16) {
            self.pixels_mut().fill(colour);
        }

        pub fn set_pixel(&mut self, x: usize, y: usize, colour: u16) {
            if x < WIDTH && y < HEIGHT {
                self.pixels_mut()[y * WIDTH + x] = colour;
            }
        }

        /// Draw fixed-width 8x13 bold ASCII text. Coordinates specify the
        /// top-left corner, and colours are raw RGB565 values.
        pub fn draw_text(
            &mut self,
            x: i32,
            y: i32,
            text: &str,
            foreground: u16,
            background: Option<u16>,
        ) {
            let mut builder = MonoTextStyleBuilder::new()
                .font(&FONT_8X13_BOLD)
                .text_color(rgb565(foreground));
            if let Some(background) = background {
                builder = builder.background_color(rgb565(background));
            }
            Text::with_baseline(
                text,
                Point::new(x, y),
                builder.build(),
                Baseline::Top,
            )
            .draw(self)
            .unwrap();
        }
    }

    fn rgb565(raw: u16) -> Rgb565 {
        Rgb565::new(
            ((raw >> 11) & 0x1f) as u8,
            ((raw >> 5) & 0x3f) as u8,
            (raw & 0x1f) as u8,
        )
    }

    impl OriginDimensions for Display {
        fn size(&self) -> Size {
            Size::new(WIDTH as u32, HEIGHT as u32)
        }
    }

    impl DrawTarget for Display {
        type Color = Rgb565;
        type Error = Infallible;

        fn draw_iter<I>(&mut self, pixels: I) -> Result<(), Self::Error>
        where
            I: IntoIterator<Item = Pixel<Self::Color>>,
        {
            let framebuffer = self.pixels_mut();
            for Pixel(point, colour) in pixels {
                if point.x >= 0
                    && point.y >= 0
                    && point.x < WIDTH as i32
                    && point.y < HEIGHT as i32
                {
                    framebuffer[point.y as usize * WIDTH + point.x as usize] =
                        colour.into_storage();
                }
            }
            Ok(())
        }
    }
}
