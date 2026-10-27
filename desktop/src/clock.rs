//! Часы Console: время показа кадров (pts) и ответы строкам на обмен метками
//! времени. Строка спрашивает время сама (`stream_clock.h` в прошивке) и по
//! нему ставит кадр на панель, поэтому все строки одной Console меняют кадр в
//! один и тот же момент.

use std::io;
use std::net::UdpSocket;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

const MAGIC: &[u8; 4] = b"LCK6";
const VERSION: u8 = 6;
const PACKET: usize = 40;
const KIND_REQUEST: u8 = 0;
const KIND_REPLY: u8 = 1;

fn epoch() -> Instant {
    static EPOCH: OnceLock<Instant> = OnceLock::new();
    // Час назад: момент захвата, снятый до первого обращения к часам, не
    // уходит в ноль.
    *EPOCH.get_or_init(|| {
        let now = Instant::now();
        now.checked_sub(Duration::from_secs(3600)).unwrap_or(now)
    })
}

/// Время на часах Console, наносекунды.
pub fn ns(at: Instant) -> u64 {
    at.saturating_duration_since(epoch()).as_nanos() as u64
}

/// Порт, на котором Console отвечает строкам. Ответчик один на процесс и
/// запускается при первом обращении.
pub fn port() -> io::Result<u16> {
    static PORT: OnceLock<Result<u16, String>> = OnceLock::new();
    PORT.get_or_init(|| {
        let socket = UdpSocket::bind("0.0.0.0:0").map_err(|error| error.to_string())?;
        let port = socket
            .local_addr()
            .map_err(|error| error.to_string())?
            .port();
        epoch();
        std::thread::Builder::new()
            .name("ledcast-clock".into())
            .spawn(move || serve(socket))
            .map_err(|error| error.to_string())?;
        Ok(port)
    })
    .clone()
    .map_err(io::Error::other)
}

fn serve(socket: UdpSocket) {
    let mut packet = [0u8; PACKET + 1];
    loop {
        match socket.recv_from(&mut packet) {
            Ok((len, from)) => {
                let received = ns(Instant::now());
                if let Some(reply) = reply(&packet[..len], received) {
                    let _ = socket.send_to(&reply, from);
                }
            }
            // Windows сообщает здесь о недоставленном прошлом ответе; ошибка
            // не должна крутить цикл вхолостую.
            Err(_) => std::thread::sleep(Duration::from_millis(1)),
        }
    }
}

/// Ответ на запрос: метки приёма и отправки по часам Console.
fn reply(request: &[u8], received_ns: u64) -> Option<[u8; PACKET]> {
    if request.len() != PACKET
        || &request[..4] != MAGIC
        || request[4] != VERSION
        || request[5] != KIND_REQUEST
    {
        return None;
    }
    let mut out = [0u8; PACKET];
    out.copy_from_slice(request);
    out[5] = KIND_REPLY;
    out[24..32].copy_from_slice(&received_ns.to_be_bytes());
    out[32..40].copy_from_slice(&ns(Instant::now()).to_be_bytes());
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(session: u64, t1_us: u64) -> [u8; PACKET] {
        let mut packet = [0u8; PACKET];
        packet[..4].copy_from_slice(MAGIC);
        packet[4] = VERSION;
        packet[5] = KIND_REQUEST;
        packet[8..16].copy_from_slice(&session.to_be_bytes());
        packet[16..24].copy_from_slice(&t1_us.to_be_bytes());
        packet
    }

    #[test]
    fn answers_requests_with_console_time() {
        let port = port().unwrap();
        let line = UdpSocket::bind("127.0.0.1:0").unwrap();
        line.set_read_timeout(Some(Duration::from_secs(1))).unwrap();
        let before = ns(Instant::now());
        line.send_to(&request(0x0102_0304_0506_0708, 777), ("127.0.0.1", port))
            .unwrap();
        let mut answer = [0u8; 64];
        let len = line.recv(&mut answer).unwrap();
        let after = ns(Instant::now());
        assert_eq!(len, PACKET);
        assert_eq!(&answer[..4], b"LCK6");
        assert_eq!(answer[4], 6);
        assert_eq!(answer[5], KIND_REPLY);
        assert_eq!(
            u64::from_be_bytes(answer[8..16].try_into().unwrap()),
            0x0102_0304_0506_0708
        );
        assert_eq!(u64::from_be_bytes(answer[16..24].try_into().unwrap()), 777);
        let t2 = u64::from_be_bytes(answer[24..32].try_into().unwrap());
        let t3 = u64::from_be_bytes(answer[32..40].try_into().unwrap());
        assert!(before <= t2 && t2 <= t3 && t3 <= after);
    }

    #[test]
    fn ignores_everything_but_requests() {
        let mut packet = request(1, 2);
        assert!(reply(&packet, 5).is_some());
        assert!(reply(&packet[..PACKET - 1], 5).is_none());
        packet[5] = KIND_REPLY;
        assert!(reply(&packet, 5).is_none());
        packet[5] = KIND_REQUEST;
        packet[4] = 5;
        assert!(reply(&packet, 5).is_none());
    }
}
