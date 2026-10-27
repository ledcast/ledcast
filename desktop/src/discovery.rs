use std::collections::BTreeSet;
use std::time::{Duration, Instant};

use mdns_sd::{ServiceDaemon, ServiceEvent};

pub trait Discovery {
    fn scan(&self, timeout: Duration) -> Result<Vec<String>, String>;
}

pub struct MdnsDiscovery;

impl Discovery for MdnsDiscovery {
    fn scan(&self, timeout: Duration) -> Result<Vec<String>, String> {
        let daemon = ServiceDaemon::new().map_err(|error| error.to_string())?;
        let receiver = daemon
            .browse("_ledcast._tcp.local.")
            .map_err(|error| error.to_string())?;
        let deadline = Instant::now() + timeout;
        let mut hosts = BTreeSet::new();
        while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
            match receiver.recv_timeout(remaining.min(Duration::from_millis(150))) {
                Ok(ServiceEvent::ServiceResolved(info)) => {
                    for address in info.get_addresses() {
                        if address.is_ipv4() {
                            hosts.insert(address.to_ip_addr().to_string());
                        }
                    }
                }
                Ok(_) => {}
                Err(_) if Instant::now() >= deadline => break,
                Err(_) => {}
            }
        }
        let _ = daemon.stop_browse("_ledcast._tcp.local.");
        let _ = daemon.shutdown();
        Ok(hosts.into_iter().collect())
    }
}
