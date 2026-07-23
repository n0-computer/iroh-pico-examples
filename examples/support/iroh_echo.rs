use std::{ffi::CString, net::Ipv4Addr, sync::Arc, thread, time::Duration};

use iroh::endpoint::{presets, PortmapperConfig, QuicTransportConfig, VarInt};
use iroh_tickets::endpoint::EndpointTicket;
use pico_std as _;
use tracing::Level;
use tracing_subscriber::{filter::Targets, prelude::*};

#[path = "../quic_crypto_provider.rs"]
mod quic_crypto_provider;

const ECHO_ALPN: &[u8] = b"echo/0";
const WIFI_CONFIG: &str = match option_env!("WIFI_CONFIG") {
    Some(value) => value,
    None => panic!("WIFI_CONFIG is not set; use WIFI_CONFIG='SSID:PASSWORD'"),
};

extern "C" {
    fn presto_wifi_connect(
        ssid: *const core::ffi::c_char,
        password: *const core::ffi::c_char,
    ) -> core::ffi::c_int;
    fn presto_wifi_ipv4_octets(octets: *mut u8);
    fn presto_memory_report(label: *const core::ffi::c_char);
}

fn memory_report(label: &'static core::ffi::CStr) {
    unsafe { presto_memory_report(label.as_ptr()) };
}

fn connect_wifi() {
    let (ssid, password) = WIFI_CONFIG
        .split_once(':')
        .expect("WIFI_CONFIG must be SSID:PASSWORD");
    let ssid = CString::new(ssid).expect("SSID contains a NUL byte");
    let password = CString::new(password).expect("password contains a NUL byte");
    let result = unsafe { presto_wifi_connect(ssid.as_ptr(), password.as_ptr()) };
    assert_eq!(result, 0, "Wi-Fi connection failed with code {result}");
}

pub fn main(relay: bool) {
    if cfg!(feature = "psram-heap") {
        let heap_probe = vec![0xa5u8; 64 * 1024];
        let address = heap_probe.as_ptr() as usize;
        assert!(
            (0x1100_0000..0x1180_0000).contains(&address),
            "Rust allocation did not come from PSRAM: 0x{address:08x}"
        );
        println!(
            "PSRAM Rust heap probe: 64 KiB at 0x{address:08x}, checksum={}",
            heap_probe.iter().map(|&byte| byte as usize).sum::<usize>()
        );
    }

    for seconds in (1..=10).rev() {
        println!("Iroh echo starts in {seconds}");
        thread::sleep(Duration::from_secs(1));
    }

    connect_wifi();
    init_tracing();
    println!(
        "Starting {} iroh echo endpoint",
        if relay { "full relay-enabled" } else { "direct-only" }
    );

    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("failed to create Tokio runtime");
    runtime.block_on(run(relay));
}

fn init_tracing() {
    let targets = Targets::new()
        .with_default(Level::INFO)
        .with_target("iroh", Level::TRACE)
        .with_target("noq", Level::TRACE)
        .with_target("noq_proto", Level::TRACE)
        .with_target("noq_udp", Level::TRACE)
        .with_target("netwatch", Level::TRACE)
        .with_target("mio", Level::TRACE)
        .with_target("tokio", Level::TRACE)
        .with_target("rustls", Level::TRACE);

    tracing_subscriber::registry()
        .with(
            tracing_subscriber::fmt::layer()
                .without_time()
                .with_ansi(false)
                .with_writer(std::io::stdout),
        )
        .with(targets)
        .init();
}

async fn run(relay: bool) {
    let transport = QuicTransportConfig::builder()
        .max_concurrent_bidi_streams(VarInt::from_u32(1))
        .max_concurrent_uni_streams(VarInt::from_u32(0))
        .stream_receive_window(VarInt::from_u32(4 * 1024))
        .receive_window(VarInt::from_u32(8 * 1024))
        .send_window(8 * 1024)
        .datagram_receive_buffer_size(None)
        .build();

    let mut builder = iroh::Endpoint::builder(presets::Empty)
        .alpns(vec![ECHO_ALPN.to_vec()])
        .crypto_provider(Arc::new(quic_crypto_provider::provider()))
        .portmapper_config(PortmapperConfig::Disabled);
    if relay {
        builder = builder
            .relay_mode(iroh::RelayMode::Default)
            .address_lookup(iroh::address_lookup::PkarrPublisher::n0_dns())
            .address_lookup(iroh::address_lookup::DnsAddressLookup::n0_dns());
    } else {
        builder = builder
            .transport_config(transport)
            .max_tls_tickets(0)
            .relay_mode(iroh::RelayMode::Disabled);
    }
    let endpoint = builder
        .bind()
        .await
        .expect("failed to bind iroh endpoint");

    let endpoint_id = endpoint.addr().id;
    let port = endpoint
        .bound_sockets()
        .first()
        .map(|socket| socket.port())
        .expect("iroh endpoint has no bound socket");
    let mut ip_octets = [0; 4];
    unsafe { presto_wifi_ipv4_octets(ip_octets.as_mut_ptr()) };
    let wifi_ip = Ipv4Addr::from(ip_octets);

    let short_ticket = EndpointTicket::new(iroh::EndpointAddr::new(endpoint_id));
    let mut addr_with_ip = endpoint.addr();
    addr_with_ip
        .addrs
        .insert(iroh::TransportAddr::Ip((wifi_ip, port).into()));
    let long_ticket = EndpointTicket::new(addr_with_ip);

    println!("Iroh endpoint ID: {endpoint_id}");
    println!("Listening on: {wifi_ip}:{port}");
    println!("Short ticket: {short_ticket}");
    println!("Long ticket:  {long_ticket}");
    if relay {
        println!("Default relay and n0 DNS discovery are enabled");
    } else {
        println!("Relay and discovery are disabled; use the long ticket for a direct connection");
    }
    memory_report(c"endpoint-bound");

    while let Some(incoming) = endpoint.accept().await {
        println!("Incoming QUIC attempt from {:?}; probing Initial decryption", incoming.remote_addr());
        let decrypted = incoming.decrypt();
        println!(
            "Initial decryption probe {}",
            if decrypted.is_some() { "succeeded" } else { "failed" }
        );
        println!("Accepting initial packet");
        memory_report(c"before-incoming-accept");
        let accepting = match incoming.accept() {
            Ok(accepting) => accepting,
            Err(error) => {
                println!("Initial QUIC accept failed: {error}");
                continue;
            }
        };
        memory_report(c"after-incoming-accept");
        println!("Initial packet accepted; waiting for QUIC/TLS handshake");

        match accepting.await {
            Ok(connection) => {
                println!("Accepted connection from {}", connection.remote_id());
                if let Err(error) = echo(connection).await {
                    println!("Echo connection failed: {error}");
                }
            }
            Err(error) => println!("Incoming connection failed: {error}"),
        }
    }
}

async fn echo(connection: iroh::endpoint::Connection) -> Result<(), Box<dyn std::error::Error>> {
    let (mut send, mut recv) = connection.accept_bi().await?;
    let mut buffer = [0u8; 1024];
    let mut total = 0u64;
    loop {
        let Some(count) = recv.read(&mut buffer).await? else {
            break;
        };
        send.write_all(&buffer[..count]).await?;
        total += count as u64;
    }
    send.finish()?;
    println!("Echoed {total} byte(s)");
    connection.closed().await;
    Ok(())
}
