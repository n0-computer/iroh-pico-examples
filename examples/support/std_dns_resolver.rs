//! Lightweight iroh DNS resolver backed by lwIP through `ToSocketAddrs`.

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, ToSocketAddrs};

use iroh::dns::{BoxIter, DnsError, Resolver, TxtRecordData};
use n0_error::e;
use n0_future::boxed::BoxFuture;
use tracing::{debug, warn};

#[derive(Debug, Clone)]
pub struct StdDnsResolver;

fn strip_fqdn_dot(host: &str) -> &str {
    host.strip_suffix('.').unwrap_or(host)
}

impl Resolver for StdDnsResolver {
    fn lookup_ipv4(&self, host: String) -> BoxFuture<Result<BoxIter<Ipv4Addr>, DnsError>> {
        Box::pin(async move {
            let query = format!("{}:0", strip_fqdn_dot(&host));
            debug!("[std-dns] IPv4 synchronous lookup: {query}");
            let addresses = query.to_socket_addrs().map_err(|error| {
                warn!("[std-dns] IPv4 lookup failed for {host}: {error}");
                e!(DnsError::NoResponse)
            })?;
            debug!("[std-dns] IPv4 synchronous lookup returned: {host}");
            let addresses: Vec<_> = addresses
                .filter_map(|address| match address.ip() {
                    IpAddr::V4(address) => Some(address),
                    IpAddr::V6(_) => None,
                })
                .collect();
            if addresses.is_empty() {
                Err(e!(DnsError::NoResponse))
            } else {
                debug!("[std-dns] IPv4 {host}: {addresses:?}");
                Ok(Box::new(addresses.into_iter()) as BoxIter<Ipv4Addr>)
            }
        })
    }

    fn lookup_ipv6(&self, host: String) -> BoxFuture<Result<BoxIter<Ipv6Addr>, DnsError>> {
        Box::pin(async move {
            let query = format!("{}:0", strip_fqdn_dot(&host));
            let addresses = query
                .to_socket_addrs()
                .map_err(|_| e!(DnsError::NoResponse))?;
            let addresses: Vec<_> = addresses
                .filter_map(|address| match address.ip() {
                    IpAddr::V6(address) => Some(address),
                    IpAddr::V4(_) => None,
                })
                .collect();
            if addresses.is_empty() {
                Err(e!(DnsError::NoResponse))
            } else {
                Ok(Box::new(addresses.into_iter()) as BoxIter<Ipv6Addr>)
            }
        })
    }

    fn lookup_txt(&self, host: String) -> BoxFuture<Result<BoxIter<TxtRecordData>, DnsError>> {
        Box::pin(async move {
            debug!("[std-dns] TXT lookup unsupported: {host}");
            Err(e!(DnsError::NoResponse))
        })
    }

    fn clear_cache(&self) {}

    fn reset(&self) -> Box<dyn Resolver> {
        Box::new(self.clone())
    }
}
