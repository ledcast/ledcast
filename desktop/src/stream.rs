//! LEDCAST Frame v6: independent lossless frames over paced UDP, shown at a
//! presentation time on the Console clock (`clock.rs`).

use std::hash::{Hash, Hasher};
use std::io;
use std::net::{SocketAddr, ToSocketAddrs, UdpSocket};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub const DEFAULT_PORT: u16 = 4048;
pub const MAX_PAYLOAD: usize = 1200;
const MAGIC: &[u8; 4] = b"LCF6";
const VERSION: u8 = 6;
const KIND_DATA: u8 = 0;
const KIND_PARITY: u8 = 1;
const CODEC_RAW: u8 = 1;
const CODEC_LZ4: u8 = 2;
const CODEC_REPEAT: u8 = 3;
/// Столько повторов подряд, затем кадр уходит целиком. Повтор ссылается на
/// предыдущий кадр; если тот потерян, панель держит более старый, и полный
/// кадр ограничивает это четырьмя кадрами вывода.
const MAX_REPEATS: u8 = 3;
const FLAG_FEC: u8 = 1;
const HEADER: usize = 72;
/// IPv4 и UDP поверх каждого датаграма: их тоже несёт Wi-Fi.
const IP_UDP_HEADER: usize = 28;
const FEEDBACK: usize = 72;

#[derive(Clone, Copy, Debug, Default)]
pub struct Feedback {
    pub session_id: u64,
    pub stream_id: u32,
    pub highest_verified: u32,
    pub highest_committed: u32,
    pub packets_received: u32,
    pub incomplete: u32,
    pub fec_recovered: u32,
    pub crc_or_decode_errors: u32,
    pub assembly_evictions: u32,
    pub late: u32,
    pub assembly_depth: u16,
    pub commit_fps: u16,
    pub arrival_jitter_us_max: u32,
    pub flip_timeouts: u32,
    /// Сколько кадров строка держит до показа: потолок задержки показа.
    pub reservoir_frames: u16,
    pub clock_synced: bool,
    pub clock_rtt_us: u32,
}

pub(crate) fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &byte in bytes {
        crc ^= u32::from(byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

#[cfg(target_os = "macos")]
pub(crate) fn precise_wait(duration: Duration) {
    #[repr(C)]
    struct MachTimebaseInfo {
        numer: u32,
        denom: u32,
    }
    unsafe extern "C" {
        fn mach_absolute_time() -> u64;
        fn mach_wait_until(deadline: u64) -> i32;
        fn mach_timebase_info(info: *mut MachTimebaseInfo) -> i32;
    }
    let mut info = MachTimebaseInfo { numer: 0, denom: 0 };
    unsafe {
        if mach_timebase_info(&mut info) != 0 || info.numer == 0 {
            std::thread::sleep(duration);
            return;
        }
        let nanos = duration.as_nanos();
        let ticks = nanos
            .saturating_mul(u128::from(info.denom))
            .div_ceil(u128::from(info.numer))
            .min(u128::from(u64::MAX)) as u64;
        let _ = mach_wait_until(mach_absolute_time().saturating_add(ticks));
    }
}

#[cfg(not(target_os = "macos"))]
pub(crate) fn precise_wait(duration: Duration) {
    std::thread::sleep(duration);
}

fn wait_until(deadline: Instant) {
    let now = Instant::now();
    if deadline > now {
        precise_wait(deadline - now);
    }
}

pub struct LedcastOutput {
    socket: UdpSocket,
    target: SocketAddr,
    session_id: u64,
    stream_id: u32,
    frame_id: u32,
    packet: [u8; HEADER + MAX_PAYLOAD],
    parity: [u8; MAX_PAYLOAD],
    compressed: Vec<u8>,
    /// Кадр в том виде, в каком уходит в сеть.
    frame: Vec<u8>,
    /// Последний отправленный целиком кадр и номер последнего ушедшего кадра.
    previous: Vec<u8>,
    previous_id: Option<u32>,
    repeats: u8,
    /// Панель на 8 бит: значения с одинаковым уровнем ШИМ сводятся к одному.
    canonical: bool,
    send_buffer_frame_bytes: usize,
    clock_port: u16,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct SendStats {
    /// Радио не принимало пакеты: кадр брошен, ни одного байта не учтено.
    pub dropped: bool,
    pub packets: u32,
    pub payload_bytes: u32,
    pub wire_bytes: u32,
    pub compressed: bool,
}

impl LedcastOutput {
    pub fn open(host: &str, port: u16, bit_depth: u8) -> io::Result<Self> {
        let target = (host, port)
            .to_socket_addrs()?
            .find(SocketAddr::is_ipv4)
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "адрес не разобран"))?;
        let socket = UdpSocket::bind("0.0.0.0:0")?;
        // Wi-Fi на macOS замолкает на 100–300 мс (фоновое сканирование,
        // AWDL). Большой буфер копил за это время десятки кадров и выпускал
        // их одной пачкой на полной скорости: приёмник на ESP32 терял почти
        // всё, а его память уходила под очередь, и он переставал отвечать
        // даже на ping. Буфер — два кадра (`send_frame` подгоняет его под
        // размер кадра); кадр, не влезший в паузу радио, отбрасывается
        // целиком (`SendStats::dropped`): устаревший кадр никому не нужен.
        socket2::SockRef::from(&socket).set_send_buffer_size(2 * (HEADER + MAX_PAYLOAD))?;
        socket2::SockRef::from(&socket).set_recv_buffer_size(64 * 1024)?;
        socket.connect(target)?;
        socket.set_nonblocking(true)?;
        let session_id = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos() as u64
            ^ u64::from(std::process::id());
        let mut hasher = std::collections::hash_map::DefaultHasher::new();
        target.hash(&mut hasher);
        let stream_id = stream_id_from_hash(hasher.finish());
        Ok(Self {
            socket,
            target,
            session_id,
            stream_id,
            frame_id: 0,
            packet: [0; HEADER + MAX_PAYLOAD],
            parity: [0; MAX_PAYLOAD],
            compressed: Vec::new(),
            frame: Vec::new(),
            previous: Vec::new(),
            previous_id: None,
            repeats: 0,
            canonical: bit_depth == 8,
            send_buffer_frame_bytes: 0,
            clock_port: crate::clock::port()?,
        })
    }

    /// Буфер отправки полон — радио стоит. Это не ошибка вывода, а повод
    /// бросить кадр.
    fn would_block(result: io::Result<usize>) -> io::Result<bool> {
        match result {
            Ok(_) => Ok(false),
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => Ok(true),
            Err(error) => Err(error),
        }
    }

    pub fn target(&self) -> SocketAddr {
        self.target
    }

    pub fn poll_feedback(&self) -> io::Result<Option<Feedback>> {
        let mut packet = [0u8; FEEDBACK];
        let mut latest = None;
        loop {
            match self.socket.recv(&mut packet) {
                Ok(FEEDBACK)
                    if &packet[..4] == b"LCB6"
                        && packet[4] == 6
                        && usize::from(packet[5]) == FEEDBACK =>
                {
                    latest = Some(Feedback {
                        session_id: u64::from_be_bytes(packet[8..16].try_into().unwrap()),
                        stream_id: u32::from_be_bytes(packet[16..20].try_into().unwrap()),
                        highest_verified: u32::from_be_bytes(packet[20..24].try_into().unwrap()),
                        highest_committed: u32::from_be_bytes(packet[24..28].try_into().unwrap()),
                        packets_received: u32::from_be_bytes(packet[28..32].try_into().unwrap()),
                        incomplete: u32::from_be_bytes(packet[32..36].try_into().unwrap()),
                        fec_recovered: u32::from_be_bytes(packet[36..40].try_into().unwrap()),
                        crc_or_decode_errors: u32::from_be_bytes(
                            packet[40..44].try_into().unwrap(),
                        ),
                        assembly_evictions: u32::from_be_bytes(packet[44..48].try_into().unwrap()),
                        late: u32::from_be_bytes(packet[48..52].try_into().unwrap()),
                        assembly_depth: u16::from_be_bytes(packet[52..54].try_into().unwrap()),
                        commit_fps: u16::from_be_bytes(packet[54..56].try_into().unwrap()),
                        arrival_jitter_us_max: u32::from_be_bytes(
                            packet[56..60].try_into().unwrap(),
                        ),
                        flip_timeouts: u32::from_be_bytes(packet[60..64].try_into().unwrap()),
                        reservoir_frames: u16::from_be_bytes(packet[64..66].try_into().unwrap()),
                        clock_synced: packet[66] & 1 != 0,
                        clock_rtt_us: u32::from_be_bytes(packet[68..72].try_into().unwrap()),
                    });
                }
                Ok(_) => {}
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => return Ok(latest),
                Err(error) => return Err(error),
            }
        }
    }

    /// `pts_ns` — момент показа на часах Console (`clock::ns`). Пакеты кадра
    /// уходят сразу, равномерно в пределах трёх четвертей периода.
    pub fn send_frame(
        &mut self,
        pixels: &[u8],
        width: u16,
        height: u16,
        pts_ns: u64,
        rate_n: u32,
        rate_d: u32,
    ) -> io::Result<SendStats> {
        let expected = usize::from(width)
            .checked_mul(usize::from(height))
            .and_then(|n| n.checked_mul(3))
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "размер кадра переполнен")
            })?;
        if width == 0
            || height == 0
            || pixels.len() != expected
            || expected > u32::MAX as usize
            || rate_n == 0
            || rate_d == 0
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "недопустимая геометрия RGB888 или частота",
            ));
        }

        // Худший случай кадра на проводе — сырой RGB888, все пакеты и чётность.
        let frame_wire_bytes =
            expected + MAX_PAYLOAD + (expected.div_ceil(MAX_PAYLOAD) + 1) * HEADER;
        if frame_wire_bytes != self.send_buffer_frame_bytes {
            socket2::SockRef::from(&self.socket).set_send_buffer_size(2 * frame_wire_bytes)?;
            self.send_buffer_frame_bytes = frame_wire_bytes;
        }

        self.frame.clear();
        if self.canonical {
            self.frame.extend(
                pixels
                    .iter()
                    .map(|&value| PANEL_CANONICAL[usize::from(value)]),
            );
        } else {
            self.frame.extend_from_slice(pixels);
        }
        let repeat_of = self
            .previous_id
            .filter(|_| self.repeats < MAX_REPEATS && self.previous == self.frame);
        // До конца отправки опоры нет: брошенный или оборванный кадр панель
        // не получит, и повторять его нельзя.
        self.previous_id = None;
        self.frame_id = self.frame_id.wrapping_add(1);
        let reference = repeat_of.unwrap_or_default().to_be_bytes();
        let (codec, encoded): (u8, &[u8]) = if repeat_of.is_some() {
            (CODEC_REPEAT, &reference)
        } else {
            self.compressed = lz4_flex::block::compress(&self.frame);
            if self.compressed.len() + 16 < self.frame.len() {
                (CODEC_LZ4, &self.compressed)
            } else {
                (CODEC_RAW, &self.frame)
            }
        };
        let data_chunks = encoded.len().div_ceil(MAX_PAYLOAD);
        if data_chunks == 0 || data_chunks > u16::MAX as usize {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "слишком много UDP-фрагментов",
            ));
        }
        let packets = data_chunks + 1;
        let period_ns = (1_000_000_000u128 * u128::from(rate_d) / u128::from(rate_n))
            .clamp(1, u128::from(u64::MAX)) as u64;
        let frame_start = Instant::now();
        // Packets stay evenly paced inside the window instead of being
        // emitted as one burst.
        let transmit_window_ns = period_ns.saturating_mul(3) / 4;
        let packet_offset = |packet_index: usize| {
            Duration::from_nanos(
                transmit_window_ns.saturating_mul(packet_index as u64)
                    / (packets.saturating_sub(1).max(1) as u64),
            )
        };
        let frame_crc = crc32(&self.frame);
        let parity_len = encoded.len().min(MAX_PAYLOAD);
        self.parity.fill(0);
        for (index, byte) in encoded.iter().copied().enumerate() {
            self.parity[index % MAX_PAYLOAD] ^= byte;
        }

        for chunk in 0..data_chunks {
            let offset = chunk * MAX_PAYLOAD;
            let end = (offset + MAX_PAYLOAD).min(encoded.len());
            let payload = &encoded[offset..end];
            write_header(
                &mut self.packet[..HEADER],
                PacketMeta {
                    session_id: self.session_id,
                    stream_id: self.stream_id,
                    frame_id: self.frame_id,
                    kind: KIND_DATA,
                    codec,
                    payload_len: payload.len(),
                    pts_ns,
                    rate_n,
                    rate_d,
                    width,
                    height,
                    decoded_bytes: self.frame.len(),
                    encoded_bytes: encoded.len(),
                    frame_crc,
                    chunk_index: chunk as u16,
                    data_chunks: data_chunks as u16,
                    clock_port: self.clock_port,
                },
            );
            self.packet[HEADER..HEADER + payload.len()].copy_from_slice(payload);
            wait_until(frame_start + packet_offset(chunk));
            if Self::would_block(self.socket.send(&self.packet[..HEADER + payload.len()]))? {
                return Ok(SendStats {
                    dropped: true,
                    ..SendStats::default()
                });
            }
        }
        // Parity closes the frame only when a data fragment was actually
        // lost. If data was complete already, the receiver's recent-complete
        // cache discards this harmless tail without allocating a new slot.
        write_header(
            &mut self.packet[..HEADER],
            PacketMeta {
                session_id: self.session_id,
                stream_id: self.stream_id,
                frame_id: self.frame_id,
                kind: KIND_PARITY,
                codec,
                payload_len: parity_len,
                pts_ns,
                rate_n,
                rate_d,
                width,
                height,
                decoded_bytes: self.frame.len(),
                encoded_bytes: encoded.len(),
                frame_crc,
                chunk_index: data_chunks as u16,
                data_chunks: data_chunks as u16,
                clock_port: self.clock_port,
            },
        );
        self.packet[HEADER..HEADER + parity_len].copy_from_slice(&self.parity[..parity_len]);
        wait_until(frame_start + packet_offset(data_chunks));
        if Self::would_block(self.socket.send(&self.packet[..HEADER + parity_len]))? {
            return Ok(SendStats {
                dropped: true,
                ..SendStats::default()
            });
        }
        let payload_bytes = encoded.len() + parity_len;
        if repeat_of.is_some() {
            self.repeats += 1;
        } else {
            self.repeats = 0;
            std::mem::swap(&mut self.previous, &mut self.frame);
        }
        self.previous_id = Some(self.frame_id);
        Ok(SendStats {
            dropped: false,
            packets: packets as u32,
            payload_bytes: payload_bytes as u32,
            wire_bytes: (payload_bytes + packets * (HEADER + IP_UDP_HEADER)) as u32,
            compressed: codec != CODEC_RAW,
        })
    }
}

/// Обратная sRGB-кривая прошивки (`kGamma16`, components/colour/colour.cpp):
/// видеокод 8 бит -> линейная яркость 16 бит. Тест сверяет копию с прошивкой.
const PANEL_GAMMA16: [u16; 256] = [
    0, 20, 40, 60, 80, 99, 119, 139, 159, 179, 199, 219, 241, 264, 288, 313, 340, 367, 396, 427,
    458, 491, 526, 562, 599, 637, 677, 718, 761, 805, 851, 898, 947, 997, 1048, 1101, 1156, 1212,
    1270, 1330, 1391, 1453, 1517, 1583, 1651, 1720, 1790, 1863, 1937, 2013, 2090, 2170, 2250, 2333,
    2418, 2504, 2592, 2681, 2773, 2866, 2961, 3058, 3157, 3258, 3360, 3464, 3570, 3678, 3788, 3900,
    4014, 4129, 4247, 4366, 4488, 4611, 4736, 4864, 4993, 5124, 5257, 5392, 5530, 5669, 5810, 5953,
    6099, 6246, 6395, 6547, 6700, 6856, 7014, 7174, 7335, 7500, 7666, 7834, 8004, 8177, 8352, 8528,
    8708, 8889, 9072, 9258, 9445, 9635, 9828, 10022, 10219, 10417, 10619, 10822, 11028, 11235,
    11446, 11658, 11873, 12090, 12309, 12530, 12754, 12980, 13209, 13440, 13673, 13909, 14146,
    14387, 14629, 14874, 15122, 15371, 15623, 15878, 16135, 16394, 16656, 16920, 17187, 17456,
    17727, 18001, 18277, 18556, 18837, 19121, 19407, 19696, 19987, 20281, 20577, 20876, 21177,
    21481, 21787, 22096, 22407, 22721, 23038, 23357, 23678, 24002, 24329, 24658, 24990, 25325,
    25662, 26001, 26344, 26688, 27036, 27386, 27739, 28094, 28452, 28813, 29176, 29542, 29911,
    30282, 30656, 31033, 31412, 31794, 32179, 32567, 32957, 33350, 33745, 34143, 34544, 34948,
    35355, 35764, 36176, 36591, 37008, 37429, 37852, 38278, 38706, 39138, 39572, 40009, 40449,
    40891, 41337, 41785, 42236, 42690, 43147, 43606, 44069, 44534, 45002, 45473, 45947, 46423,
    46903, 47385, 47871, 48359, 48850, 49344, 49841, 50341, 50844, 51349, 51858, 52369, 52884,
    53401, 53921, 54445, 54971, 55500, 56032, 56567, 57105, 57646, 58190, 58737, 59287, 59840,
    60396, 60955, 61517, 62082, 62650, 63221, 63795, 64372, 64952, 65535,
];

/// Уровень ШИМ, который 8-битная панель выставит для видеокода: гамма и
/// округление к ближайшему, как в `map8` при глубине 8.
const fn panel_level(value: u8) -> u16 {
    let level = (PANEL_GAMMA16[value as usize] as u32 + 128) >> 8;
    if level > 255 {
        255
    } else {
        level as u16
    }
}

/// Видеокод -> наименьший код с тем же уровнем на панели. Из 256 кодов панель
/// различает 183: тёмные соседи сливаются после гаммы. Сводя их к одному
/// представителю, кадр не меняется на панели ни на бит, а шум теней перестаёт
/// мешать сжатию.
const PANEL_CANONICAL: [u8; 256] = {
    let mut table = [0u8; 256];
    let mut value = 0;
    while value < 256 {
        let mut first = value;
        while first > 0 && panel_level((first - 1) as u8) == panel_level(value as u8) {
            first -= 1;
        }
        table[value] = first as u8;
        value += 1;
    }
    table
};

fn stream_id_from_hash(hash: u64) -> u32 {
    (hash as u32).max(1)
}

struct PacketMeta {
    session_id: u64,
    stream_id: u32,
    frame_id: u32,
    kind: u8,
    codec: u8,
    payload_len: usize,
    pts_ns: u64,
    rate_n: u32,
    rate_d: u32,
    width: u16,
    height: u16,
    decoded_bytes: usize,
    encoded_bytes: usize,
    frame_crc: u32,
    chunk_index: u16,
    data_chunks: u16,
    clock_port: u16,
}

fn write_header(header: &mut [u8], m: PacketMeta) {
    header.fill(0);
    header[..4].copy_from_slice(MAGIC);
    header[4] = VERSION;
    header[5] = m.kind;
    header[6] = m.codec;
    header[7] = FLAG_FEC;
    header[8..10].copy_from_slice(&(HEADER as u16).to_be_bytes());
    header[10..12].copy_from_slice(&(m.payload_len as u16).to_be_bytes());
    header[12..20].copy_from_slice(&m.session_id.to_be_bytes());
    header[20..24].copy_from_slice(&m.stream_id.to_be_bytes());
    header[24..28].copy_from_slice(&m.frame_id.to_be_bytes());
    header[28..36].copy_from_slice(&m.pts_ns.to_be_bytes());
    header[36..40].copy_from_slice(&m.rate_n.to_be_bytes());
    header[40..44].copy_from_slice(&m.rate_d.to_be_bytes());
    header[44..46].copy_from_slice(&m.width.to_be_bytes());
    header[46..48].copy_from_slice(&m.height.to_be_bytes());
    header[48..52].copy_from_slice(&(m.decoded_bytes as u32).to_be_bytes());
    header[52..56].copy_from_slice(&(m.encoded_bytes as u32).to_be_bytes());
    header[56..60].copy_from_slice(&m.frame_crc.to_be_bytes());
    header[60..62].copy_from_slice(&m.chunk_index.to_be_bytes());
    header[62..64].copy_from_slice(&m.data_chunks.to_be_bytes());
    header[64..66].copy_from_slice(&(MAX_PAYLOAD as u16).to_be_bytes());
    header[66..68].copy_from_slice(&m.clock_port.to_be_bytes());
    let checksum = crc32(&header[..68]);
    header[68..72].copy_from_slice(&checksum.to_be_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;

    fn receive(pixels: &[u8], width: u16, height: u16) -> Vec<Vec<u8>> {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket
            .set_read_timeout(Some(Duration::from_millis(500)))
            .unwrap();
        let mut output =
            LedcastOutput::open("127.0.0.1", socket.local_addr().unwrap().port(), 8).unwrap();
        let sent = output
            .send_frame(pixels, width, height, 123_000_000, 60_000, 1_001)
            .unwrap();
        (0..sent.packets)
            .map(|_| {
                let mut p = vec![0; HEADER + MAX_PAYLOAD];
                let n = socket.recv(&mut p).unwrap();
                p.truncate(n);
                p
            })
            .collect()
    }

    #[test]
    fn v6_header_is_complete_and_self_checked() {
        let pixels: Vec<u8> = (0..32 * 16 * 3).map(|i| (i % 251) as u8).collect();
        let packets = receive(&pixels, 32, 16);
        let first = &packets[0];
        assert_eq!(&first[..4], b"LCF6");
        assert_eq!(first[4], 6);
        assert_eq!(first[5], KIND_DATA);
        assert!(matches!(first[6], CODEC_RAW | CODEC_LZ4));
        assert_eq!(u16::from_be_bytes(first[8..10].try_into().unwrap()), 72);
        assert_eq!(
            u64::from_be_bytes(first[28..36].try_into().unwrap()),
            123_000_000
        );
        assert_eq!(
            u32::from_be_bytes(first[36..40].try_into().unwrap()),
            60_000
        );
        assert_eq!(u32::from_be_bytes(first[40..44].try_into().unwrap()), 1_001);
        assert_eq!(
            u16::from_be_bytes(first[66..68].try_into().unwrap()),
            crate::clock::port().unwrap()
        );
        let canonical: Vec<u8> = pixels
            .iter()
            .map(|&value| PANEL_CANONICAL[usize::from(value)])
            .collect();
        assert_eq!(
            u32::from_be_bytes(first[56..60].try_into().unwrap()),
            crc32(&canonical)
        );
        assert_eq!(
            u32::from_be_bytes(first[68..72].try_into().unwrap()),
            crc32(&first[..68])
        );
        assert_eq!(packets.last().unwrap()[5], KIND_PARITY);
    }

    #[test]
    fn codec_numbers_match_v6_wire_contract() {
        assert_eq!(CODEC_RAW, 1);
        assert_eq!(CODEC_LZ4, 2);
        assert_eq!(CODEC_REPEAT, 3);
    }

    #[test]
    fn panel_gamma_matches_firmware() {
        let source = std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../firmware/components/colour/colour.cpp"
        ))
        .unwrap();
        let body = source
            .split("kGamma16[256] = {")
            .nth(1)
            .and_then(|rest| rest.split("};").next())
            .unwrap();
        let firmware: Vec<u16> = body
            .split(|c: char| !c.is_ascii_digit())
            .filter(|token| !token.is_empty())
            .map(|token| token.parse().unwrap())
            .collect();
        assert_eq!(firmware, PANEL_GAMMA16);
    }

    #[test]
    fn canonical_codes_keep_every_panel_level() {
        let mut distinct = std::collections::HashSet::new();
        for value in 0..=255u8 {
            let canonical = PANEL_CANONICAL[usize::from(value)];
            assert!(canonical <= value);
            assert_eq!(panel_level(canonical), panel_level(value));
            assert_eq!(PANEL_CANONICAL[usize::from(canonical)], canonical);
            distinct.insert(canonical);
        }
        assert_eq!(distinct.len(), 183);
        assert_eq!(PANEL_CANONICAL[255], 255);
    }

    #[test]
    fn identical_frames_repeat_previous_and_refresh_in_full() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket
            .set_read_timeout(Some(Duration::from_millis(500)))
            .unwrap();
        let mut output =
            LedcastOutput::open("127.0.0.1", socket.local_addr().unwrap().port(), 8).unwrap();
        let pixels: Vec<u8> = (0..96 * 16 * 3).map(|i| (i * 73 % 251) as u8).collect();
        let canonical: Vec<u8> = pixels
            .iter()
            .map(|&value| PANEL_CANONICAL[usize::from(value)])
            .collect();
        let mut codecs = Vec::new();
        for _ in 0..usize::from(MAX_REPEATS) + 2 {
            let sent = output.send_frame(&pixels, 96, 16, 0, 60, 1).unwrap();
            let mut first = Vec::new();
            for index in 0..sent.packets {
                let mut p = vec![0; HEADER + MAX_PAYLOAD];
                let n = socket.recv(&mut p).unwrap();
                p.truncate(n);
                if index == 0 {
                    first = p;
                }
            }
            let frame_id = u32::from_be_bytes(first[24..28].try_into().unwrap());
            assert_eq!(
                u32::from_be_bytes(first[56..60].try_into().unwrap()),
                crc32(&canonical)
            );
            if first[6] == CODEC_REPEAT {
                assert_eq!(sent.packets, 2);
                assert_eq!(first.len(), HEADER + 4);
                let reference = u32::from_be_bytes(first[HEADER..HEADER + 4].try_into().unwrap());
                assert_eq!(reference, frame_id - 1);
            }
            codecs.push(first[6]);
        }
        let full = if canonical.len() > lz4_flex::block::compress(&canonical).len() + 16 {
            CODEC_LZ4
        } else {
            CODEC_RAW
        };
        let mut expected = vec![full];
        expected.extend(std::iter::repeat_n(CODEC_REPEAT, usize::from(MAX_REPEATS)));
        expected.push(full);
        assert_eq!(codecs, expected);
    }

    #[test]
    fn stream_id_is_never_zero() {
        assert_eq!(stream_id_from_hash(0), 1);
        assert_eq!(stream_id_from_hash(42), 42);
    }

    #[test]
    fn xor_parity_recovers_one_missing_chunk() {
        let pixels: Vec<u8> = (0..96 * 16 * 3).map(|i| (i * 73 % 251) as u8).collect();
        let packets = receive(&pixels, 96, 16);
        let count = u16::from_be_bytes(packets[0][62..64].try_into().unwrap()) as usize;
        for missing in 0..count {
            let mut recovered = packets[count][HEADER..].to_vec();
            for (index, packet) in packets[..count].iter().enumerate() {
                if index != missing {
                    for (out, byte) in recovered.iter_mut().zip(&packet[HEADER..]) {
                        *out ^= *byte;
                    }
                }
            }
            assert_eq!(
                &recovered[..packets[missing].len() - HEADER],
                &packets[missing][HEADER..]
            );
        }
    }

    #[test]
    fn malformed_geometry_and_rate_are_rejected() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        let mut out =
            LedcastOutput::open("127.0.0.1", socket.local_addr().unwrap().port(), 8).unwrap();
        assert_eq!(
            out.send_frame(&[], 0, 16, 0, 60, 1).unwrap_err().kind(),
            io::ErrorKind::InvalidInput
        );
        assert_eq!(
            out.send_frame(&[0; 5], 2, 1, 0, 60, 1).unwrap_err().kind(),
            io::ErrorKind::InvalidInput
        );
        assert_eq!(
            out.send_frame(&[0; 6], 2, 1, 0, 0, 1).unwrap_err().kind(),
            io::ErrorKind::InvalidInput
        );
    }
}
