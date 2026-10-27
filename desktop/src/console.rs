use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::Serialize;

use crate::canvas::{Fit, Rotation};
use crate::device::{self, Client, DeviceStatus};
use crate::discovery::{Discovery, MdnsDiscovery};
use crate::feed::{Feed, Route, Stats};
use crate::pairing::{self, Access, Key};
use crate::router_name::RouterNames;
use crate::settings::{Mapping, Node, Settings};
use crate::source::ndi::Ndi;
use crate::source::VideoSource;

const STATUS_INTERVAL: Duration = Duration::from_secs(5);
/// A node whose last successful status is older than this is shown as not
/// responding. It stays listed and keeps its video route either way. 20 s
/// covers one full timed-out poll (device::STATUS_TIMEOUT) plus the next
/// interval, so a single slow answer from a busy ESP does not flip the UI.
const STATUS_GRACE: Duration = Duration::from_secs(20);
/// A node that has not answered for this long leaves the list; it comes back
/// with its next answer. Nodes from settings that have not answered since the
/// console started are not listed at all — the list shows the lines that are
/// actually on the network, not every line ever connected.
const LIST_GRACE: Duration = Duration::from_secs(5 * 60);
const MDNS_SCAN: Duration = Duration::from_millis(700);
const NDI_INTERVAL: Duration = Duration::from_secs(1);
const NDI_DISCOVERY_WAIT: Duration = Duration::from_millis(100);

#[derive(Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OutputView {
    running: bool,
    fps: f32,
    megabits_per_second: f32,
    error: Option<String>,
    device_fps: u16,
    queue_depth: u16,
    repeated: u32,
    late: u32,
    evicted: u32,
    torn: u32,
    jitter_us_max: u32,
    source_fps: f32,
    target_fps: f32,
    /// Сколько задержки строка удерживает; 0 — ещё не сообщила.
    capacity_ms: u32,
    clock_synced: bool,
    clock_rtt_us: u32,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NodeView {
    id: String,
    name: String,
    host: String,
    width: u16,
    height: u16,
    modules: u16,
    firmware: String,
    rssi: Option<i16>,
    online: bool,
    /// Console holds a key for this line that the line has not turned down.
    paired: bool,
    /// Имя строки в DNS роутера (`ledcast-a4dc.lan`); `None` — роутер его не знает.
    router_name: Option<String>,
    /// Seconds since the last successful status; `None` if the node has not
    /// answered since the console started.
    last_seen_secs: Option<u64>,
    signal: bool,
    device_fps: f32,
    brightness: u8,
    show_enabled: Option<bool>,
    show_control: bool,
    /// Строка показывает тест, а не поток; `None` — теста нет.
    test_pattern: Option<String>,
    owner: Option<String>,
    mapping: Mapping,
    output: OutputView,
}

#[derive(Clone, Default, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ConsoleSnapshot {
    nodes: Vec<NodeView>,
    ndi_sources: Vec<String>,
    ndi_version: Option<String>,
    ndi_error: Option<String>,
    running: bool,
    traffic_mbps: f32,
    discovering: bool,
    notice: Option<String>,
    /// Заданная задержка показа и та, с которой идёт вывод (меньше заданной,
    /// если какой-то строке не хватает памяти).
    latency_ms: u32,
    applied_latency_ms: u32,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PreviewView {
    node_id: String,
    width: u16,
    height: u16,
    pixels: Vec<u8>,
    generation: u64,
}

enum Command {
    Discover(Option<String>),
    UpdateMapping {
        node_id: String,
        ndi_source: Option<String>,
        fit: Fit,
        rotation: Rotation,
        mirror_x: bool,
    },
    UpdateBrightness {
        node_id: String,
        value: u8,
        reply: Sender<Result<(), String>>,
    },
    UpdateShowEnabled {
        node_id: String,
        enabled: bool,
        reply: Sender<Result<bool, String>>,
    },
    Play,
    Stop,
    SetLatency(u32),
    OpenWeb {
        node_id: String,
        reply: Sender<Result<(), String>>,
    },
    Paired {
        node_id: String,
        key: Key,
        session: device::Session,
        reply: Sender<()>,
    },
    Forget {
        node_id: String,
        reply: Sender<Result<(), String>>,
    },
    Session {
        node_id: String,
        reply: Sender<Result<String, String>>,
    },
}

pub struct ConsoleState {
    commands: Sender<Command>,
    snapshot: Arc<Mutex<ConsoleSnapshot>>,
    feed: Feed,
}

impl ConsoleState {
    fn new() -> Self {
        let (commands, inbox) = mpsc::channel();
        let snapshot = Arc::new(Mutex::new(ConsoleSnapshot::default()));
        let feed = Feed::start();
        let runtime_snapshot = Arc::clone(&snapshot);
        let runtime_feed = feed.clone();
        std::thread::Builder::new()
            .name("ledcast-console".into())
            .spawn(move || Runtime::new(runtime_feed, runtime_snapshot).run(inbox))
            .expect("LEDCAST console worker failed to start");
        Self {
            commands,
            snapshot,
            feed,
        }
    }
}

/// Result of one status request, run on its own thread.
struct Probe {
    host: String,
    started: Instant,
    result: Result<DeviceStatus, String>,
}

enum Event {
    Discovered(Vec<String>),
    Status(Probe),
}

struct Seen {
    status: DeviceStatus,
    at: Instant,
}

/// Last known status of every node. A failed or slow poll never removes a
/// node: it only stops refreshing `Seen::at`, and the node turns "not
/// responding" after `STATUS_GRACE`.
#[derive(Default)]
struct Statuses {
    seen: HashMap<String, Seen>,
    in_flight: HashSet<String>,
    written: HashMap<String, Instant>,
}

impl Statuses {
    /// Hosts to probe now. At most one request per host is in flight, so a
    /// node that needs 8 s to answer is not stacked with new polls and does
    /// not delay polls of other hosts.
    fn start(&mut self, hosts: impl IntoIterator<Item = String>) -> Vec<String> {
        hosts
            .into_iter()
            .filter(|host| self.in_flight.insert(host.clone()))
            .collect()
    }

    fn polling(&self, host: &str) -> bool {
        self.in_flight.contains(host)
    }

    /// Applies a finished probe; returns whether a video route changed.
    fn record(
        &mut self,
        settings: &mut Settings,
        probe: Probe,
        now: Instant,
    ) -> Result<bool, String> {
        self.in_flight.remove(&probe.host);
        let status = probe.result?;
        let node = device::node_from_status(&probe.host, status.clone());
        let route_changed = match settings.nodes.iter_mut().find(|item| item.id == node.id) {
            Some(existing) => {
                let changed = existing.host != node.host
                    || existing.width != node.width
                    || existing.height != node.height
                    || existing.frame_rate != node.frame_rate;
                *existing = node.clone();
                changed
            }
            None => {
                settings.nodes.push(node.clone());
                true
            }
        };
        settings.mapping_mut(&node.id);
        // A poll that started before a confirmed configuration write may
        // carry the old value; it still proves the node is alive.
        let outdated = self
            .written
            .get(&node.id)
            .is_some_and(|written| probe.started < *written);
        match self.seen.get_mut(&node.id) {
            Some(seen) if outdated => seen.at = now,
            _ => {
                self.seen.insert(node.id, Seen { status, at: now });
            }
        }
        Ok(route_changed)
    }

    /// Status to update after a write the node confirmed at `now`.
    fn confirmed(&mut self, node_id: &str, now: Instant) -> Option<&mut DeviceStatus> {
        self.written.insert(node_id.to_owned(), now);
        self.seen.get_mut(node_id).map(|seen| &mut seen.status)
    }

    fn get(&self, node_id: &str) -> Option<&DeviceStatus> {
        self.seen.get(node_id).map(|seen| &seen.status)
    }

    fn last_seen(&self, node_id: &str, now: Instant) -> Option<Duration> {
        self.seen
            .get(node_id)
            .map(|seen| now.saturating_duration_since(seen.at))
    }

    fn online(&self, node_id: &str, now: Instant) -> bool {
        self.last_seen(node_id, now)
            .is_some_and(|age| age < STATUS_GRACE)
    }

    fn listed(&self, node_id: &str, now: Instant) -> bool {
        self.last_seen(node_id, now)
            .is_some_and(|age| age < LIST_GRACE)
    }
}

struct Runtime {
    settings: Settings,
    feed: Feed,
    ndi: Result<Ndi, String>,
    ndi_sources: Vec<String>,
    statuses: Statuses,
    access: Access,
    router_names: RouterNames,
    events: Sender<Event>,
    inbox: Receiver<Event>,
    snapshot: Arc<Mutex<ConsoleSnapshot>>,
    notice: Option<String>,
    scanning: bool,
    /// Hosts from the current discovery round that have not answered yet.
    discovery: BTreeSet<String>,
    last_refresh: Instant,
    last_ndi: Instant,
    operator_paused: bool,
}

impl Runtime {
    fn new(feed: Feed, snapshot: Arc<Mutex<ConsoleSnapshot>>) -> Self {
        let mut settings = Settings::load();
        if !crate::feed::LATENCIES_MS.contains(&settings.latency_ms) {
            settings.latency_ms = crate::feed::DEFAULT_LATENCY_MS;
        }
        feed.set_latency(settings.latency_ms);
        let (events, inbox) = mpsc::channel();
        Self {
            settings,
            feed,
            ndi: Ndi::load(),
            ndi_sources: Vec::new(),
            statuses: Statuses::default(),
            access: Access::default(),
            router_names: RouterNames::default(),
            events,
            inbox,
            snapshot,
            notice: None,
            scanning: false,
            discovery: BTreeSet::new(),
            last_refresh: Instant::now(),
            last_ndi: Instant::now() - NDI_INTERVAL,
            operator_paused: false,
        }
    }

    fn run(mut self, inbox: Receiver<Command>) {
        // Discovery and status polls run on their own threads and report
        // back through `self.inbox`; this loop never waits on the network.
        self.discover(None);
        loop {
            match inbox.recv_timeout(Duration::from_millis(50)) {
                Ok(command) => self.command(command),
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => {
                    self.feed.stop();
                    return;
                }
            }
            while let Ok(command) = inbox.try_recv() {
                self.command(command);
            }
            while let Ok(event) = self.inbox.try_recv() {
                self.event(event);
            }
            if self.last_ndi.elapsed() >= NDI_INTERVAL {
                if let Ok(ndi) = &self.ndi {
                    // NDI discovery is event driven. A zero-timeout read only
                    // returns the finder's last snapshot and, with some NDI
                    // runtimes, never observes a source created after startup.
                    // Wait briefly for the change notification on every poll;
                    // capture and output run on separate workers, so this does
                    // not touch the video hot path.
                    let mut sources = ndi.sources(NDI_DISCOVERY_WAIT);
                    sources.sort();
                    sources.dedup();
                    if sources != self.ndi_sources {
                        let before = self.routed();
                        self.ndi_sources = sources;
                        // Merely adding/removing an option must not restart all
                        // active receivers, and a mapped source that drops out
                        // of the list for a moment keeps its receiver. Only a
                        // mapped source that becomes available (e.g. right
                        // after launch) starts the feed.
                        if !self.routed().is_subset(&before) && !self.operator_paused {
                            self.play();
                        }
                    }
                }
                self.last_ndi = Instant::now();
            }
            if self.last_refresh.elapsed() >= STATUS_INTERVAL {
                self.poll_known();
            }
            self.publish();
        }
    }

    fn command(&mut self, command: Command) {
        match command {
            Command::Discover(host) => self.discover(host),
            Command::UpdateMapping {
                node_id,
                ndi_source,
                fit,
                rotation,
                mirror_x,
            } => {
                let mapping = self.settings.mapping_mut(&node_id);
                mapping.enabled = ndi_source.is_some();
                mapping.ndi_source = ndi_source;
                mapping.fit = fit;
                mapping.transform.rotation = rotation;
                mapping.transform.mirror_x = mirror_x;
                self.settings.save();
                if !self.operator_paused {
                    self.play();
                }
            }
            Command::UpdateBrightness {
                node_id,
                value,
                reply,
            } => {
                let result = self.host(&node_id).and_then(|host| {
                    self.access
                        .request(&node_id, &host, |client| client.set_brightness(value))
                });
                if result.is_ok() {
                    if let Some(status) = self.statuses.confirmed(&node_id, Instant::now()) {
                        status.display.brightness = value;
                    }
                }
                let _ = reply.send(result);
            }
            Command::UpdateShowEnabled {
                node_id,
                enabled,
                reply,
            } => {
                // Status polls run on their own threads. `Statuses::record`
                // drops a poll that started before this write was confirmed,
                // so it cannot overwrite the confirmed value.
                let supported = self
                    .statuses
                    .get(&node_id)
                    .is_some_and(|status| status.show_control && status.show_enabled.is_some());
                let result = if !supported {
                    Err("Эта прошивка не поддерживает управление Show из консоли".into())
                } else {
                    self.host(&node_id).and_then(|host| {
                        self.access
                            .request(&node_id, &host, |client| client.set_show_enabled(enabled))
                    })
                };
                if let Ok(confirmed) = result {
                    if let Some(status) = self.statuses.confirmed(&node_id, Instant::now()) {
                        status.show_enabled = Some(confirmed);
                    }
                }
                self.publish();
                let _ = reply.send(result);
            }
            Command::Play => {
                self.operator_paused = false;
                self.play();
            }
            Command::Stop => {
                self.operator_paused = true;
                self.stop();
            }
            Command::SetLatency(milliseconds) => {
                self.settings.latency_ms = milliseconds;
                self.settings.save();
                self.feed.set_latency(milliseconds);
                self.publish();
            }
            Command::OpenWeb { node_id, reply } => {
                let _ = reply.send(self.open_web(&node_id));
            }
            Command::Paired {
                node_id,
                key,
                session,
                reply,
            } => {
                self.access.paired_with(&node_id, key, session);
                self.publish();
                let _ = reply.send(());
            }
            Command::Forget { node_id, reply } => {
                let _ = reply.send(self.access.forget(&node_id));
                self.publish();
            }
            Command::Session { node_id, reply } => {
                let result = self
                    .host(&node_id)
                    .and_then(|host| self.access.live_token(&node_id, &host));
                self.publish();
                let _ = reply.send(result);
            }
        }
    }

    fn host(&self, node_id: &str) -> Result<String, String> {
        self.settings
            .nodes
            .iter()
            .find(|node| node.id == node_id)
            .map(|node| node.host.clone())
            .ok_or_else(|| "Строка не найдена".to_string())
    }

    fn discover(&mut self, manual_host: Option<String>) {
        if let Some(host) = manual_host {
            self.discovery.insert(host.clone());
            self.probe([host]);
        } else if !self.scanning {
            self.scanning = true;
            let events = self.events.clone();
            let spawned = std::thread::Builder::new()
                .name("ledcast-mdns".into())
                .spawn(move || {
                    let found = MdnsDiscovery.scan(MDNS_SCAN).unwrap_or_default();
                    let _ = events.send(Event::Discovered(found));
                });
            self.scanning = spawned.is_ok();
        }
        self.poll_known();
        self.publish();
    }

    fn poll_known(&mut self) {
        let hosts: Vec<String> = self
            .settings
            .nodes
            .iter()
            .map(|node| node.host.clone())
            .collect();
        self.probe(hosts);
        self.last_refresh = Instant::now();
    }

    fn probe(&mut self, hosts: impl IntoIterator<Item = String>) {
        for host in self.statuses.start(hosts) {
            if spawn_probe(host.clone(), self.events.clone()).is_err() {
                self.statuses.in_flight.remove(&host);
            }
        }
    }

    fn event(&mut self, event: Event) {
        match event {
            Event::Discovered(hosts) => {
                self.scanning = false;
                self.discovery.extend(hosts.iter().cloned());
                self.probe(hosts);
            }
            Event::Status(probe) => {
                self.discovery.remove(&probe.host);
                let first = self.settings.nodes.is_empty();
                match self
                    .statuses
                    .record(&mut self.settings, probe, Instant::now())
                {
                    Ok(route_changed) => {
                        for node in &self.settings.nodes {
                            self.access.load(&node.id);
                        }
                        self.settings.save();
                        if first {
                            self.notice = None;
                        }
                        // Only a changed route restarts the feed. Reachability
                        // over HTTP has no say in where video is sent.
                        if route_changed && !self.operator_paused {
                            self.play();
                        }
                    }
                    Err(error) => {
                        if self.settings.nodes.is_empty() {
                            self.notice = Some(error);
                        }
                    }
                }
            }
        }
    }

    fn routed(&self) -> BTreeSet<String> {
        self.routes()
            .into_iter()
            .map(|route| route.node.id)
            .collect()
    }

    fn routes(&self) -> Vec<Route> {
        self.settings
            .mappings
            .iter()
            .filter(|mapping| {
                mapping.enabled
                    && mapping
                        .ndi_source
                        .as_ref()
                        .is_some_and(|source| self.ndi_sources.contains(source))
            })
            .filter_map(|mapping| {
                self.settings
                    .nodes
                    .iter()
                    .find(|node| node.id == mapping.node_id)
                    .cloned()
                    .map(|node| Route {
                        node,
                        mapping: mapping.clone(),
                    })
            })
            .collect()
    }

    fn play(&mut self) {
        let routes = self.routes();
        if routes.is_empty() {
            self.feed.stop();
            self.notice = None;
            return;
        }
        let Some(ndi) = self.ndi.as_ref().ok() else {
            self.notice = self.ndi.as_ref().err().cloned();
            return;
        };
        let unique: BTreeSet<String> = routes
            .iter()
            .filter_map(|route| route.mapping.ndi_source.clone())
            .collect();
        let mut sources: HashMap<String, Box<dyn VideoSource>> = HashMap::new();
        for source_name in unique {
            match ndi.connect(&source_name) {
                Ok(receiver) => {
                    sources.insert(source_name, Box::new(receiver));
                }
                Err(error) => {
                    self.notice = Some(error);
                    return;
                }
            }
        }
        self.settings.save();
        self.feed.play(routes, sources);
        self.notice = None;
    }

    fn stop(&mut self) {
        self.feed.stop();
    }

    fn open_web(&mut self, node_id: &str) -> Result<(), String> {
        let node = self
            .settings
            .nodes
            .iter()
            .find(|node| node.id == node_id)
            .ok_or_else(|| "Строка не найдена".to_string())?;
        open::that(format!("http://{}/", node.host)).map_err(|error| error.to_string())
    }

    fn publish(&self) {
        let stats = self.feed.stats();
        let now = Instant::now();
        let nodes = self
            .settings
            .nodes
            .iter()
            .filter(|node| self.statuses.listed(&node.id, now))
            .map(|node| {
                let mut view = node_view(node, &self.settings, &self.statuses, &stats, now);
                view.paired = self.access.paired(&node.id);
                view.router_name = self.router_names.get(&node.host);
                view
            })
            .collect();
        *self.snapshot.lock().unwrap() = ConsoleSnapshot {
            nodes,
            ndi_sources: self.ndi_sources.clone(),
            ndi_version: self.ndi.as_ref().ok().map(Ndi::version),
            ndi_error: self.ndi.as_ref().err().cloned(),
            running: stats.running,
            traffic_mbps: stats
                .nodes
                .values()
                .map(|output| output.megabits_per_second)
                .sum(),
            discovering: self.scanning
                || self
                    .discovery
                    .iter()
                    .any(|host| self.statuses.polling(host)),
            notice: self.notice.clone(),
            latency_ms: self.settings.latency_ms,
            applied_latency_ms: if stats.running {
                stats.latency_ms
            } else {
                self.settings.latency_ms
            },
        };
    }
}

fn spawn_probe(host: String, events: Sender<Event>) -> std::io::Result<()> {
    std::thread::Builder::new()
        .name("ledcast-status".into())
        .spawn(move || {
            let started = Instant::now();
            let result = Client::new(&host).status();
            let _ = events.send(Event::Status(Probe {
                host,
                started,
                result,
            }));
        })
        .map(drop)
}

/// A listed node keeps its last known status while it does not answer;
/// `online` and `last_seen_secs` tell the UI how fresh it is.
fn node_view(
    node: &Node,
    settings: &Settings,
    statuses: &Statuses,
    stats: &Stats,
    now: Instant,
) -> NodeView {
    let status = statuses.get(&node.id);
    let online = statuses.online(&node.id, now);
    let output = stats.nodes.get(&node.id);
    NodeView {
        id: node.id.clone(),
        name: node.name.clone(),
        host: node.host.clone(),
        width: node.width,
        height: node.height,
        modules: node.modules,
        firmware: node.firmware.clone(),
        rssi: status.and_then(|value| value.rssi).or(node.rssi),
        online,
        paired: false,
        router_name: None,
        last_seen_secs: statuses.last_seen(&node.id, now).map(|age| age.as_secs()),
        signal: online && status.is_some_and(|value| value.signal),
        device_fps: output.filter(|value| value.device_fps != 0).map_or_else(
            || status.filter(|_| online).map_or(0.0, |value| value.fps),
            |value| value.device_fps as f32,
        ),
        brightness: status.map_or(128, |value| value.display.brightness),
        show_enabled: status.and_then(|value| value.show_enabled),
        show_control: status.is_some_and(|value| value.show_control),
        test_pattern: status
            .filter(|value| online && !value.test_pattern.is_empty())
            .map(|value| value.test_pattern.clone()),
        owner: status.and_then(|value| value.owner.clone()),
        mapping: settings
            .mappings
            .iter()
            .find(|mapping| mapping.node_id == node.id)
            .cloned()
            .unwrap_or_else(|| Mapping {
                node_id: node.id.clone(),
                ndi_source: None,
                fit: Fit::Cover,
                transform: Default::default(),
                enabled: false,
            }),
        output: output.map_or_else(OutputView::default, |value| OutputView {
            running: value.running,
            fps: value.fps,
            megabits_per_second: value.megabits_per_second,
            error: value.error.clone(),
            device_fps: value.device_fps,
            queue_depth: value.queue_depth,
            repeated: value.repeated,
            late: value.late,
            evicted: value.evicted,
            torn: value.torn,
            jitter_us_max: value.jitter_us_max,
            source_fps: value.source_fps,
            target_fps: value.target_fps,
            capacity_ms: value.capacity_ms,
            clock_synced: value.clock_synced,
            clock_rtt_us: value.clock_rtt_us,
        }),
    }
}

#[tauri::command]
fn snapshot(state: tauri::State<'_, ConsoleState>) -> ConsoleSnapshot {
    state.snapshot.lock().unwrap().clone()
}

#[tauri::command]
fn discover(state: tauri::State<'_, ConsoleState>, host: Option<String>) -> Result<(), String> {
    state
        .commands
        .send(Command::Discover(
            host.filter(|value| !value.trim().is_empty()),
        ))
        .map_err(|error| error.to_string())
}

#[tauri::command]
fn update_mapping(
    state: tauri::State<'_, ConsoleState>,
    node_id: String,
    ndi_source: Option<String>,
    fit: Fit,
    rotation: Rotation,
    mirror_x: bool,
) -> Result<(), String> {
    state
        .commands
        .send(Command::UpdateMapping {
            node_id,
            ndi_source,
            fit,
            rotation,
            mirror_x,
        })
        .map_err(|error| error.to_string())
}

#[tauri::command]
fn update_brightness(
    state: tauri::State<'_, ConsoleState>,
    node_id: String,
    value: u8,
) -> Result<(), String> {
    let (sender, receiver) = mpsc::channel();
    state
        .commands
        .send(Command::UpdateBrightness {
            node_id,
            value,
            reply: sender,
        })
        .map_err(|error| error.to_string())?;
    receiver
        .recv_timeout(Duration::from_secs(3))
        .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn update_show_enabled(
    state: tauri::State<'_, ConsoleState>,
    node_id: String,
    enabled: bool,
) -> Result<bool, String> {
    let (sender, receiver) = mpsc::channel();
    state
        .commands
        .send(Command::UpdateShowEnabled {
            node_id,
            enabled,
            reply: sender,
        })
        .map_err(|error| error.to_string())?;
    tauri::async_runtime::spawn_blocking(move || {
        receiver
            .recv_timeout(Duration::from_secs(10))
            .map_err(|error| error.to_string())?
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
fn play(state: tauri::State<'_, ConsoleState>) -> Result<(), String> {
    state
        .commands
        .send(Command::Play)
        .map_err(|error| error.to_string())
}

#[tauri::command]
fn stop(state: tauri::State<'_, ConsoleState>) -> Result<(), String> {
    state
        .commands
        .send(Command::Stop)
        .map_err(|error| error.to_string())
}

#[tauri::command]
fn set_latency(state: tauri::State<'_, ConsoleState>, milliseconds: u32) -> Result<(), String> {
    if !crate::feed::LATENCIES_MS.contains(&milliseconds) {
        return Err(format!("Задержка {milliseconds} мс не поддерживается"));
    }
    state
        .commands
        .send(Command::SetLatency(milliseconds))
        .map_err(|error| error.to_string())
}

#[tauri::command]
fn open_web(state: tauri::State<'_, ConsoleState>, node_id: String) -> Result<(), String> {
    let (sender, receiver) = mpsc::channel();
    state
        .commands
        .send(Command::OpenWeb {
            node_id,
            reply: sender,
        })
        .map_err(|error| error.to_string())?;
    receiver
        .recv_timeout(Duration::from_secs(6))
        .map_err(|error| error.to_string())?
}

fn listed_line(state: &ConsoleState, node_id: &str) -> Result<(String, String), String> {
    state
        .snapshot
        .lock()
        .unwrap()
        .nodes
        .iter()
        .find(|node| node.id == node_id)
        .map(|node| (node.name.clone(), node.host.clone()))
        .ok_or_else(|| "Строка не найдена".to_string())
}

/// Подключает строку по её паролю: ключ выводится из пароля, проверяется входом
/// и только потом попадает в хранилище ОС. Сам пароль нигде не сохраняется.
#[tauri::command]
async fn pair_line(
    state: tauri::State<'_, ConsoleState>,
    node_id: String,
    password: String,
) -> Result<(), String> {
    if password.is_empty() {
        return Err("Введите пароль строки".into());
    }
    let (_, host) = listed_line(&state, &node_id)?;
    let id = node_id.clone();
    let (key, session) =
        tauri::async_runtime::spawn_blocking(move || pairing::pair(&host, &id, &password))
            .await
            .map_err(|error| error.to_string())??;
    // The reply comes after the snapshot is republished: the next snapshot
    // the UI asks for already shows the line as paired.
    let (sender, receiver) = mpsc::channel();
    state
        .commands
        .send(Command::Paired {
            node_id,
            key,
            session,
            reply: sender,
        })
        .map_err(|error| error.to_string())?;
    tauri::async_runtime::spawn_blocking(move || {
        receiver
            .recv_timeout(Duration::from_secs(10))
            .map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
async fn forget_line(state: tauri::State<'_, ConsoleState>, node_id: String) -> Result<(), String> {
    let (sender, receiver) = mpsc::channel();
    state
        .commands
        .send(Command::Forget {
            node_id,
            reply: sender,
        })
        .map_err(|error| error.to_string())?;
    tauri::async_runtime::spawn_blocking(move || {
        receiver
            .recv_timeout(Duration::from_secs(10))
            .map_err(|error| error.to_string())?
    })
    .await
    .map_err(|error| error.to_string())?
}

/// Веб-интерфейс строки в отдельном окне Console; повторное открытие поднимает уже открытое.
/// Окно входит само: сессию строки Console получает своим ключом и передаёт странице.
#[tauri::command]
async fn open_line_window(
    app: tauri::AppHandle,
    state: tauri::State<'_, ConsoleState>,
    node_id: String,
) -> Result<(), String> {
    use tauri::Manager;

    let (name, host) = listed_line(&state, &node_id)?;
    let label: String = format!("line-{node_id}")
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' {
                c
            } else {
                '-'
            }
        })
        .collect();
    if let Some(window) = app.get_webview_window(&label) {
        return window.set_focus().map_err(|error| error.to_string());
    }
    // Сначала локальная заставка: страница строки идёт по Wi‑Fi секунду-другую.
    let loader = format!("line-loader.html?host={}", line_host(&host)?);
    let (sender, receiver) = mpsc::channel();
    state
        .commands
        .send(Command::Session {
            node_id,
            reply: sender,
        })
        .map_err(|error| error.to_string())?;
    let token = tauri::async_runtime::spawn_blocking(move || {
        receiver
            .recv_timeout(Duration::from_secs(10))
            .map_err(|error| error.to_string())?
    })
    .await
    .map_err(|error| error.to_string())??;
    let session = serde_json::to_string(&token).map_err(|error| error.to_string())?;
    let title = if name.is_empty() { host } else { name };
    tauri::WebviewWindowBuilder::new(&app, label, tauri::WebviewUrl::App(loader.into()))
        .initialization_script(format!("window.__LEDCAST_SESSION__ = {session};"))
        // Фон интерфейса строки: окно не вспыхивает белым до первого кадра.
        .background_color(tauri::window::Color(13, 15, 19, 255))
        .title(format!("{title} — LEDCAST"))
        .inner_size(1180.0, 800.0)
        .min_inner_size(360.0, 560.0)
        .build()
        .map(|_| ())
        .map_err(|error| error.to_string())
}

/// Адрес строки для заставки: имя или IPv4 с портом, без символов разметки URL.
fn line_host(host: &str) -> Result<&str, String> {
    let valid = !host.is_empty()
        && host
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | ':'));
    if valid {
        Ok(host)
    } else {
        Err(format!("Неверный адрес строки: {host}"))
    }
}

#[tauri::command]
fn preview(
    state: tauri::State<'_, ConsoleState>,
    node_id: String,
    generation: u64,
) -> Option<PreviewView> {
    state
        .feed
        .preview(&node_id, generation)
        .map(|value| PreviewView {
            node_id: value.node_id,
            width: value.width,
            height: value.height,
            pixels: value.pixels,
            generation: value.generation,
        })
}

pub fn run() {
    tauri::Builder::default()
        .manage(ConsoleState::new())
        .invoke_handler(tauri::generate_handler![
            snapshot,
            discover,
            update_mapping,
            update_brightness,
            update_show_enabled,
            play,
            stop,
            set_latency,
            open_web,
            open_line_window,
            pair_line,
            forget_line,
            preview
        ])
        .run(tauri::generate_context!())
        .expect("LEDCAST Console failed to start");
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};
    use std::net::TcpListener;

    fn status(id: &str, brightness: u8) -> DeviceStatus {
        DeviceStatus {
            id: id.into(),
            name: "LEDCAST Node".into(),
            width: 160,
            height: 16,
            signal: true,
            fps: 60.0,
            display: device::DisplayStatus {
                brightness,
                frame_rate: 120,
            },
            ..DeviceStatus::default()
        }
    }

    fn answer(host: &str, started: Instant, result: Result<DeviceStatus, String>) -> Probe {
        Probe {
            host: host.into(),
            started,
            result,
        }
    }

    #[test]
    fn failed_polls_keep_node_listed_and_only_mark_it_stale() {
        let mut settings = Settings::default();
        let mut statuses = Statuses::default();
        let t0 = Instant::now();
        statuses.start(["10.0.0.5".to_string()]);
        let added = statuses.record(
            &mut settings,
            answer("10.0.0.5", t0, Ok(status("line", 90))),
            t0,
        );
        assert_eq!(added, Ok(true));

        for second in 1..=6 {
            let at = t0 + Duration::from_secs(second * 5);
            statuses.start(["10.0.0.5".to_string()]);
            let failed = statuses.record(
                &mut settings,
                answer("10.0.0.5", at, Err("timeout".into())),
                at,
            );
            assert_eq!(
                failed,
                Err("timeout".into()),
                "a failure never changes a route"
            );
        }

        assert_eq!(settings.nodes.len(), 1);
        let stats = Stats::default();
        let fresh = node_view(
            &settings.nodes[0],
            &settings,
            &statuses,
            &stats,
            t0 + Duration::from_secs(19),
        );
        assert!(fresh.online, "inside the grace period");
        assert!(fresh.signal);
        let stale = node_view(
            &settings.nodes[0],
            &settings,
            &statuses,
            &stats,
            t0 + Duration::from_secs(31),
        );
        assert!(!stale.online);
        assert!(!stale.signal);
        assert_eq!(stale.last_seen_secs, Some(31));
        assert_eq!(stale.brightness, 90, "last known status is kept");
    }

    #[test]
    fn silent_node_leaves_the_list_and_returns_with_its_answer() {
        let mut settings = Settings::default();
        let mut statuses = Statuses::default();
        let t0 = Instant::now();
        assert!(!statuses.listed("line", t0), "never answered — not listed");
        statuses.start(["10.0.0.5".to_owned()]);
        statuses
            .record(
                &mut settings,
                Probe {
                    host: "10.0.0.5".into(),
                    started: t0,
                    result: Ok(status("line", 0)),
                },
                t0,
            )
            .unwrap();
        assert!(statuses.listed("line", t0 + LIST_GRACE - Duration::from_secs(1)));
        assert!(!statuses.listed("line", t0 + LIST_GRACE));
        let back = t0 + LIST_GRACE + Duration::from_secs(30);
        statuses.start(["10.0.0.5".to_owned()]);
        statuses
            .record(
                &mut settings,
                Probe {
                    host: "10.0.0.5".into(),
                    started: back,
                    result: Ok(status("line", 0)),
                },
                back,
            )
            .unwrap();
        assert!(statuses.listed("line", back));
    }

    #[test]
    fn only_a_changed_node_changes_the_route() {
        let mut settings = Settings::default();
        let mut statuses = Statuses::default();
        let now = Instant::now();
        assert_eq!(
            statuses.record(
                &mut settings,
                answer("10.0.0.5", now, Ok(status("line", 1))),
                now
            ),
            Ok(true)
        );
        assert_eq!(
            statuses.record(
                &mut settings,
                answer("10.0.0.5", now, Ok(status("line", 2))),
                now
            ),
            Ok(false)
        );
        assert_eq!(
            statuses.record(
                &mut settings,
                answer("10.0.0.6", now, Ok(status("line", 2))),
                now
            ),
            Ok(true)
        );
        assert_eq!(settings.nodes.len(), 1);
        assert_eq!(settings.nodes[0].host, "10.0.0.6");
    }

    #[test]
    fn poll_started_before_a_confirmed_write_does_not_undo_it() {
        let mut settings = Settings::default();
        let mut statuses = Statuses::default();
        let t0 = Instant::now();
        statuses
            .record(&mut settings, answer("h", t0, Ok(status("line", 10))), t0)
            .unwrap();
        let written = t0 + Duration::from_secs(1);
        statuses
            .confirmed("line", written)
            .unwrap()
            .display
            .brightness = 200;

        let late = t0 + Duration::from_secs(3);
        statuses
            .record(&mut settings, answer("h", t0, Ok(status("line", 10))), late)
            .unwrap();
        assert_eq!(statuses.get("line").unwrap().display.brightness, 200);
        assert_eq!(
            statuses.last_seen("line", late),
            Some(Duration::ZERO),
            "still proves liveness"
        );

        let after = written + Duration::from_millis(1);
        statuses
            .record(
                &mut settings,
                answer("h", after, Ok(status("line", 50))),
                late,
            )
            .unwrap();
        assert_eq!(statuses.get("line").unwrap().display.brightness, 50);
    }

    #[test]
    fn one_request_per_host_is_in_flight() {
        let mut settings = Settings::default();
        let mut statuses = Statuses::default();
        assert_eq!(
            statuses.start(["a".to_string(), "b".to_string()]),
            ["a", "b"]
        );
        assert_eq!(
            statuses.start(["a".to_string(), "c".to_string()]),
            ["c"],
            "slow a is not stacked, c is not delayed"
        );
        let now = Instant::now();
        let _ = statuses.record(&mut settings, answer("a", now, Err("timeout".into())), now);
        assert!(!statuses.polling("a"));
        assert_eq!(statuses.start(["a".to_string()]), ["a"]);
    }

    #[test]
    fn slow_node_does_not_delay_another_nodes_status() {
        let silent = TcpListener::bind("127.0.0.1:0").unwrap();
        let slow = silent.local_addr().unwrap().to_string();
        let server = TcpListener::bind("127.0.0.1:0").unwrap();
        let fast = server.local_addr().unwrap().to_string();
        std::thread::spawn(move || {
            let (mut stream, _) = server.accept().unwrap();
            let mut request = [0; 1024];
            let _ = stream.read(&mut request);
            let body = r#"{"id":"fast","width":160,"height":16,"display":{"frame_rate":120}}"#;
            write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body).unwrap();
        });

        let (events, inbox) = mpsc::channel();
        spawn_probe(slow.clone(), events.clone()).unwrap();
        spawn_probe(fast.clone(), events).unwrap();
        let Event::Status(probe) = inbox.recv_timeout(Duration::from_secs(3)).unwrap() else {
            panic!("expected a status");
        };
        assert_eq!(probe.host, fast);
        assert_eq!(probe.result.unwrap().id, "fast");
        drop(silent);
    }
}
