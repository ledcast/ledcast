use std::collections::HashMap;
use std::io::Write;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::canvas::Canvas;
use crate::device::{Client, Failure};
use crate::pairing;
use crate::settings::{Mapping, Node};
use crate::source::{FrameView, PixelLayout, VideoSource};
use crate::stream::LedcastOutput;

pub const DEFAULT_MAX_FPS: u16 = 60;
/// Задержка показа по умолчанию: время от захвата кадра до его показа на всех
/// строках сразу. Её съедают доставка по Wi‑Fi и паузы радио.
pub const DEFAULT_LATENCY_MS: u32 = 100;
/// Задержки, которые предлагает Console. Каждый кадр задержки строка держит в
/// памяти, поэтому длинным строкам доступны только меньшие; 25 мс по Wi‑Fi
/// теряет кадры и годится для кабеля и пустого эфира.
pub const LATENCIES_MS: [u32; 4] = [25, 50, 75, DEFAULT_LATENCY_MS];
/// Без нового кадра источника строке всё равно уходит повтор: она не гаснет и
/// продолжает сверять часы.
const KEEPALIVE: Duration = Duration::from_millis(100);

pub(crate) fn timing_trace(line: &str) {
    eprintln!("{line}");
    let Some(path) = std::env::var_os("LEDCAST_TIMING_TRACE_FILE") else {
        return;
    };
    if let Ok(mut file) = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
    {
        let _ = writeln!(file, "{line}");
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct FrameRate {
    numerator: u32,
    denominator: u32,
}

impl FrameRate {
    fn new(numerator: u32, denominator: u32) -> Self {
        if numerator == 0 || denominator == 0 {
            Self {
                numerator: 60,
                denominator: 1,
            }
        } else {
            Self {
                numerator,
                denominator,
            }
        }
    }

    fn capped(self, frame_rate: u16) -> Self {
        let cap = u32::from(frame_rate.max(1));
        if u64::from(self.numerator) > u64::from(cap) * u64::from(self.denominator) {
            Self {
                numerator: cap,
                denominator: 1,
            }
        } else {
            self
        }
    }

    fn fps(self) -> f32 {
        self.numerator as f32 / self.denominator as f32
    }
}

#[cfg(target_os = "macos")]
fn set_realtime_priority() {
    unsafe extern "C" {
        fn pthread_set_qos_class_self_np(qos_class: u32, relative_priority: i32) -> i32;
    }
    // QOS_CLASS_USER_INTERACTIVE. Failure is harmless: timing remains absolute.
    const USER_INTERACTIVE: u32 = 0x21;
    unsafe {
        let _ = pthread_set_qos_class_self_np(USER_INTERACTIVE, 0);
    }
}

#[cfg(target_os = "windows")]
fn set_realtime_priority() {
    use std::cell::Cell;
    use std::ffi::c_void;

    #[link(name = "Avrt")]
    unsafe extern "system" {
        fn AvSetMmThreadCharacteristicsW(
            task_name: *const u16,
            task_index: *mut u32,
        ) -> *mut c_void;
    }
    thread_local! {
        static MMCSS_HANDLE: Cell<*mut c_void> = const { Cell::new(std::ptr::null_mut()) };
    }
    MMCSS_HANDLE.with(|slot| {
        if slot.get().is_null() {
            let name: Vec<u16> = "Pro Audio\0".encode_utf16().collect();
            let mut index = 0u32;
            let handle = unsafe { AvSetMmThreadCharacteristicsW(name.as_ptr(), &mut index) };
            slot.set(handle);
        }
    });
}

#[cfg(not(any(target_os = "macos", target_os = "windows")))]
fn set_realtime_priority() {}

#[derive(Clone)]
pub struct Route {
    pub node: Node,
    pub mapping: Mapping,
}

#[derive(Clone)]
pub struct Preview {
    pub node_id: String,
    pub width: u16,
    pub height: u16,
    pub pixels: Vec<u8>,
    pub generation: u64,
}

#[derive(Clone, Default)]
pub struct NodeStats {
    pub running: bool,
    pub frames: u64,
    pub fps: f32,
    pub megabits_per_second: f32,
    pub error: Option<String>,
    pub device_fps: u16,
    pub queue_depth: u16,
    pub repeated: u32,
    pub late: u32,
    pub evicted: u32,
    pub torn: u32,
    pub jitter_us_max: u32,
    pub source_fps: f32,
    pub target_fps: f32,
    /// Сколько задержки строка удерживает в своём резерве кадров; 0 — ещё не
    /// сообщила.
    pub capacity_ms: u32,
    pub clock_synced: bool,
    pub clock_rtt_us: u32,
}

#[derive(Clone, Default)]
pub struct Stats {
    pub running: bool,
    pub nodes: HashMap<String, NodeStats>,
    /// Задержка показа, с которой идёт вывод: заданная или меньше, если
    /// какой-то строке не хватает памяти на заданную.
    pub latency_ms: u32,
}

impl Stats {
    /// Одна задержка на все строки — иначе они разойдутся. Строка, которой не
    /// хватает резерва, опускает её для всех.
    fn latency_ms(&self, wanted: u32) -> u32 {
        self.nodes
            .values()
            .filter(|node| node.capacity_ms != 0)
            .map(|node| node.capacity_ms)
            .fold(wanted, u32::min)
    }
}

enum Command {
    Start(Vec<Route>, HashMap<String, Box<dyn VideoSource>>),
    Stop,
}

struct Shared {
    latency_ms: AtomicU32,
    stats: Mutex<Stats>,
    previews: Mutex<HashMap<String, Preview>>,
    preview_generation: AtomicU64,
}

#[derive(Clone)]
pub struct Feed {
    commands: Sender<Command>,
    shared: Arc<Shared>,
}

impl Feed {
    pub fn start() -> Self {
        let (commands, inbox) = mpsc::channel();
        let shared = Arc::new(Shared {
            latency_ms: AtomicU32::new(DEFAULT_LATENCY_MS),
            stats: Mutex::new(Stats::default()),
            previews: Mutex::new(HashMap::new()),
            preview_generation: AtomicU64::new(0),
        });
        let worker = Arc::clone(&shared);
        std::thread::Builder::new()
            .name("ledcast-output".into())
            .spawn(move || run(inbox, worker))
            .expect("LEDCAST output worker failed to start");
        Self { commands, shared }
    }

    pub fn play(&self, routes: Vec<Route>, sources: HashMap<String, Box<dyn VideoSource>>) {
        let _ = self.commands.send(Command::Start(routes, sources));
    }

    pub fn stop(&self) {
        let _ = self.commands.send(Command::Stop);
    }

    /// Задержка показа; идущий вывод подхватывает её со следующего кадра.
    pub fn set_latency(&self, milliseconds: u32) {
        self.shared
            .latency_ms
            .store(milliseconds, Ordering::Release);
    }

    pub fn stats(&self) -> Stats {
        self.shared.stats.lock().unwrap().clone()
    }

    pub fn preview(&self, node_id: &str, seen: u64) -> Option<Preview> {
        let previews = self.shared.previews.lock().unwrap();
        if previews
            .get(node_id)
            .is_some_and(|preview| preview.generation > seen)
        {
            previews.get(node_id).cloned()
        } else {
            None
        }
    }
}

struct OwnedFrame {
    width: u32,
    height: u32,
    stride: usize,
    layout: PixelLayout,
    data: Vec<u8>,
    generation: u64,
    frame_rate: FrameRate,
    timestamp: i64,
    /// Когда кадр пришёл от источника: от этого момента считается время показа.
    captured: Instant,
}

impl OwnedFrame {
    fn view(&self) -> FrameView<'_> {
        FrameView {
            width: self.width,
            height: self.height,
            stride: self.stride,
            layout: self.layout,
            data: &self.data,
            frame_rate_n: self.frame_rate.numerator,
            frame_rate_d: self.frame_rate.denominator,
            timestamp: self.timestamp,
        }
    }
}

struct FramePool {
    slots: Vec<Arc<OwnedFrame>>,
}

impl FramePool {
    fn new(consumers: usize) -> Self {
        // Each worker can retain one snapshot, plus the published snapshot
        // and the next writer. Slow consumers cannot grow memory usage.
        let slots = (0..consumers + 2)
            .map(|_| {
                Arc::new(OwnedFrame {
                    width: 0,
                    height: 0,
                    stride: 0,
                    layout: PixelLayout::Bgrx,
                    data: Vec::new(),
                    generation: 0,
                    frame_rate: FrameRate::new(60, 1),
                    timestamp: 0,
                    captured: Instant::now(),
                })
            })
            .collect();
        Self { slots }
    }

    fn copy_from(
        &mut self,
        frame: &FrameView<'_>,
        generation: u64,
        captured: Instant,
    ) -> Option<Arc<OwnedFrame>> {
        let bytes = frame.stride.checked_mul(frame.height as usize)?;
        let source = frame.data.get(..bytes)?;
        for slot in &mut self.slots {
            // Only the pool's reference remains: neither mailbox nor any
            // independent preview/output worker can still read these bytes.
            if let Some(owned) = Arc::get_mut(slot) {
                owned.data.resize(bytes, 0);
                owned.data.copy_from_slice(source);
                owned.width = frame.width;
                owned.height = frame.height;
                owned.stride = frame.stride;
                owned.layout = frame.layout;
                owned.generation = generation;
                owned.frame_rate = FrameRate::new(frame.frame_rate_n, frame.frame_rate_d);
                owned.timestamp = frame.timestamp;
                owned.captured = captured;
                return Some(Arc::clone(slot));
            }
        }
        None
    }
}

struct FrameMailbox {
    frame: Mutex<Option<Arc<OwnedFrame>>>,
    updated: Condvar,
}

type LatestFrame = Arc<FrameMailbox>;

#[cfg(target_os = "macos")]
struct StreamingActivity(
    objc2::rc::Retained<objc2::runtime::ProtocolObject<dyn objc2_foundation::NSObjectProtocol>>,
);

#[cfg(target_os = "macos")]
impl StreamingActivity {
    fn new() -> Self {
        use objc2_foundation::{NSActivityOptions, NSProcessInfo, NSString};
        // Live video keeps running when its control window is covered. Thread
        // QoS alone does not opt the process out of App Nap/timer coalescing.
        // Keep display/system idle sleep policy under the user's control.
        let options = NSActivityOptions::UserInitiatedAllowingIdleSystemSleep
            | NSActivityOptions::LatencyCritical;
        Self(
            NSProcessInfo::processInfo().beginActivityWithOptions_reason(
                options,
                &NSString::from_str("LEDCAST live NDI video"),
            ),
        )
    }
}

#[cfg(target_os = "macos")]
impl Drop for StreamingActivity {
    fn drop(&mut self) {
        // This token came from beginActivity above and is ended exactly once,
        // after Pipeline::stop has joined the live-video workers.
        unsafe { objc2_foundation::NSProcessInfo::processInfo().endActivity(&self.0) };
    }
}

struct Pipeline {
    stop: Arc<AtomicBool>,
    workers: Vec<JoinHandle<()>>,
    #[cfg(target_os = "macos")]
    _activity: Option<StreamingActivity>,
}

impl Pipeline {
    fn stop(self) {
        self.stop.store(true, Ordering::Release);
        for worker in self.workers {
            let _ = worker.join();
        }
    }
}

fn run(inbox: Receiver<Command>, shared: Arc<Shared>) {
    let mut pipeline: Option<Pipeline> = None;
    while let Ok(command) = inbox.recv() {
        match command {
            Command::Start(routes, sources) => {
                if let Some(previous) = pipeline.take() {
                    previous.stop();
                }
                pipeline = Some(start_pipeline(routes, sources, Arc::clone(&shared)));
            }
            Command::Stop => {
                if let Some(previous) = pipeline.take() {
                    previous.stop();
                }
                *shared.stats.lock().unwrap() = Stats::default();
                shared.previews.lock().unwrap().clear();
            }
        }
    }
    if let Some(previous) = pipeline {
        previous.stop();
    }
}

fn start_pipeline(
    routes: Vec<Route>,
    sources: HashMap<String, Box<dyn VideoSource>>,
    shared: Arc<Shared>,
) -> Pipeline {
    #[cfg(target_os = "macos")]
    let activity = (!routes.is_empty() && !sources.is_empty()).then(StreamingActivity::new);
    let stop = Arc::new(AtomicBool::new(false));
    let mut workers = Vec::new();
    let mut mailboxes: HashMap<String, LatestFrame> = HashMap::new();

    for (name, mut source) in sources {
        let consumers = routes
            .iter()
            .filter(|route| route.mapping.ndi_source.as_deref() == Some(name.as_str()))
            .count()
            * 2;
        let mailbox: LatestFrame = Arc::new(FrameMailbox {
            frame: Mutex::new(None),
            updated: Condvar::new(),
        });
        mailboxes.insert(name.clone(), Arc::clone(&mailbox));
        let capture_stop = Arc::clone(&stop);
        workers.push(
            std::thread::Builder::new()
                .name("ledcast-ndi-capture".into())
                .spawn(move || {
                    set_realtime_priority();
                    let trace_timing = std::env::var_os("LEDCAST_TIMING_TRACE").is_some();
                    let mut last_capture = None;
                    let mut generation = 0u64;
                    let mut pool = FramePool::new(consumers);
                    let mut trace_started = Instant::now();
                    let mut captured = 0u32;
                    let mut pool_drops = 0u32;
                    let mut gap_us_max = 0u128;
                    let mut acquire_us_max = 0u128;
                    let mut copy_us_max = 0u128;
                    while !capture_stop.load(Ordering::Acquire) {
                        // The source owns its clock (NDI FrameSync uses a
                        // paced pull). Do not add another sleep here.
                        let acquire_started = trace_timing.then(Instant::now);
                        if let Some(frame) = source.next_frame(Duration::from_millis(100)) {
                            let arrived = Instant::now();
                            let rate = FrameRate::new(frame.frame_rate_n, frame.frame_rate_d);
                            if trace_timing {
                                let now = Instant::now();
                                if let Some(previous) = last_capture {
                                    let gap = now.duration_since(previous);
                                    gap_us_max = gap_us_max.max(gap.as_micros());
                                }
                                last_capture = Some(now);
                                acquire_us_max = acquire_us_max.max(acquire_started.unwrap().elapsed().as_micros());
                            }
                            generation = generation.wrapping_add(1);
                            captured += 1;
                            let copy_started = trace_timing.then(Instant::now);
                            if let Some(owned) = pool.copy_from(&frame, generation, arrived) {
                                *mailbox.frame.lock().unwrap() = Some(owned);
                                mailbox.updated.notify_all();
                            } else {
                                pool_drops += 1;
                            }
                            if trace_timing {
                                copy_us_max = copy_us_max.max(copy_started.unwrap().elapsed().as_micros());
                                if trace_started.elapsed() >= Duration::from_secs(1) {
                                    timing_trace(&format!("LEDCAST capture timing: source={name:?} declared_fps={:.3} captured_fps={:.3} size={}x{} gap_us_max={gap_us_max} acquire_us_max={acquire_us_max} copy_us_max={copy_us_max} pool_drops={pool_drops} pool_slots={}", rate.fps(), captured as f32 / trace_started.elapsed().as_secs_f32(), frame.width, frame.height, pool.slots.len()));
                                    trace_started = Instant::now();
                                    captured = 0;
                                    pool_drops = 0;
                                    gap_us_max = 0;
                                    acquire_us_max = 0;
                                    copy_us_max = 0;
                                }
                            }
                        }
                    }
                })
                .expect("NDI capture worker failed to start"),
        );
    }

    let mut stats = Stats {
        running: false,
        nodes: HashMap::new(),
        latency_ms: shared.latency_ms.load(Ordering::Acquire),
    };
    for route in routes {
        let node_id = route.node.id.clone();
        stats.nodes.insert(node_id.clone(), NodeStats::default());
        let Some(source_name) = route.mapping.ndi_source.clone() else {
            continue;
        };
        let Some(mailbox) = mailboxes.get(&source_name).cloned() else {
            continue;
        };
        stats.running = true;
        let preview_route = route.clone();
        let preview_mailbox = Arc::clone(&mailbox);
        let preview_stop = Arc::clone(&stop);
        let preview_shared = Arc::clone(&shared);
        workers.push(
            std::thread::Builder::new()
                .name(format!("ledcast-preview-{node_id}"))
                .spawn(move || {
                    preview_worker(preview_route, preview_mailbox, preview_stop, preview_shared)
                })
                .expect("LEDCAST preview worker failed to start"),
        );
        let output_stop = Arc::clone(&stop);
        let output_shared = Arc::clone(&shared);
        workers.push(
            std::thread::Builder::new()
                .name(format!("ledcast-output-{node_id}"))
                .spawn(move || output_worker(route, mailbox, output_stop, output_shared))
                .expect("LEDCAST node output worker failed to start"),
        );
    }
    *shared.stats.lock().unwrap() = stats;
    Pipeline {
        stop,
        workers,
        #[cfg(target_os = "macos")]
        _activity: activity,
    }
}

fn preview_worker(route: Route, mailbox: LatestFrame, stop: Arc<AtomicBool>, shared: Arc<Shared>) {
    let node_id = route.node.id.clone();
    let mut canvas = Canvas::new(route.node.width, route.node.height);
    let mut seen_generation = 0u64;

    while !stop.load(Ordering::Acquire) {
        let mut frame = mailbox.frame.lock().unwrap();
        while frame
            .as_ref()
            .is_none_or(|value| value.generation == seen_generation)
            && !stop.load(Ordering::Acquire)
        {
            let (next, _) = mailbox
                .updated
                .wait_timeout(frame, Duration::from_millis(100))
                .unwrap();
            frame = next;
        }
        let Some(current) = frame.clone() else {
            continue;
        };
        drop(frame);

        seen_generation = current.generation;
        canvas.draw(
            &current.view(),
            route.node.width,
            route.node.height,
            route.mapping.fit,
            route.mapping.transform,
        );
        let generation = shared.preview_generation.fetch_add(1, Ordering::Relaxed) + 1;
        shared.previews.lock().unwrap().insert(
            node_id.clone(),
            Preview {
                node_id: node_id.clone(),
                width: canvas.width,
                height: canvas.height,
                pixels: canvas.pixels.clone(),
                generation,
            },
        );
    }
}

/// Вход Console на строку для потока: строка принимает кадры только с адреса,
/// вошедшего ключом. Пока вход не удался, вывод на строку ничего не шлёт: строки
/// нет в сети — и пакетов нет, и Mac не ищет её широковещательными ARP-запросами.
struct Claim {
    started: Instant,
    heard_ms: AtomicU64,
    accepted: AtomicBool,
    error: Mutex<Option<String>>,
}

impl Claim {
    /// Обратной связи нет дольше этого — строка не принимает поток.
    const SILENCE: Duration = Duration::from_secs(3);
    /// Молчащую строку проверяют так часто: она могла пропасть из сети.
    const PROBE: Duration = Duration::from_secs(5);
    /// Повторный вход к живой, но молчащей строке не чаще этого: каждый занимает её сессию.
    const RELOGIN: Duration = Duration::from_secs(10);
    const FAILED_RETRY: Duration = Duration::from_secs(2);
    /// Вход освежается заранее, задолго до конца сессии строки.
    const RENEW: Duration = Duration::from_secs(6 * 60 * 60);

    fn new() -> Self {
        Self {
            started: Instant::now(),
            heard_ms: AtomicU64::new(0),
            accepted: AtomicBool::new(false),
            error: Mutex::new(None),
        }
    }

    fn heard(&self) {
        let ms = self.started.elapsed().as_millis() as u64;
        self.heard_ms.store(ms.max(1), Ordering::Relaxed);
    }

    fn silent(&self) -> bool {
        let heard = self.heard_ms.load(Ordering::Relaxed);
        heard == 0 || self.started.elapsed() > Duration::from_millis(heard) + Self::SILENCE
    }

    fn accepted(&self) -> bool {
        self.accepted.load(Ordering::Acquire)
    }

    fn settle(&self, accepted: bool, error: Option<String>) {
        self.accepted.store(accepted, Ordering::Release);
        *self.error.lock().unwrap() = error;
    }
}

fn claim_worker(host: String, device_id: String, claim: Arc<Claim>, stop: Arc<AtomicBool>) {
    let mut renew_at = Instant::now();
    let mut retry_at = Instant::now();
    let mut probe_at = Instant::now();
    let mut logged_in_at: Option<Instant> = None;
    while !stop.load(Ordering::Acquire) {
        let now = Instant::now();
        let mut login_now = if claim.accepted() {
            now >= renew_at
        } else {
            now >= retry_at
        };
        if claim.accepted() && !login_now && claim.silent() && now >= probe_at {
            probe_at = now + Claim::PROBE;
            match Client::new(&host).status() {
                Err(_) => claim.settle(false, Some("Строка не отвечает".into())),
                // Встроенное шоу и тест закрывают приём: молчание ожидаемо.
                Ok(status)
                    if status.show_enabled == Some(true) || !status.test_pattern.is_empty() => {}
                // Строка жива и в режиме NDI, но молчит — перезагрузилась и забыла адрес.
                Ok(_) => login_now = logged_in_at.is_none_or(|at| at.elapsed() >= Claim::RELOGIN),
            }
        }
        if login_now {
            // Ключ читается при каждом входе: строку можно подключить, не останавливая вывод.
            let login = match pairing::stored_key(&device_id) {
                Some(key) => pairing::login(&host, &key),
                None => Err(Failure::Other(pairing::LOCKED.into())),
            };
            match login {
                Ok(session) => {
                    let lifetime = Duration::from_secs(session.expires_in) / 2;
                    renew_at = now + lifetime.min(Claim::RENEW);
                    logged_in_at = Some(now);
                    probe_at = now + Claim::PROBE;
                    claim.settle(true, None);
                }
                Err(Failure::Unauthorized) => {
                    retry_at = now + Claim::RELOGIN;
                    claim.settle(false, Some(pairing::REJECTED.to_string()));
                }
                Err(other) => {
                    retry_at = now + Claim::FAILED_RETRY;
                    claim.settle(false, Some(other.to_string()));
                }
            }
        }
        std::thread::sleep(Duration::from_millis(250));
    }
}

/// Ошибка, после которой сокет уже не оживёт: его сеть пропала. «Отказ в соединении»
/// сюда не входит — это строка закрыла приём на время встроенного шоу или теста.
fn socket_lost(error: &std::io::Error) -> bool {
    use std::io::ErrorKind::*;
    matches!(
        error.kind(),
        BrokenPipe
            | NotConnected
            | AddrNotAvailable
            | NetworkUnreachable
            | NetworkDown
            | HostUnreachable
    )
}

fn output_worker(route: Route, mailbox: LatestFrame, stop: Arc<AtomicBool>, shared: Arc<Shared>) {
    set_realtime_priority();
    let node_id = route.node.id.clone();
    let claim = Arc::new(Claim::new());
    let claimer = {
        let host = route.node.host.clone();
        let device_id = node_id.clone();
        let claim = Arc::clone(&claim);
        let stop = Arc::clone(&stop);
        std::thread::Builder::new()
            .name(format!("ledcast-claim-{node_id}"))
            .spawn(move || claim_worker(host, device_id, claim, stop))
            .expect("LEDCAST claim worker failed to start")
    };
    let mut output = match LedcastOutput::open(
        &route.node.host,
        crate::stream::DEFAULT_PORT,
        route.node.bit_depth,
    ) {
        Ok(output) => output,
        Err(error) => {
            if let Some(stat) = shared.stats.lock().unwrap().nodes.get_mut(&node_id) {
                stat.error = Some(error.to_string());
            }
            return;
        }
    };
    let mut reopened = Instant::now();
    let mut canvas = Canvas::new(route.node.width, route.node.height);
    let mut seen_generation = 0u64;
    let mut last_sent: Option<Instant> = None;
    let mut window_started = Instant::now();
    let mut window_frames = 0u32;
    let mut window_bytes = 0u64;
    let trace_timing = std::env::var_os("LEDCAST_TIMING_TRACE").is_some();
    let mut window_repeats = 0u32;
    let mut window_errors = 0u32;
    let mut window_skipped = 0u32;

    while !stop.load(Ordering::Acquire) {
        // Кадр уходит, как только источник его дал: время показа задаёт не
        // момент отправки, а момент захвата. Без новых кадров — повтор.
        let current = {
            let mut slot = mailbox.frame.lock().unwrap();
            let deadline = Instant::now() + KEEPALIVE;
            while slot
                .as_ref()
                .is_none_or(|frame| frame.generation == seen_generation)
                && !stop.load(Ordering::Acquire)
            {
                let now = Instant::now();
                if now >= deadline {
                    break;
                }
                slot = mailbox
                    .updated
                    .wait_timeout(slot, deadline - now)
                    .unwrap()
                    .0;
            }
            slot.clone()
        };
        let Some(current) = current else {
            continue;
        };
        if !claim.accepted() {
            if let Some(stat) = shared.stats.lock().unwrap().nodes.get_mut(&node_id) {
                stat.running = false;
                stat.fps = 0.0;
                stat.megabits_per_second = 0.0;
                stat.error = claim.error.lock().unwrap().clone();
            }
            std::thread::sleep(Duration::from_millis(100));
            continue;
        }
        let new_source_frame = current.generation != seen_generation;
        let output_rate = current.frame_rate.capped(route.node.frame_rate);
        let period = Duration::from_nanos(
            (1_000_000_000u64 * u64::from(output_rate.denominator))
                / u64::from(output_rate.numerator.max(1)),
        );
        // Источник чаще, чем выводит строка: лишние кадры пропускаются по
        // времени захвата, одинаково на всех строках.
        if new_source_frame
            && last_sent.is_some_and(|sent| current.captured < sent + period * 3 / 4)
        {
            seen_generation = current.generation;
            window_skipped += 1;
            continue;
        }
        let presented_at = if new_source_frame {
            current.captured
        } else {
            window_repeats += 1;
            Instant::now()
        };
        if new_source_frame {
            seen_generation = current.generation;
            canvas.draw(
                &current.view(),
                route.node.width,
                route.node.height,
                route.mapping.fit,
                route.mapping.transform,
            );
        }
        let latency_ms = {
            let mut stats = shared.stats.lock().unwrap();
            let latency_ms = stats.latency_ms(shared.latency_ms.load(Ordering::Acquire));
            stats.latency_ms = latency_ms;
            latency_ms
        };
        let presentation_ns = crate::clock::ns(presented_at) + u64::from(latency_ms) * 1_000_000;

        match output.send_frame(
            &canvas.pixels,
            canvas.width,
            canvas.height,
            presentation_ns,
            output_rate.numerator,
            output_rate.denominator,
        ) {
            // Радио на паузе: кадр брошен до ухода в эфир, это не ошибка.
            Ok(sent) if sent.dropped => {}
            Ok(sent) => {
                if new_source_frame {
                    last_sent = Some(current.captured);
                }
                window_frames += 1;
                window_bytes += sent.wire_bytes as u64;
                if let Some(stat) = shared.stats.lock().unwrap().nodes.get_mut(&node_id) {
                    stat.frames += 1;
                    stat.running = true;
                    stat.error = claim.error.lock().unwrap().clone();
                    stat.source_fps = current.frame_rate.fps();
                    stat.target_fps = output_rate.fps();
                }
            }
            Err(error) => {
                if trace_timing && window_errors == 0 {
                    timing_trace(&format!("LEDCAST output error: node={node_id} {error}"));
                }
                window_errors += 1;
                if let Some(stat) = shared.stats.lock().unwrap().nodes.get_mut(&node_id) {
                    stat.error = Some(error.to_string());
                }
                // Сокет привязан к сети, которой больше нет (Mac переподключился): шлёт
                // в никуда вечно. Новый сокет — не чаще раза в секунду.
                if socket_lost(&error) && reopened.elapsed() >= Duration::from_secs(1) {
                    reopened = Instant::now();
                    if let Ok(fresh) = LedcastOutput::open(
                        &route.node.host,
                        crate::stream::DEFAULT_PORT,
                        route.node.bit_depth,
                    ) {
                        output = fresh;
                    }
                }
            }
        }

        if let Ok(Some(feedback)) = output.poll_feedback() {
            claim.heard();
            if trace_timing {
                timing_trace(&format!(
                    "LEDCAST device feedback: node={node_id} verified={} committed={} packets={} incomplete={} recovered={} corrupt={} evicted={} late={} depth={} commit_fps={} jitter_us_max={} flip_timeouts={} reservoir={} clock_synced={} clock_rtt_us={}",
                    feedback.highest_verified,
                    feedback.highest_committed,
                    feedback.packets_received,
                    feedback.incomplete,
                    feedback.fec_recovered,
                    feedback.crc_or_decode_errors,
                    feedback.assembly_evictions,
                    feedback.late,
                    feedback.assembly_depth,
                    feedback.commit_fps,
                    feedback.arrival_jitter_us_max,
                    feedback.flip_timeouts,
                    feedback.reservoir_frames,
                    feedback.clock_synced,
                    feedback.clock_rtt_us,
                ));
            }
            if let Some(stat) = shared.stats.lock().unwrap().nodes.get_mut(&node_id) {
                stat.device_fps = feedback.commit_fps;
                stat.queue_depth = feedback.assembly_depth;
                stat.repeated = 0;
                stat.late = feedback.late;
                stat.evicted = feedback.assembly_evictions;
                stat.torn = feedback.crc_or_decode_errors + feedback.flip_timeouts;
                stat.jitter_us_max = feedback.arrival_jitter_us_max;
                stat.clock_synced = feedback.clock_synced;
                stat.clock_rtt_us = feedback.clock_rtt_us;
                // Последний кадр резерва — запас на неровную доставку.
                let rate = current.frame_rate.capped(route.node.frame_rate);
                stat.capacity_ms = (u64::from(feedback.reservoir_frames.saturating_sub(1))
                    * 1000
                    * u64::from(rate.denominator)
                    / u64::from(rate.numerator.max(1))) as u32;
            }
        }

        if window_started.elapsed() >= Duration::from_secs(1) {
            let seconds = window_started.elapsed().as_secs_f32();
            if trace_timing {
                timing_trace(&format!("LEDCAST output timing: node={node_id} fps={:.3} keepalive_repeats={window_repeats} skipped={window_skipped} frames={window_frames} errors={window_errors}", window_frames as f32 / seconds));
            }
            if let Some(stat) = shared.stats.lock().unwrap().nodes.get_mut(&node_id) {
                stat.fps = window_frames as f32 / seconds;
                stat.megabits_per_second = window_bytes as f32 * 8.0 / seconds / 1_000_000.0;
                stat.running = window_frames > 0;
            }
            window_frames = 0;
            window_bytes = 0;
            window_repeats = 0;
            window_errors = 0;
            window_skipped = 0;
            window_started = Instant::now();
        }
    }
    let _ = claimer.join();
}

pub fn estimated_mbps(nodes: &[Node], mappings: &[Mapping]) -> f32 {
    mappings
        .iter()
        .filter(|mapping| mapping.enabled && mapping.ndi_source.is_some())
        .filter_map(|mapping| nodes.iter().find(|node| node.id == mapping.node_id))
        .map(|node| {
            node.width as f32 * node.height as f32 * 3.0 * node.frame_rate as f32 * 8.0
                / 1_000_000.0
        })
        .sum()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ndi_rate_is_preserved_until_panel_cap() {
        assert_eq!(
            FrameRate::new(30_000, 1_001).capped(60).fps(),
            30_000.0 / 1_001.0
        );
        assert_eq!(FrameRate::new(60, 1).capped(30), FrameRate::new(30, 1));
    }

    struct SolidSource {
        pixels: Vec<u8>,
    }

    #[test]
    fn capture_pool_reuses_storage_without_overwriting_readers() {
        let mut pool = FramePool::new(2);
        let pixels = [1u8; 8];
        let mut frame = FrameView {
            width: 2,
            height: 1,
            stride: 8,
            layout: PixelLayout::Rgba,
            data: &pixels,
            frame_rate_n: 60,
            frame_rate_d: 1,
            timestamp: 7,
        };
        let first = pool.copy_from(&frame, 1, Instant::now()).unwrap();
        let first_arc = Arc::as_ptr(&first);
        let first_pixels = first.data.as_ptr();
        let changed = [2u8; 8];
        frame.data = &changed;
        let second = pool.copy_from(&frame, 2, Instant::now()).unwrap();
        assert_ne!(Arc::as_ptr(&second), first_arc);
        assert_eq!(first.data, pixels); // Independent preview can retain it.
        drop(first);
        for generation in 3..100 {
            let current = pool.copy_from(&frame, generation, Instant::now()).unwrap();
            assert_eq!(Arc::as_ptr(&current), first_arc);
            assert_eq!(current.data.as_ptr(), first_pixels);
            assert_eq!(current.generation, generation);
            assert_eq!(current.data, changed);
        }
        assert_eq!(second.generation, 2); // Independent output remains intact.
        assert_eq!(pool.slots.len(), 4);
    }

    #[test]
    fn capture_pool_is_bounded_and_resumes_after_a_reader_releases() {
        let mut pool = FramePool::new(2);
        let pixels = [1u8; 8];
        let mut frame = FrameView {
            width: 2,
            height: 1,
            stride: 8,
            layout: PixelLayout::Rgba,
            data: &pixels,
            frame_rate_n: 60,
            frame_rate_d: 1,
            timestamp: 7,
        };
        let mut readers: Vec<_> = (0..4)
            .map(|i| pool.copy_from(&frame, i, Instant::now()).unwrap())
            .collect();
        assert!(pool.copy_from(&frame, 5, Instant::now()).is_none());
        assert_eq!(pool.slots.len(), 4);
        readers.pop();
        let larger = [3u8; 16];
        frame.height = 2;
        frame.data = &larger;
        let resized = pool.copy_from(&frame, 6, Instant::now()).unwrap();
        assert_eq!(resized.data, larger);
        assert!(readers.iter().all(|reader| reader.data == pixels));
        drop(resized);
        frame.data = &pixels; // Invalid length never reaches the copy.
        assert!(pool.copy_from(&frame, 7, Instant::now()).is_none());
    }

    impl VideoSource for SolidSource {
        fn title(&self) -> String {
            "test".into()
        }

        fn next_frame(&mut self, _timeout: Duration) -> Option<crate::source::FrameView<'_>> {
            Some(crate::source::FrameView {
                width: 2,
                height: 1,
                stride: 8,
                layout: crate::source::PixelLayout::Rgba,
                data: &self.pixels,
                frame_rate_n: 60,
                frame_rate_d: 1,
                timestamp: 0,
            })
        }
    }

    #[test]
    fn traffic_is_rgb888_at_60_fps() {
        let node = Node {
            id: "1".into(),
            name: "A".into(),
            host: "127.0.0.1".into(),
            width: 384,
            height: 32,
            modules: 12,
            firmware: String::new(),
            rssi: None,
            bit_depth: 8,
            frame_rate: 60,
        };
        let mapping = Mapping {
            node_id: "1".into(),
            ndi_source: Some("NDI".into()),
            fit: crate::canvas::Fit::Cover,
            transform: Default::default(),
            enabled: true,
        };
        assert!((estimated_mbps(&[node], &[mapping]) - 17.69472).abs() < 0.001);
    }

    #[test]
    fn preview_and_output_start_without_a_control_handshake() {
        let feed = Feed::start();
        let node = Node {
            id: "line-1".into(),
            name: "Line".into(),
            host: "127.0.0.1".into(),
            width: 2,
            height: 1,
            modules: 1,
            firmware: String::new(),
            rssi: None,
            bit_depth: 8,
            frame_rate: 120,
        };
        let mapping = Mapping {
            node_id: node.id.clone(),
            ndi_source: Some("test".into()),
            fit: crate::canvas::Fit::Stretch,
            transform: Default::default(),
            enabled: true,
        };
        let route = Route { node, mapping };
        let mut sources: HashMap<String, Box<dyn VideoSource>> = HashMap::new();
        sources.insert(
            "test".into(),
            Box::new(SolidSource {
                pixels: vec![255, 0, 0, 255, 0, 255, 0, 255],
            }),
        );
        feed.play(vec![route], sources);

        let deadline = Instant::now() + Duration::from_secs(1);
        let preview = loop {
            if let Some(preview) = feed.preview("line-1", 0) {
                break preview;
            }
            assert!(Instant::now() < deadline, "preview was not produced");
            std::thread::sleep(Duration::from_millis(10));
        };
        assert_eq!(preview.pixels, vec![255, 0, 0, 0, 255, 0]);
    }
}
