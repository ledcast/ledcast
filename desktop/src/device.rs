use std::fmt;
use std::time::Duration;

use serde::Deserialize;

use crate::settings::Node;

/// Status polls run off the console thread, one at a time per host, so a long
/// timeout costs nothing elsewhere. While receiving video the ESP has been
/// seen answering `/api/v1/status` only after 8 s; 10 s still lets that
/// answer count instead of discarding it as a failure.
pub const STATUS_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct DisplayStatus {
    pub brightness: u8,
    pub frame_rate: u16,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(default)]
pub struct DeviceStatus {
    pub id: String,
    pub name: String,
    pub firmware: String,
    pub modules: u16,
    pub width: u16,
    pub height: u16,
    pub rssi: Option<i16>,
    pub signal: bool,
    pub fps: f32,
    pub owner: Option<String>,
    pub bit_depth: u8,
    pub display: DisplayStatus,
    pub show_enabled: Option<bool>,
    pub show_control: bool,
    /// Тестовое изображение на строке; пусто — теста нет.
    pub test_pattern: String,
}

/// Why a request to a line failed. 401 and 429 matter to the caller: the
/// first means the session or the key is no longer valid, the second that
/// the line's login rate limit is hit.
#[derive(Debug, PartialEq)]
pub enum Failure {
    Unauthorized,
    RateLimited,
    Other(String),
}

impl Failure {
    fn from_ureq(host: &str, error: ureq::Error) -> Self {
        match error {
            ureq::Error::StatusCode(401) => Self::Unauthorized,
            ureq::Error::StatusCode(429) => Self::RateLimited,
            other => Self::Other(format!("{host}: {other}")),
        }
    }
}

impl fmt::Display for Failure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Unauthorized => f.write_str("Строка не приняла доступ Console"),
            Self::RateLimited => f.write_str("Слишком много попыток входа — подождите минуту"),
            Self::Other(message) => f.write_str(message),
        }
    }
}

/// Session token the line issued, as returned by the login endpoints.
#[derive(Debug, Deserialize)]
pub struct Session {
    pub token: String,
    pub expires_in: u64,
}

pub struct Client<'a> {
    host: &'a str,
    token: Option<&'a str>,
}

impl<'a> Client<'a> {
    pub fn new(host: &'a str) -> Self {
        Self { host, token: None }
    }

    /// Client whose mutating requests carry `Authorization: Bearer <token>`.
    pub fn with_token(host: &'a str, token: &'a str) -> Self {
        Self {
            host,
            token: Some(token),
        }
    }

    fn put(&self, path: &str) -> ureq::RequestBuilder<ureq::typestate::WithBody> {
        let request = ureq::put(format!("http://{}{path}", self.host))
            .config()
            .timeout_global(Some(Duration::from_secs(2)))
            .build();
        match self.token {
            Some(token) => request.header("Authorization", format!("Bearer {token}")),
            None => request,
        }
    }

    /// One-time nonce for the console login, valid for about 30 s.
    pub fn challenge(&self) -> Result<String, Failure> {
        #[derive(Deserialize)]
        struct Reply {
            nonce: String,
        }
        let url = format!("http://{}/api/v1/auth/challenge", self.host);
        let mut response = ureq::get(&url)
            .config()
            .timeout_global(Some(Duration::from_secs(3)))
            .build()
            .call()
            .map_err(|error| Failure::from_ureq(self.host, error))?;
        response
            .body_mut()
            .read_json::<Reply>()
            .map(|reply| reply.nonce)
            .map_err(|error| Failure::Other(format!("{}: {error}", self.host)))
    }

    pub fn console_login(
        &self,
        nonce: &str,
        proof: &str,
        console: &str,
    ) -> Result<Session, Failure> {
        let url = format!("http://{}/api/v1/auth/console", self.host);
        let mut response = ureq::post(&url)
            .config()
            .timeout_global(Some(Duration::from_secs(3)))
            .build()
            .send_json(serde_json::json!({ "nonce": nonce, "proof": proof, "console": console }))
            .map_err(|error| Failure::from_ureq(self.host, error))?;
        response
            .body_mut()
            .read_json()
            .map_err(|error| Failure::Other(format!("{}: {error}", self.host)))
    }

    pub fn status(&self) -> Result<DeviceStatus, String> {
        let url = format!("http://{}/api/v1/status", self.host);
        let request = ureq::get(&url)
            .config()
            .timeout_global(Some(STATUS_TIMEOUT))
            .build();
        let mut response = request
            .call()
            .map_err(|error| format!("{}: {error}", self.host))?;
        response
            .body_mut()
            .read_json()
            .map_err(|error| format!("{}: {error}", self.host))
    }

    /// Строка ещё помнит сессию этого клиента: после перезагрузки она их теряет.
    pub fn session_alive(&self) -> Result<bool, Failure> {
        #[derive(Deserialize)]
        struct Reply {
            authenticated: bool,
        }
        let request = ureq::get(format!("http://{}/api/v1/auth/session", self.host))
            .config()
            .timeout_global(Some(Duration::from_secs(2)))
            .build();
        let request = match self.token {
            Some(token) => request.header("Authorization", format!("Bearer {token}")),
            None => request,
        };
        let mut response = request
            .call()
            .map_err(|error| Failure::from_ureq(self.host, error))?;
        response
            .body_mut()
            .read_json::<Reply>()
            .map(|reply| reply.authenticated)
            .map_err(|error| Failure::Other(format!("{}: {error}", self.host)))
    }

    pub fn set_brightness(&self, value: u8) -> Result<(), Failure> {
        self.put("/api/v1/brightness")
            .send_json(serde_json::json!({ "value": value }))
            .map(|_| ())
            .map_err(|error| Failure::from_ureq(self.host, error))
    }

    pub fn set_show_enabled(&self, enabled: bool) -> Result<bool, Failure> {
        #[derive(Deserialize)]
        struct Reply {
            enabled: bool,
        }
        let mut response = self
            .put("/api/v1/show/enabled")
            .send_json(serde_json::json!({ "enabled": enabled }))
            .map_err(|error| Failure::from_ureq(self.host, error))?;
        response
            .body_mut()
            .read_json::<Reply>()
            .map(|reply| reply.enabled)
            .map_err(|error| Failure::Other(format!("{}: {error}", self.host)))
    }
}

pub fn node_from_status(host: &str, status: DeviceStatus) -> Node {
    Node {
        id: if status.id.is_empty() {
            host.to_owned()
        } else {
            status.id
        },
        name: if status.name.is_empty() {
            format!("LEDCAST {host}")
        } else {
            status.name
        },
        host: host.to_owned(),
        width: status.width,
        height: status.height,
        modules: status.modules,
        firmware: status.firmware,
        rssi: status.rssi,
        bit_depth: status.bit_depth.max(1),
        frame_rate: status.display.frame_rate,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;

    #[test]
    fn show_capability_is_explicit() {
        let legacy: DeviceStatus = serde_json::from_str("{}").unwrap();
        assert!(!legacy.show_control);
        assert_eq!(legacy.show_enabled, None);
        let modern: DeviceStatus =
            serde_json::from_str(r#"{"show_control":true,"show_enabled":false}"#).unwrap();
        assert!(modern.show_control);
        assert_eq!(modern.show_enabled, Some(false));
    }

    #[test]
    fn show_request_uses_confirmed_reply_not_requested_value() {
        let server = TcpListener::bind("127.0.0.1:0").unwrap();
        let host = server.local_addr().unwrap().to_string();
        let worker = std::thread::spawn(move || {
            let (mut stream, _) = server.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(2)))
                .unwrap();
            let mut raw = Vec::new();
            let mut chunk = [0; 1024];
            loop {
                let n = stream.read(&mut chunk).unwrap();
                assert!(n > 0);
                raw.extend_from_slice(&chunk[..n]);
                let text = String::from_utf8_lossy(&raw);
                if let Some(end) = text.find("\r\n\r\n") {
                    let length = text[..end]
                        .lines()
                        .find_map(|line| {
                            let (key, value) = line.split_once(':')?;
                            key.eq_ignore_ascii_case("content-length")
                                .then(|| value.trim().parse::<usize>().unwrap())
                        })
                        .unwrap();
                    if raw.len() >= end + 4 + length {
                        break;
                    }
                }
            }
            let text = String::from_utf8(raw).unwrap();
            assert!(text.starts_with("PUT /api/v1/show/enabled HTTP/1.1"));
            assert!(text
                .lines()
                .any(|line| line.eq_ignore_ascii_case("authorization: Bearer secret")));
            let body = text.split_once("\r\n\r\n").unwrap().1;
            assert_eq!(
                serde_json::from_str::<serde_json::Value>(body).unwrap(),
                serde_json::json!({"enabled":true})
            );
            let body = r#"{"enabled":false}"#;
            write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body).unwrap();
        });
        assert!(!Client::with_token(&host, "secret")
            .set_show_enabled(true)
            .unwrap());
        worker.join().unwrap();
    }
}
