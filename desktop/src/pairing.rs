//! Console access to lines without asking for the password again.
//!
//! The key is `K = HMAC-SHA256(line password, "ledcast-console:" + line id)`.
//! The line recomputes it on every login and stores nothing, so a new line
//! password revokes every Console at once. The key never goes over the wire:
//! Console asks the line for a one-time nonce and answers with
//! `hex(HMAC-SHA256(K, nonce))`, getting a session token back. The password
//! is used once while pairing and is not stored; the keys live in
//! `keys.json` next to the Console settings, readable by the user only.

use std::collections::{HashMap, HashSet};
use std::time::{Duration, Instant};

use hmac::{Hmac, Mac};
use sha2::Sha256;

use crate::device::{Client, Failure, Session};

pub type Key = [u8; 32];

/// How Console introduces itself to the line on login.
const CONSOLE_NAME: &str = "LEDCAST Console";
/// A cached token is renewed this long before the line would expire it.
const TOKEN_MARGIN: Duration = Duration::from_secs(60);

pub const LOCKED: &str = "Строка не подключена к Console — введите её пароль";
pub const REJECTED: &str = "Пароль строки изменился — подключите её заново";

fn hmac_sha256(key: &[u8], message: &[u8]) -> Key {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("HMAC accepts any key length");
    mac.update(message);
    mac.finalize().into_bytes().into()
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

fn parse_hex(text: &str) -> Option<Key> {
    if text.len() != 64 || !text.is_ascii() {
        return None;
    }
    let mut key = [0; 32];
    for (index, byte) in key.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&text[index * 2..index * 2 + 2], 16).ok()?;
    }
    Some(key)
}

pub fn derive_key(password: &str, device_id: &str) -> Key {
    hmac_sha256(
        password.as_bytes(),
        format!("ledcast-console:{device_id}").as_bytes(),
    )
}

/// Answer to a challenge: the nonce is hashed as the 64 ASCII hex characters
/// the line sent.
pub fn proof(key: &Key, nonce: &str) -> String {
    hex(&hmac_sha256(key, nonce.as_bytes()))
}

/// Keys of paired lines: `keys.json` in the Console data folder, `{line id: hex key}`.
mod store {
    use std::collections::BTreeMap;
    use std::path::PathBuf;

    use super::{hex, parse_hex, Key};

    fn path() -> Result<PathBuf, String> {
        crate::settings::data_dir()
            .map(|dir| dir.join("keys.json"))
            .ok_or_else(|| "Нет папки данных Console".to_string())
    }

    fn read() -> BTreeMap<String, String> {
        path()
            .ok()
            .and_then(|path| std::fs::read_to_string(path).ok())
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default()
    }

    fn write(keys: &BTreeMap<String, String>) -> Result<(), String> {
        let path = path()?;
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).map_err(|error| error.to_string())?;
        }
        let text = serde_json::to_string_pretty(keys).map_err(|error| error.to_string())?;
        let mut options = std::fs::OpenOptions::new();
        options.write(true).create(true).truncate(true);
        #[cfg(unix)]
        std::os::unix::fs::OpenOptionsExt::mode(&mut options, 0o600);
        let mut file = options.open(&path).map_err(|error| error.to_string())?;
        std::io::Write::write_all(&mut file, text.as_bytes()).map_err(|error| error.to_string())
    }

    pub fn load(device_id: &str) -> Option<Key> {
        read().get(device_id).and_then(|text| parse_hex(text))
    }

    pub fn save(device_id: &str, key: &Key) -> Result<(), String> {
        let mut keys = read();
        keys.insert(device_id.to_owned(), hex(key));
        write(&keys).map_err(|error| format!("Ключ не сохранился: {error}"))
    }

    pub fn delete(device_id: &str) -> Result<(), String> {
        let mut keys = read();
        if keys.remove(device_id).is_none() {
            return Ok(());
        }
        write(&keys).map_err(|error| format!("Ключ не удалился: {error}"))
    }
}

/// Stored key of a paired line, read from disk: the output picks up a line
/// paired after the stream started.
pub fn stored_key(device_id: &str) -> Option<Key> {
    store::load(device_id)
}

/// Challenge → proof → session token. The line also accepts the stream from
/// the address that logged in, for as long as the session lives.
pub fn login(host: &str, key: &Key) -> Result<Session, Failure> {
    let client = Client::new(host);
    let nonce = client.challenge()?;
    client.console_login(&nonce, &proof(key, &nonce), CONSOLE_NAME)
}

/// Derives the key from the password, proves it to the line once and only
/// then stores it. Runs off the console thread.
pub fn pair(host: &str, device_id: &str, password: &str) -> Result<(Key, Session), String> {
    let status = Client::new(host).status()?;
    if status.id != device_id {
        return Err("По этому адресу отвечает другая строка — обновите список".into());
    }
    let key = derive_key(password, device_id);
    let session = login(host, &key).map_err(|failure| match failure {
        Failure::Unauthorized => "Неверный пароль".to_string(),
        other => other.to_string(),
    })?;
    store::save(device_id, &key)?;
    Ok((key, session))
}

struct Cached {
    token: String,
    renew_at: Instant,
}

/// Keys and sessions of all lines, owned by the console thread.
#[derive(Default)]
pub struct Access {
    /// `None` — looked up, no key stored.
    keys: HashMap<String, Option<Key>>,
    /// The line turned the stored key down: its password changed.
    rejected: HashSet<String>,
    sessions: HashMap<String, Cached>,
}

impl Access {
    /// Reads the stored key once per line; later calls cost a map lookup.
    pub fn load(&mut self, device_id: &str) {
        if !self.keys.contains_key(device_id) {
            self.keys
                .insert(device_id.to_owned(), store::load(device_id));
        }
    }

    pub fn paired(&self, device_id: &str) -> bool {
        self.keys.get(device_id).is_some_and(Option::is_some) && !self.rejected.contains(device_id)
    }

    pub fn paired_with(&mut self, device_id: &str, key: Key, session: Session) {
        self.keys.insert(device_id.to_owned(), Some(key));
        self.rejected.remove(device_id);
        self.remember(device_id, session);
    }

    pub fn forget(&mut self, device_id: &str) -> Result<(), String> {
        store::delete(device_id)?;
        self.keys.insert(device_id.to_owned(), None);
        self.rejected.remove(device_id);
        self.sessions.remove(device_id);
        Ok(())
    }

    fn remember(&mut self, device_id: &str, session: Session) {
        let lifetime = Duration::from_secs(session.expires_in).saturating_sub(TOKEN_MARGIN);
        self.sessions.insert(
            device_id.to_owned(),
            Cached {
                token: session.token,
                renew_at: Instant::now() + lifetime,
            },
        );
    }

    /// Cached session token, or a fresh one from a new login.
    pub fn token(&mut self, device_id: &str, host: &str) -> Result<String, String> {
        if let Some(cached) = self.sessions.get(device_id) {
            if Instant::now() < cached.renew_at {
                return Ok(cached.token.clone());
            }
        }
        self.sessions.remove(device_id);
        if self.rejected.contains(device_id) {
            return Err(REJECTED.into());
        }
        let Some(Some(key)) = self.keys.get(device_id).copied() else {
            return Err(LOCKED.into());
        };
        match login(host, &key) {
            Ok(session) => {
                let token = session.token.clone();
                self.remember(device_id, session);
                Ok(token)
            }
            Err(Failure::Unauthorized) => {
                self.rejected.insert(device_id.to_owned());
                Err(REJECTED.into())
            }
            Err(other) => Err(other.to_string()),
        }
    }

    /// Токен для окна контроллера. Окно само его не обновит, поэтому
    /// сохранённая сессия сначала проверяется: строка, которую перезагрузили,
    /// её уже не помнит, и окно попросило бы пароль.
    pub fn live_token(&mut self, device_id: &str, host: &str) -> Result<String, String> {
        if let Some(cached) = self.sessions.get(device_id) {
            if Instant::now() < cached.renew_at
                && Client::with_token(host, &cached.token).session_alive() == Ok(true)
            {
                return Ok(cached.token.clone());
            }
        }
        self.sessions.remove(device_id);
        self.token(device_id, host)
    }

    /// Runs a request with the line's session; a 401 (the line restarted
    /// and lost its sessions) logs in again and repeats it once.
    pub fn request<T>(
        &mut self,
        device_id: &str,
        host: &str,
        send: impl Fn(&Client) -> Result<T, Failure>,
    ) -> Result<T, String> {
        let token = self.token(device_id, host)?;
        match send(&Client::with_token(host, &token)) {
            Err(Failure::Unauthorized) => {
                self.sessions.remove(device_id);
                let token = self.token(device_id, host)?;
                send(&Client::with_token(host, &token)).map_err(|failure| failure.to_string())
            }
            result => result.map_err(|failure| failure.to_string()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Same vectors as firmware/test/test_console_auth.cpp and the device mock,
    // computed independently with Python's hmac.
    #[test]
    fn key_and_proof_match_the_firmware() {
        let key = derive_key("ledcast1", "ledcast-7f2a91");
        assert_eq!(
            hex(&key),
            "4e865ed217587b470a82e5c53c27188963cc2a4afd78afaac157a8935a544fb2"
        );
        assert_eq!(
            proof(
                &key,
                "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
            ),
            "c8d68caa93494443820bbde7ecf42483760ec68b17ce934649cb6522c0dbbb16"
        );
        assert_eq!(
            hex(&derive_key("88888888", "A0B1C2D3E4F5")),
            "35ca6fc7a96b71b89b46fa3d87f2bf81cc1e4e01db4e72df406118be54c887e4"
        );
    }

    #[test]
    fn stored_key_round_trips_through_hex() {
        let key = derive_key("ledcast1", "line");
        assert_eq!(parse_hex(&hex(&key)), Some(key));
        assert_eq!(parse_hex("zz"), None);
        assert_eq!(parse_hex(&"й".repeat(32)), None);
    }

    #[test]
    fn line_without_a_key_is_locked_without_network() {
        let mut access = Access::default();
        access.keys.insert("line".into(), None);
        assert!(!access.paired("line"));
        assert_eq!(access.token("line", "127.0.0.1:9"), Err(LOCKED.into()));
        access.keys.insert("line".into(), Some([7; 32]));
        assert!(access.paired("line"));
        access.rejected.insert("line".into());
        assert!(!access.paired("line"));
        assert_eq!(access.token("line", "127.0.0.1:9"), Err(REJECTED.into()));
    }
}
