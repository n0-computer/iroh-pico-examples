fn main() {
    iroh_echo_common::main(iroh_echo_common::Config {
        relay: false,
        psram_heap: false,
    });
}
