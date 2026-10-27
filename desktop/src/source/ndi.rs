//! Приём видео по NDI.
//!
//! Библиотека грузится в рантайме, а не линкуется: NDI распространяется своим
//! установщиком, и приложение должно собираться и запускаться на машине, где
//! его нет — тогда вход просто недоступен, а проверочная картинка работает.
//!
//! Структуры повторяют Processing.NDI.structs.h из NDI SDK. Расхождение в
//! разметке полей проявится не отказом, а порчей памяти, поэтому порядок и
//! типы менять нельзя.

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::Arc;
use std::time::{Duration, Instant};

use libloading::{Library, Symbol};

use super::{FrameView, PixelLayout, VideoSource};

#[repr(C)]
#[derive(Clone, Copy)]
struct RawSource {
    ndi_name: *const c_char,
    url_address: *const c_char,
}

#[repr(C)]
struct FindCreate {
    show_local_sources: bool,
    groups: *const c_char,
    extra_ips: *const c_char,
}

#[repr(C)]
struct RecvCreate {
    source_to_connect_to: RawSource,
    color_format: i32,
    bandwidth: i32,
    allow_video_fields: bool,
    recv_name: *const c_char,
}

#[repr(C)]
struct VideoFrame {
    xres: i32,
    yres: i32,
    four_cc: i32,
    frame_rate_n: i32,
    frame_rate_d: i32,
    picture_aspect_ratio: f32,
    frame_format_type: i32,
    timecode: i64,
    data: *mut u8,
    line_stride_in_bytes: i32,
    metadata: *const c_char,
    timestamp: i64,
}

impl Default for VideoFrame {
    fn default() -> Self {
        // Нули — то, что SDK ждёт от неинициализированного кадра.
        unsafe { std::mem::zeroed() }
    }
}

/// Всегда четыре байта на точку: BGRX без альфы, BGRA с ней. Так отпадает
/// разбор UYVY, который иначе приезжает от половины отправителей.
const COLOR_FORMAT_BGRX_BGRA: i32 = 0;
const BANDWIDTH_HIGHEST: i32 = 100;
const FRAME_FORMAT_PROGRESSIVE: i32 = 1;

const FOURCC_BGRA: i32 = i32::from_le_bytes(*b"BGRA");
const FOURCC_BGRX: i32 = i32::from_le_bytes(*b"BGRX");
const FOURCC_RGBA: i32 = i32::from_le_bytes(*b"RGBA");
const FOURCC_RGBX: i32 = i32::from_le_bytes(*b"RGBX");

struct Api {
    initialize: unsafe extern "C" fn() -> bool,
    version: unsafe extern "C" fn() -> *const c_char,
    find_create: unsafe extern "C" fn(*const FindCreate) -> *mut c_void,
    find_destroy: unsafe extern "C" fn(*mut c_void),
    find_current_sources: unsafe extern "C" fn(*mut c_void, *mut u32) -> *const RawSource,
    find_wait: unsafe extern "C" fn(*mut c_void, u32) -> bool,
    recv_create: unsafe extern "C" fn(*const RecvCreate) -> *mut c_void,
    recv_destroy: unsafe extern "C" fn(*mut c_void),
    framesync_create: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
    framesync_destroy: unsafe extern "C" fn(*mut c_void),
    framesync_capture: unsafe extern "C" fn(*mut c_void, *mut VideoFrame, i32),
    framesync_free_video: unsafe extern "C" fn(*mut c_void, *mut VideoFrame),
}

/// Загруженная библиотека NDI и поиск источников. Живёт в потоке интерфейса:
/// поиск в NDI не рассчитан на параллельные вызовы, поэтому наружу отдаётся
/// только таблица функций, а сам поиск остаётся здесь.
pub struct Ndi {
    library: Arc<Library>,
    api: Arc<Api>,
    finder: *mut c_void,
}

/// Где искать библиотеку. Установщик NDI кладёт её в системный каталог, SDK
/// сообщает свой путь переменной окружения, а Resolume приносит свою копию —
/// с ней отдельный установщик NDI не нужен вовсе.
fn library_paths() -> Vec<String> {
    let mut paths = Vec::new();
    for variable in [
        "NDI_RUNTIME_DIR_V6",
        "NDI_RUNTIME_DIR_V5",
        "NDI_RUNTIME_DIR_V4",
    ] {
        if let Ok(dir) = std::env::var(variable) {
            paths.push(format!("{dir}/{}", library_name()));
        }
    }
    if cfg!(target_os = "windows") {
        paths.push(r"C:\Program Files\Resolume Arena\Processing.NDI.Lib.x64.dll".to_string());
        paths.push(r"C:\Program Files\Resolume Avenue\Processing.NDI.Lib.x64.dll".to_string());
        paths.push(library_name().to_string());
    } else {
        // Many video applications bundle the official runtime instead of
        // installing it system-wide: inside their .app, or — like Resolume —
        // next to the .app in the application folder. Looking only for a
        // particular product makes a portable console unnecessarily brittle,
        // so accept these standard locations for any application.
        if let Ok(applications) = std::fs::read_dir("/Applications") {
            for application in applications.flatten() {
                for relative in [
                    "libndi.dylib",
                    "Contents/Frameworks/libndi.dylib",
                    "Contents/MacOS/libndi.dylib",
                ] {
                    let candidate = application.path().join(relative);
                    if candidate.is_file() {
                        paths.push(candidate.to_string_lossy().into_owned());
                    }
                }
            }
        }
        paths.push(format!("/usr/local/lib/{}", library_name()));
        paths.push("/Library/NDI SDK for Apple/lib/macOS/libndi.dylib".to_string());
        paths.push(library_name().to_string());
    }
    paths
}

fn library_name() -> &'static str {
    if cfg!(target_os = "windows") {
        "Processing.NDI.Lib.x64.dll"
    } else if cfg!(target_os = "macos") {
        "libndi.dylib"
    } else {
        "libndi.so.6"
    }
}

impl Ndi {
    pub fn load() -> Result<Self, String> {
        let mut last = String::new();
        for path in library_paths() {
            match unsafe { Library::new(&path) } {
                Ok(library) => return Self::bind(library),
                Err(err) => last = format!("{path}: {err}"),
            }
        }
        Err(format!(
            "библиотека NDI не найдена ({last}). Поставьте NDI Tools или NDI SDK с ndi.video"
        ))
    }

    fn bind(library: Library) -> Result<Self, String> {
        unsafe fn symbol<T: Copy>(library: &Library, name: &[u8]) -> Result<T, String> {
            let found: Symbol<T> = library.get(name).map_err(|err| {
                format!(
                    "в библиотеке NDI нет {}: {err}",
                    String::from_utf8_lossy(name)
                )
            })?;
            Ok(*found)
        }

        let api = unsafe {
            Api {
                initialize: symbol(&library, b"NDIlib_initialize\0")?,
                version: symbol(&library, b"NDIlib_version\0")?,
                find_create: symbol(&library, b"NDIlib_find_create_v2\0")?,
                find_destroy: symbol(&library, b"NDIlib_find_destroy\0")?,
                find_current_sources: symbol(&library, b"NDIlib_find_get_current_sources\0")?,
                find_wait: symbol(&library, b"NDIlib_find_wait_for_sources\0")?,
                recv_create: symbol(&library, b"NDIlib_recv_create_v3\0")?,
                recv_destroy: symbol(&library, b"NDIlib_recv_destroy\0")?,
                framesync_create: symbol(&library, b"NDIlib_framesync_create\0")?,
                framesync_destroy: symbol(&library, b"NDIlib_framesync_destroy\0")?,
                framesync_capture: symbol(&library, b"NDIlib_framesync_capture_video\0")?,
                framesync_free_video: symbol(&library, b"NDIlib_framesync_free_video\0")?,
            }
        };

        if !unsafe { (api.initialize)() } {
            return Err("NDI не запустился: процессор не поддерживается библиотекой".into());
        }

        let settings = FindCreate {
            show_local_sources: true,
            groups: std::ptr::null(),
            extra_ips: std::ptr::null(),
        };
        let finder = unsafe { (api.find_create)(&settings) };
        if finder.is_null() {
            return Err("NDI не отдал поиск источников".into());
        }

        Ok(Self {
            library: Arc::new(library),
            api: Arc::new(api),
            finder,
        })
    }

    pub fn version(&self) -> String {
        unsafe {
            let raw = (self.api.version)();
            if raw.is_null() {
                return String::new();
            }
            CStr::from_ptr(raw).to_string_lossy().into_owned()
        }
    }

    /// Имена источников, видимых сейчас. Ненулевое ожидание позволяет SDK
    /// доставить уведомление об источнике, появившемся уже после запуска.
    pub fn sources(&self, wait: Duration) -> Vec<String> {
        unsafe {
            (self.api.find_wait)(self.finder, wait.as_millis() as u32);
            let mut count = 0u32;
            let first = (self.api.find_current_sources)(self.finder, &mut count);
            if first.is_null() {
                return Vec::new();
            }
            (0..count as usize)
                .map(|index| {
                    let source = &*first.add(index);
                    if source.ndi_name.is_null() {
                        String::new()
                    } else {
                        CStr::from_ptr(source.ndi_name)
                            .to_string_lossy()
                            .into_owned()
                    }
                })
                .filter(|name| !name.is_empty())
                .collect()
        }
    }

    pub fn connect(&self, name: &str) -> Result<NdiReceiver, String> {
        let source_name = CString::new(name).map_err(|_| "имя источника с нулевым байтом")?;
        let recv_name = CString::new("LEDCAST Console").unwrap();
        let settings = RecvCreate {
            source_to_connect_to: RawSource {
                ndi_name: source_name.as_ptr(),
                url_address: std::ptr::null(),
            },
            color_format: COLOR_FORMAT_BGRX_BGRA,
            bandwidth: BANDWIDTH_HIGHEST,
            allow_video_fields: false,
            recv_name: recv_name.as_ptr(),
        };

        let receiver = unsafe { (self.api.recv_create)(&settings) };
        if receiver.is_null() {
            return Err(format!("не удалось подключиться к источнику «{name}»"));
        }
        let framesync = unsafe { (self.api.framesync_create)(receiver) };
        if framesync.is_null() {
            unsafe { (self.api.recv_destroy)(receiver) };
            return Err(format!("не удалось синхронизировать источник «{name}»"));
        }
        Ok(NdiReceiver {
            _library: Arc::clone(&self.library),
            api: Arc::clone(&self.api),
            receiver,
            framesync,
            next_capture: Instant::now(),
            capture_period: Duration::from_nanos(1_000_000_000 / 60),
            timing: std::env::var_os("LEDCAST_TIMING_TRACE").map(|_| CaptureTiming::new()),
            frame: VideoFrame::default(),
            held: false,
            title: name.to_string(),
        })
    }
}

impl Drop for Ndi {
    fn drop(&mut self) {
        unsafe { (self.api.find_destroy)(self.finder) };
    }
}

/// Приёмник уезжает в поток подачи и держит библиотеку живой: выгруженная
/// библиотека оставила бы указатели на функции висеть в пустоте.
pub struct NdiReceiver {
    _library: Arc<Library>,
    api: Arc<Api>,
    receiver: *mut c_void,
    framesync: *mut c_void,
    next_capture: Instant,
    capture_period: Duration,
    timing: Option<CaptureTiming>,
    frame: VideoFrame,
    held: bool,
    title: String,
}

struct CaptureTiming {
    started: Instant,
    release_us_max: u128,
    wait_us_max: u128,
    overshoot_us_max: u128,
    sdk_us_max: u128,
}

impl CaptureTiming {
    fn new() -> Self {
        Self {
            started: Instant::now(),
            release_us_max: 0,
            wait_us_max: 0,
            overshoot_us_max: 0,
            sdk_us_max: 0,
        }
    }
}

// Приёмник принадлежит одному потоку, все вызовы к нему идут оттуда же.
unsafe impl Send for NdiReceiver {}

impl NdiReceiver {
    fn release(&mut self) {
        if self.held {
            unsafe { (self.api.framesync_free_video)(self.framesync, &mut self.frame) };
            self.held = false;
        }
    }
}

impl VideoSource for NdiReceiver {
    fn title(&self) -> String {
        self.title.clone()
    }

    fn next_frame(&mut self, timeout: Duration) -> Option<FrameView<'_>> {
        // Предыдущий кадр отдаётся библиотеке только сейчас: пока он не
        // освобождён, данные под ним валидны и их читает полотно.
        let release_started = self.timing.as_ref().map(|_| Instant::now());
        self.release();
        if let Some(timing) = &mut self.timing {
            timing.release_us_max = timing
                .release_us_max
                .max(release_started.unwrap().elapsed().as_micros());
        }

        // FrameSync corrects network jitter and source/local clock drift before
        // both preview and LED output see a frame. It is nonblocking: pull on
        // an absolute local clock, never in a free-running capture loop.
        let now = Instant::now();
        let wait = self.next_capture.saturating_duration_since(now);
        if wait > timeout {
            crate::stream::precise_wait(timeout);
            return None;
        }
        if !wait.is_zero() {
            let wait_started = self.timing.as_ref().map(|_| Instant::now());
            crate::stream::precise_wait(wait);
            if let Some(timing) = &mut self.timing {
                let elapsed = wait_started.unwrap().elapsed();
                timing.wait_us_max = timing.wait_us_max.max(elapsed.as_micros());
                timing.overshoot_us_max = timing
                    .overshoot_us_max
                    .max(elapsed.saturating_sub(wait).as_micros());
            }
        }
        let capture_time = self.next_capture.max(now);
        let sdk_started = self.timing.as_ref().map(|_| Instant::now());
        unsafe {
            (self.api.framesync_capture)(self.framesync, &mut self.frame, FRAME_FORMAT_PROGRESSIVE)
        };
        if let Some(timing) = &mut self.timing {
            timing.sdk_us_max = timing
                .sdk_us_max
                .max(sdk_started.unwrap().elapsed().as_micros());
            if timing.started.elapsed() >= Duration::from_secs(1) {
                let fourcc_bytes = self.frame.four_cc.to_le_bytes();
                let fourcc = std::str::from_utf8(&fourcc_bytes).unwrap_or("????");
                crate::feed::timing_trace(&format!("LEDCAST NDI timing: source={:?} fourcc={} wait_us_max={} wait_overshoot_us_max={} sdk_capture_us_max={} release_us_max={} rate={}/{}", self.title, fourcc, timing.wait_us_max, timing.overshoot_us_max, timing.sdk_us_max, timing.release_us_max, self.frame.frame_rate_n, self.frame.frame_rate_d));
                *timing = CaptureTiming::new();
            }
        }
        if self.frame.frame_rate_n > 0 && self.frame.frame_rate_d > 0 {
            self.capture_period = Duration::from_nanos(
                (1_000_000_000u64 * self.frame.frame_rate_d as u64
                    / self.frame.frame_rate_n as u64)
                    .max(1),
            );
        }
        self.next_capture = capture_time + self.capture_period;
        if self.frame.data.is_null() {
            return None;
        }
        self.held = true;
        validate_video_frame(&self.frame)?;

        let layout = match self.frame.four_cc {
            FOURCC_BGRA => PixelLayout::Bgra,
            FOURCC_BGRX => PixelLayout::Bgrx,
            FOURCC_RGBA => PixelLayout::Rgba,
            FOURCC_RGBX => PixelLayout::Rgbx,
            // Сжатые и полукадровые форматы сюда не приезжают: приёмник создан
            // с BGRX_BGRA. Всё остальное показывать нечем, кадр пропускаем.
            _ => return None,
        };

        let (stride, length) = validate_video_frame(&self.frame)?;
        let data = unsafe { std::slice::from_raw_parts(self.frame.data, length) };

        Some(FrameView {
            width: self.frame.xres as u32,
            height: self.frame.yres as u32,
            stride,
            layout,
            data,
            frame_rate_n: self.frame.frame_rate_n.max(0) as u32,
            frame_rate_d: self.frame.frame_rate_d.max(0) as u32,
            timestamp: self.frame.timestamp,
        })
    }
}

impl Drop for NdiReceiver {
    fn drop(&mut self) {
        self.release();
        unsafe {
            (self.api.framesync_destroy)(self.framesync);
            (self.api.recv_destroy)(self.receiver);
        }
    }
}

fn validate_video_frame(frame: &VideoFrame) -> Option<(usize, usize)> {
    if frame.data.is_null() || frame.xres <= 0 || frame.yres <= 0 {
        return None;
    }
    if !matches!(
        frame.four_cc,
        FOURCC_BGRA | FOURCC_BGRX | FOURCC_RGBA | FOURCC_RGBX
    ) {
        return None;
    }
    let row_bytes = usize::try_from(frame.xres).ok()?.checked_mul(4)?;
    let stride = usize::try_from(frame.line_stride_in_bytes).ok()?;
    if stride < row_bytes {
        return None;
    }
    let length = stride.checked_mul(usize::try_from(frame.yres).ok()?)?;
    if length > isize::MAX as usize {
        return None;
    }
    Some((stride, length))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn разметка_структур_совпадает_с_заголовком_sdk() {
        // Расхождение здесь портит память, а не даёт понятную ошибку.
        assert_eq!(
            std::mem::size_of::<RawSource>(),
            2 * std::mem::size_of::<usize>()
        );
        assert_eq!(std::mem::size_of::<VideoFrame>(), 72);
        assert_eq!(std::mem::align_of::<VideoFrame>(), 8);
        assert_eq!(memoffset_data(), 40, "смещение указателя на точки");
    }

    fn memoffset_data() -> usize {
        let frame = VideoFrame::default();
        let base = &frame as *const VideoFrame as usize;
        (&frame.data as *const *mut u8 as usize) - base
    }

    #[test]
    fn коды_форматов_совпадают_с_ndi() {
        assert_eq!(FOURCC_BGRA, 0x41524742);
        assert_eq!(FOURCC_RGBA, 0x41424752);
    }

    #[test]
    fn недопустимая_геометрия_не_доходит_до_from_raw_parts() {
        let mut frame = VideoFrame {
            data: std::ptr::dangling_mut(),
            four_cc: FOURCC_BGRA,
            xres: 32,
            yres: 16,
            line_stride_in_bytes: 127,
            ..VideoFrame::default()
        };
        assert!(validate_video_frame(&frame).is_none());
        frame.line_stride_in_bytes = 128;
        assert_eq!(validate_video_frame(&frame), Some((128, 2048)));
        frame.xres = -1;
        assert!(validate_video_frame(&frame).is_none());
    }
}
