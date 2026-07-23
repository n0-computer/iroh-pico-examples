mod insecure_verifier;
mod iroh_echo;
mod quic_crypto_provider;
mod std_dns_resolver;

fn main() {
    iroh_echo::main(iroh_echo::Config {
        relay: true,
        psram_heap: true,
    });
}
