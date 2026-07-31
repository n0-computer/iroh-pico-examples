#[cfg(feature = "relay")]
mod insecure_verifier;
mod iroh_echo;
mod quic_crypto_provider;
#[cfg(feature = "relay")]
mod std_dns_resolver;

fn main() {
    iroh_echo::main(iroh_echo::Config {
        relay: false,
        psram_heap: false,
    });
}
