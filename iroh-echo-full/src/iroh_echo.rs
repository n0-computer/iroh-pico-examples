use std::{
    ffi::CString,
    sync::{Arc, Mutex},
    time::Duration,
};

use iroh::Watcher;
use iroh::endpoint::{presets, PortmapperConfig, QuicTransportConfig, VarInt};
#[cfg(feature = "relay")]
use iroh::tls::CaTlsConfig;
use iroh_tickets::endpoint::EndpointTicket;
use n0_future::StreamExt;
use pico_std::display::{Display, HEIGHT};
use pico_std::outputs::{AmbientLeds, AMBIENT_LED_COUNT};
use pico_std as _;
use tokio::sync::mpsc;
use tracing::Level;
use tracing_subscriber::{filter::Targets, prelude::*};

use crate::{insecure_verifier, quic_crypto_provider, std_dns_resolver};

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
    #[cfg(feature = "relay")]
    fn presto_time_sync() -> core::ffi::c_int;
    fn presto_memory_report(label: *const core::ffi::c_char);
    fn esp_fill_random(buffer: *mut core::ffi::c_void, length: usize);
}

/// Load the endpoint secret from flash, or generate and persist one on first
/// boot so the endpoint id stays stable across restarts. Must run before WiFi
/// and the display start, because the first-boot flash write stalls XIP.
fn load_or_create_secret() -> iroh::SecretKey {
    if let Some(bytes) = pico_std::secret::load() {
        println!("Loaded endpoint secret from flash");
        return iroh::SecretKey::from_bytes(&bytes);
    }
    let mut bytes = [0u8; 32];
    unsafe { esp_fill_random(bytes.as_mut_ptr() as *mut core::ffi::c_void, bytes.len()) };
    match pico_std::secret::store(&bytes) {
        Ok(()) => println!("Generated a new endpoint secret and stored it in flash"),
        Err(code) => println!(
            "Warning: storing the endpoint secret failed with code {code}; \
             the endpoint id will change on restart"
        ),
    }
    iroh::SecretKey::from_bytes(&bytes)
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

struct Outputs {
    leds: AmbientLeds,
    led_state: LedState,
    active_connections: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum LedState {
    Red,
    Yellow,
    Green,
}

impl Outputs {
    fn new() -> Self {
        let mut leds = AmbientLeds::new();
        let status = leds.status();
        if status != 0 {
            println!("Outputs: LED init status={status}");
        }
        let led_state = LedState::Red;
        apply_led_state(&mut leds, led_state);

        Self {
            leds,
            led_state,
            active_connections: 0,
        }
    }

    fn set_addr_state(&mut self, endpoint_addr: &iroh::EndpointAddr) {
        let next_state = if endpoint_addr.relay_urls().next().is_some() {
            LedState::Green
        } else if !endpoint_addr.addrs.is_empty() {
            LedState::Yellow
        } else {
            LedState::Red
        };

        if next_state != self.led_state {
            self.led_state = next_state;
            self.refresh_leds();
        }
    }

    fn connection_started(&mut self) {
        self.active_connections = self.active_connections.saturating_add(1);
        self.refresh_leds();
    }

    fn connection_finished(&mut self) {
        self.active_connections = self.active_connections.saturating_sub(1);
        self.refresh_leds();
    }

    fn refresh_leds(&mut self) {
        if self.active_connections > 0 {
            set_all_leds(&mut self.leds, 40, 40, 40);
        } else {
            apply_led_state(&mut self.leds, self.led_state);
        }
    }
}

struct ConnectionLedGuard {
    outputs: Arc<Mutex<Outputs>>,
}

impl ConnectionLedGuard {
    fn start(outputs: Arc<Mutex<Outputs>>) -> Self {
        if let Ok(mut state) = outputs.lock() {
            state.connection_started();
        }
        Self { outputs }
    }
}

#[derive(Clone, Debug)]
enum UiEvent {
    ConnectionEstablished(String),
    ConnectionEnded { established: bool },
}

struct UiEventGuard {
    tx: mpsc::UnboundedSender<UiEvent>,
    established: bool,
}

impl UiEventGuard {
    fn new(tx: mpsc::UnboundedSender<UiEvent>) -> Self {
        Self {
            tx,
            established: false,
        }
    }

    fn mark_established(&mut self, remote: String) {
        self.established = true;
        let _ = self.tx.send(UiEvent::ConnectionEstablished(remote));
    }
}

impl Drop for UiEventGuard {
    fn drop(&mut self) {
        let _ = self
            .tx
            .send(UiEvent::ConnectionEnded { established: self.established });
    }
}

#[derive(Clone, Debug)]
struct DisplayState {
    endpoint_addr: iroh::EndpointAddr,
    long_ticket: String,
    active_connections: usize,
    last_connection_from: Option<String>,
}

impl DisplayState {
    fn new(endpoint_addr: iroh::EndpointAddr) -> Self {
        let long_ticket = EndpointTicket::new(endpoint_addr.clone()).to_string();
        Self {
            endpoint_addr,
            long_ticket,
            active_connections: 0,
            last_connection_from: None,
        }
    }

    fn apply_addr_update(&mut self, endpoint_addr: iroh::EndpointAddr) {
        self.long_ticket = EndpointTicket::new(endpoint_addr.clone()).to_string();
        self.endpoint_addr = endpoint_addr;
    }

    fn apply_ui_event(&mut self, event: UiEvent) {
        match event {
            UiEvent::ConnectionEstablished(remote) => {
                self.active_connections = self.active_connections.saturating_add(1);
                self.last_connection_from = Some(remote);
            }
            UiEvent::ConnectionEnded { established } => {
                if established {
                    self.active_connections = self.active_connections.saturating_sub(1);
                }
                if self.active_connections == 0 {
                    self.last_connection_from = None;
                }
            }
        }
    }
}

impl Drop for ConnectionLedGuard {
    fn drop(&mut self) {
        if let Ok(mut state) = self.outputs.lock() {
            state.connection_finished();
        }
    }
}

fn apply_led_state(leds: &mut AmbientLeds, state: LedState) {
    let (red, green, blue) = match state {
        LedState::Red => (40, 0, 0),
        LedState::Yellow => (40, 40, 0),
        LedState::Green => (0, 40, 0),
    };

    set_all_leds(leds, red, green, blue);
}

fn set_all_leds(leds: &mut AmbientLeds, red: u8, green: u8, blue: u8) {

    for index in 0..AMBIENT_LED_COUNT {
        leds.set_rgb(index, red, green, blue);
    }
    if let Err(code) = leds.show() {
        println!("Outputs: LED show failed with code {code}");
    }
}

#[derive(Clone, Copy)]
pub struct Config {
    pub relay: bool,
    pub psram_heap: bool,
}

pub fn main(config: Config) {
    if config.psram_heap {
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

    let secret_key = load_or_create_secret();

    let mut display = Display::start().expect("display initialization failed");
    let outputs = Arc::new(Mutex::new(Outputs::new()));
    display.fill(0x0000);
    display.draw_text(12, 12, "IROH ECHO FULL", 0xffff, Some(0x0000));
    display.draw_text(12, 26, "STARTING...", 0xffff, Some(0x0000));
    if let Err(code) = display.present() {
        println!("Display: initial present failed with code {code}; continuing");
    }
    println!("Display scanout started");

    connect_wifi();
    display.fill(0x0000);
    display.draw_text(12, 12, "IROH ECHO FULL", 0xffff, None);
    display.draw_text(12, 28, "WI-FI CONNECTED", 0x07e0, None);
    display.draw_text(12, 44, "STARTING ENDPOINT...", 0xffff, None);
    if let Err(code) = display.present() {
        println!("Display: status present failed with code {code}; continuing");
    }
    if config.relay {
        #[cfg(not(feature = "relay"))]
        panic!("relay support was not compiled into iroh-echo-full");
        #[cfg(feature = "relay")]
        {
            let result = unsafe { presto_time_sync() };
            assert_eq!(result, 0, "wall-clock synchronization failed");
        }
    }
    init_tracing();
    println!(
        "Starting {} iroh echo endpoint",
        if config.relay {
            "full relay-enabled"
        } else {
            "direct-only"
        }
    );

    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .max_blocking_threads(1)
        .thread_stack_size(16 * 1024)
        .build()
        .expect("failed to create Tokio runtime");
    runtime.block_on(run(config.relay, secret_key, &mut display, outputs));
}

fn init_tracing() {
    let targets = Targets::new().with_default(Level::INFO);

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

async fn run(
    relay: bool,
    secret_key: iroh::SecretKey,
    display: &mut Display,
    outputs: Arc<Mutex<Outputs>>,
) {
    let transport = QuicTransportConfig::builder()
        .max_concurrent_bidi_streams(VarInt::from_u32(1))
        .max_concurrent_uni_streams(VarInt::from_u32(0))
        .stream_receive_window(VarInt::from_u32(4 * 1024))
        .receive_window(VarInt::from_u32(8 * 1024))
        .send_window(8 * 1024)
        .datagram_receive_buffer_size(None)
        .build();

    let mut builder = iroh::Endpoint::builder(presets::Empty)
        .secret_key(secret_key)
        .alpns(vec![ECHO_ALPN.to_vec()])
        .crypto_provider(Arc::new(quic_crypto_provider::provider()))
        .portmapper_config(PortmapperConfig::Disabled);
    if relay {
        #[cfg(feature = "relay")]
        {
            let dns_resolver =
                iroh::dns::DnsResolver::custom(std_dns_resolver::StdDnsResolver);
            builder = builder
                .ca_tls_config(CaTlsConfig::custom_server_cert_verifier(
                    insecure_verifier::skip_verify(),
                ))
                .dns_resolver(dns_resolver)
                .relay_mode(iroh::RelayMode::Default)
                .net_report_config(iroh::NetReportConfig::minimal())
                .address_lookup(iroh::address_lookup::PkarrPublisher::n0_dns())
                .address_lookup(iroh::address_lookup::PkarrResolver::n0_dns());
        }
        #[cfg(not(feature = "relay"))]
        unreachable!("relay support checked before starting the runtime");
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

    let local_addr = endpoint.addr();
    let endpoint_id = local_addr.id;
    let mut display_state = DisplayState::new(local_addr.clone());

    let short_ticket = EndpointTicket::new(iroh::EndpointAddr::new(endpoint_id));
    let local_long_ticket = EndpointTicket::new(local_addr.clone());

    println!("Iroh endpoint ID: {endpoint_id}");
    let listening_addrs = local_addr
        .ip_addrs()
        .map(ToString::to_string)
        .collect::<Vec<_>>();
    if listening_addrs.is_empty() {
        println!("Listening on: none reported yet");
    } else {
        println!("Listening on: {}", listening_addrs.join(", "));
    }
    println!("Short ticket: {short_ticket}");
    println!("Local long ticket: {local_long_ticket}");
    if let Ok(mut state) = outputs.lock() {
        state.set_addr_state(&local_addr);
    }
    render_display_state(display, &display_state);

    let (addr_update_tx, mut addr_update_rx) =
        tokio::sync::mpsc::channel::<iroh::EndpointAddr>(4);
    let mut addr_stream = endpoint.watch_addr().stream();
    tokio::spawn(async move {
        while let Some(current) = addr_stream.next().await {
            if addr_update_tx.send(current).await.is_err() {
                break;
            }
        }
    });

    if relay {
        println!("Default relay and n0 DNS discovery are enabled");
        println!("Watching endpoint address changes for relay/home-relay updates");
    } else {
        println!(
            "Relay and discovery are disabled; use the local long ticket for a direct connection"
        );
    }
    memory_report(c"endpoint-bound");

    let mut ticket_interval = tokio::time::interval(Duration::from_secs(30));
    let mut connection_tasks = tokio::task::JoinSet::new();
    let (ui_tx, mut ui_rx) = mpsc::unbounded_channel::<UiEvent>();
    // Skip the immediate tick; we already printed the ticket above.
    ticket_interval.tick().await;

    loop {
        tokio::select! {
            _ = ticket_interval.tick() => {
                let current_short_ticket = EndpointTicket::new(iroh::EndpointAddr::new(endpoint_id));
                let current_long_ticket = EndpointTicket::new(display_state.endpoint_addr.clone());
                println!(
                    "Ticket heartbeat: short={current_short_ticket} long={current_long_ticket}"
                );
            }
            ui_event = ui_rx.recv() => {
                let Some(ui_event) = ui_event else {
                    break;
                };
                display_state.apply_ui_event(ui_event);
                render_display_state(display, &display_state);
            }
            addr_update = addr_update_rx.recv() => {
                let Some(addr_update) = addr_update else {
                    break;
                };
                display_state.apply_addr_update(addr_update.clone());
                if let Ok(mut state) = outputs.lock() {
                    state.set_addr_state(&addr_update);
                }
                render_display_state(display, &display_state);
                if relay {
                    let home_relay = addr_update
                        .relay_urls()
                        .next()
                        .map(ToString::to_string)
                        .unwrap_or_else(|| "none".to_string());
                    println!("Addr update: home relay {home_relay}");
                }
            }
            incoming = endpoint.accept() => {
                let Some(incoming) = incoming else {
                    break;
                };
                let ui_tx_for_connection = ui_tx.clone();
                let outputs_for_connection = Arc::clone(&outputs);
                connection_tasks.spawn(async move {
                    let _guard = ConnectionLedGuard::start(outputs_for_connection);
                    let mut ui_guard = UiEventGuard::new(ui_tx_for_connection);
                    if let Err(error) = accept_and_echo(incoming, &mut ui_guard).await {
                        println!("Echo connection failed: {error}");
                    }
                });
            }
            done = connection_tasks.join_next(), if !connection_tasks.is_empty() => {
                if let Some(Err(error)) = done {
                    println!("Echo task aborted: {error}");
                }
            }
        }
    }
}

fn status_for_addr(endpoint_addr: &iroh::EndpointAddr) -> &'static str {
    if endpoint_addr.relay_urls().next().is_some() {
        "RELAY ONLINE"
    } else {
        "LOCAL ENDPOINT"
    }
}

fn render_display_state(display: &mut Display, state: &DisplayState) {
    let status = status_for_addr(&state.endpoint_addr);
    let endpoint_addr = &state.endpoint_addr;
    let ticket = state.long_ticket.as_str();

    display.fill(0x0000);
    display.draw_text(12, 10, "IROH ECHO FULL", 0xffff, None);
    display.draw_text(12, 24, status, 0x07e0, None);
    let home_relay = endpoint_addr
        .relay_urls()
        .next()
        .map(ToString::to_string)
        .unwrap_or_else(|| "none".to_string());
    display.draw_text(
        12,
        38,
        &format!("HOME RELAY: {home_relay}"),
        0xffe0,
        None,
    );

    let connection_line = if state.active_connections > 0 {
        format!(
            "CONNECTION FROM: {}",
            state
                .last_connection_from
                .as_deref()
                .unwrap_or("(pending)")
        )
    } else {
        "CONNECTION FROM: none".to_string()
    };
    display.draw_text(12, 52, &connection_line, 0xffff, None);

    let mut address_lines = Vec::new();
    for addr in &endpoint_addr.addrs {
        push_wrapped_line(&mut address_lines, &format!("{addr}"), 58);
    }
    if address_lines.is_empty() {
        address_lines.push("none".to_string());
    }

    display.draw_text(12, 66, "ENDPOINT ADDRS:", 0xffff, None);
    let max_addr_lines = 8usize;
    let shown_addr_lines = address_lines.len().min(max_addr_lines);
    for (line, text) in address_lines.iter().take(shown_addr_lines).enumerate() {
        display.draw_text(12, 80 + line as i32 * 14, text, 0x07ff, None);
    }

    let ticket_header_y = 80 + shown_addr_lines as i32 * 14 + 4;
    display.draw_text(12, ticket_header_y, "TICKET:", 0xffff, None);
    let ticket_start_y = ticket_header_y + 14;
    let max_ticket_lines = ((HEIGHT as i32 - ticket_start_y) / 14).max(0) as usize;

    for (line, bytes) in ticket.as_bytes().chunks(58).take(max_ticket_lines).enumerate() {
        let text = core::str::from_utf8(bytes).unwrap_or("?");
        display.draw_text(12, ticket_start_y + line as i32 * 14, text, 0xffff, None);
    }
    if let Err(code) = display.present() {
        println!("Display: update present failed with code {code}; continuing");
    }
}

fn push_wrapped_line(lines: &mut Vec<String>, text: &str, width: usize) {
    for chunk in text.as_bytes().chunks(width) {
        lines.push(String::from_utf8_lossy(chunk).into_owned());
    }
}

async fn accept_and_echo(
    incoming: iroh::endpoint::Incoming,
    ui_guard: &mut UiEventGuard,
) -> Result<(), Box<dyn std::error::Error>> {
    println!(
        "Incoming QUIC attempt from {:?}; probing Initial decryption",
        incoming.remote_addr()
    );
    let decrypted = incoming.decrypt();
    println!(
        "Initial decryption probe {}",
        if decrypted.is_some() { "succeeded" } else { "failed" }
    );
    println!("Accepting initial packet");
    memory_report(c"before-incoming-accept");
    let accepting = incoming.accept()?;
    memory_report(c"after-incoming-accept");
    println!("Initial packet accepted; waiting for QUIC/TLS handshake");

    let connection = accepting.await?;
    println!("Accepted connection from {}", connection.remote_id());
    ui_guard.mark_established(connection.remote_id().to_string());
    echo(connection).await
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
