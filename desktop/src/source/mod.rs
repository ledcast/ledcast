//! Источники картинки: то, что приложение получает и кладёт на полотно.

pub mod ndi;

use std::time::Duration;

/// Порядок каналов в точке источника.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PixelLayout {
    /// Порядок, в котором NDI отдаёт кадр с настоящей альфой.
    Bgra,
    Rgba,
    /// X — служебный байт с неопределённым значением, а не прозрачность.
    Bgrx,
    Rgbx,
}

/// Кадр источника без копии: NDI отдаёт указатель в свою память, и гонять
/// восемь мегабайт через промежуточный буфер на каждом кадре незачем. Живёт до
/// следующего запроса кадра у того же источника.
pub struct FrameView<'a> {
    pub width: u32,
    pub height: u32,
    /// Длина строки в байтах: у NDI она бывает больше ширины на выравнивание.
    pub stride: usize,
    pub layout: PixelLayout,
    pub data: &'a [u8],
    /// Nominal cadence declared by the source. NDI uses a rational value
    /// (for example 30000/1001), so keep it lossless until the wire boundary.
    pub frame_rate_n: u32,
    pub frame_rate_d: u32,
    pub timestamp: i64,
}

impl FrameView<'_> {
    pub fn nominal_fps(&self) -> f64 {
        if self.frame_rate_n == 0 || self.frame_rate_d == 0 {
            60.0
        } else {
            self.frame_rate_n as f64 / self.frame_rate_d as f64
        }
    }
}

pub trait VideoSource: Send {
    /// Что показывать в интерфейсе как имя источника.
    fn title(&self) -> String;

    /// Следующий кадр. None означает, что за отведённое время кадра не было:
    /// источник жив, но молчит — это не ошибка.
    fn next_frame(&mut self, timeout: Duration) -> Option<FrameView<'_>>;
}
