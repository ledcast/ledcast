//! Имя строки в DNS роутера (`ledcast-a4dc.lan`, `.home`…): обратный запрос по её IP.
//!
//! Роутеры по-разному записывают имена из DHCP, а многие не записывают вовсе. Обратный
//! запрос отвечает ровно тем, что знает DNS этой сети: есть ответ — имя работает здесь.

use std::collections::HashMap;
use std::net::IpAddr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Без ответа строку спрашивают снова не раньше, чем через это время.
const RETRY: Duration = Duration::from_secs(60);

#[derive(Clone)]
enum Lookup {
    Pending,
    Found(String),
    Missing(Instant),
}

#[derive(Default)]
pub struct RouterNames {
    hosts: Arc<Mutex<HashMap<String, Lookup>>>,
}

impl RouterNames {
    /// Известное имя хоста; первый вызов (и повтор после неудачи) спрашивает DNS в фоне.
    pub fn get(&self, host: &str) -> Option<String> {
        let mut hosts = self.hosts.lock().unwrap();
        match hosts.get(host) {
            Some(Lookup::Found(name)) => return Some(name.clone()),
            Some(Lookup::Pending) => return None,
            Some(Lookup::Missing(at)) if at.elapsed() < RETRY => return None,
            _ => {}
        }
        let Ok(ip) = host.parse::<IpAddr>() else {
            return None;
        };
        hosts.insert(host.to_owned(), Lookup::Pending);
        let shared = Arc::clone(&self.hosts);
        let key = host.to_owned();
        let spawned = std::thread::Builder::new()
            .name("ledcast-router-name".into())
            .spawn(move || {
                let result = dns_lookup::lookup_addr(&ip)
                    .ok()
                    .and_then(|name| router_name(&name, &ip));
                let entry = result.map_or_else(|| Lookup::Missing(Instant::now()), Lookup::Found);
                shared.lock().unwrap().insert(key, entry);
            });
        if spawned.is_err() {
            hosts.insert(host.to_owned(), Lookup::Missing(Instant::now()));
        }
        None
    }
}

/// Имя от DNS роутера: не сам адрес и не `.local` — тот отвечает mDNS, а не роутер.
fn router_name(name: &str, ip: &IpAddr) -> Option<String> {
    let name = name.trim_end_matches('.');
    let local = name.to_ascii_lowercase().ends_with(".local");
    (!name.is_empty() && name != ip.to_string() && !local && name.parse::<IpAddr>().is_err())
        .then(|| name.to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keeps_only_names_from_the_router() {
        let ip: IpAddr = "192.168.1.181".parse().unwrap();
        assert_eq!(
            router_name("ledcast-a4dc.lan.", &ip).as_deref(),
            Some("ledcast-a4dc.lan")
        );
        assert_eq!(
            router_name("ledcast-a4dc.home", &ip).as_deref(),
            Some("ledcast-a4dc.home")
        );
        assert_eq!(router_name("ledcast-a4dc.local", &ip), None);
        assert_eq!(router_name("192.168.1.181", &ip), None);
        assert_eq!(router_name("", &ip), None);
    }
}
