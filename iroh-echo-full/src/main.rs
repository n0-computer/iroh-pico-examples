fn main() {
    iroh_echo_common::main(iroh_echo_common::Config {
        relay: true,
        psram_heap: true,
    });
}
