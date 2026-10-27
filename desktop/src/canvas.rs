//! Приведение кадра источника к LED-строке.
//!
//! Полотно — десятки тысяч точек против двух миллионов у источника, поэтому
//! точка полотна усредняется по своей области, а не берётся ближайшей: на
//! тонких линиях выборка ближайшей точки даёт мерцающую кашу при движении.

use crate::source::{FrameView, PixelLayout};
use std::sync::OnceLock;

// NDI and video converters may leave a few code values of moving noise in a
// nominally black pixel. They are invisible in the desktop preview but the
// first HUB75 bit-plane turns them into isolated coloured flashes. Gate the
// pixel as a whole so real dark saturated colours are not shifted channel by
// channel.
const NEAR_BLACK_MAX: u8 = 12;

/// Как кадр источника ложится на полотно, если пропорции не совпали.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, serde::Serialize, serde::Deserialize)]
pub enum Fit {
    /// Растянуть на всё полотно, пропорции не сохраняются.
    Stretch,
    /// Вписать целиком, по краям чёрные поля.
    Contain,
    /// Заполнить полотно, лишнее обрезать.
    #[default]
    Cover,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, serde::Serialize, serde::Deserialize)]
pub enum Rotation {
    #[default]
    Deg0,
    Deg90,
    Deg180,
    Deg270,
}

impl Rotation {
    pub const ALL: [Self; 4] = [Self::Deg0, Self::Deg90, Self::Deg180, Self::Deg270];

    pub fn label(self) -> &'static str {
        match self {
            Self::Deg0 => "0°",
            Self::Deg90 => "90°",
            Self::Deg180 => "180°",
            Self::Deg270 => "270°",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, serde::Serialize, serde::Deserialize)]
pub struct Transform {
    pub rotation: Rotation,
    pub mirror_x: bool,
}

impl Fit {
    pub fn label(self) -> &'static str {
        match self {
            Fit::Stretch => "растянуть",
            Fit::Contain => "вписать целиком",
            Fit::Cover => "заполнить с обрезкой",
        }
    }
}

/// Полотно в RGB888: ровно то, что уходит в транспорт LEDCAST.
pub struct Canvas {
    pub width: u16,
    pub height: u16,
    pub pixels: Vec<u8>,
}

impl Canvas {
    pub fn new(width: u16, height: u16) -> Self {
        Self {
            width,
            height,
            pixels: vec![0; width as usize * height as usize * 3],
        }
    }

    fn resize(&mut self, width: u16, height: u16) {
        if self.width != width || self.height != height {
            self.width = width;
            self.height = height;
            self.pixels = vec![0; width as usize * height as usize * 3];
        }
    }

    /// Кадр источника → полотно RGB888. Ограничение разрядности и дизеринг
    /// выполняет только контроллер: прежнее повторное квантование здесь
    /// необратимо портило градиенты ещё до передачи по сети.
    pub fn draw(
        &mut self,
        frame: &FrameView,
        width: u16,
        height: u16,
        fit: Fit,
        transform: Transform,
    ) {
        self.resize(width, height);
        self.pixels.fill(0);
        if frame.width == 0 || frame.height == 0 || width == 0 || height == 0 {
            return;
        }

        let (oriented_width, oriented_height) = match transform.rotation {
            Rotation::Deg90 | Rotation::Deg270 => (frame.height, frame.width),
            _ => (frame.width, frame.height),
        };
        let (source, target) = layout(oriented_width, oriented_height, width, height, fit);
        let colour = colour_tables();
        let (offset_r, offset_g, offset_b, offset_a, stride) = frame.layout.offsets();

        for row in 0..target.height {
            let y = target.y + row;
            // Область источника, приходящаяся на строку полотна.
            let (top, bottom) = span(source.y, source.height, row, target.height);
            for column in 0..target.width {
                let x = target.x + column;
                let (left, right) = span(source.x, source.width, column, target.width);

                // Больше восьми выборок по оси картинку уже не меняют, а на
                // 4K-источнике стоят лишних десятков миллисекунд на кадр.
                let step_x = ((right - left) / 8).max(1);
                let step_y = ((bottom - top) / 8).max(1);

                let (mut sum_r, mut sum_g, mut sum_b, mut count) = (0u32, 0u32, 0u32, 0u32);
                let mut sample_y = top;
                while sample_y < bottom {
                    let mut sample_x = left;
                    while sample_x < right {
                        let (source_x, source_y) = transformed_source_point(
                            sample_x,
                            sample_y,
                            frame.width,
                            frame.height,
                            transform,
                        );
                        let at = source_y as usize * frame.stride + source_x as usize * stride;
                        // NDI BGRA/RGBA carries straight alpha. Resolume's
                        // checkerboard is only a monitor aid; a physical LED
                        // surface has a black background. Composite in linear
                        // light so transparent edge colours cannot leak into
                        // the LED frame or its preview.
                        let alpha = offset_a.map_or(255, |offset| frame.data[at + offset]) as u32;
                        sum_r +=
                            colour.decode[frame.data[at + offset_r] as usize] as u32 * alpha / 255;
                        sum_g +=
                            colour.decode[frame.data[at + offset_g] as usize] as u32 * alpha / 255;
                        sum_b +=
                            colour.decode[frame.data[at + offset_b] as usize] as u32 * alpha / 255;
                        count += 1;
                        sample_x += step_x;
                    }
                    sample_y += step_y;
                }
                if count == 0 {
                    continue;
                }

                let at = (y as usize * width as usize + x as usize) * 3;
                let r = colour.encode[(sum_r / count) as usize];
                let g = colour.encode[(sum_g / count) as usize];
                let b = colour.encode[(sum_b / count) as usize];
                if r.max(g).max(b) <= NEAR_BLACK_MAX {
                    self.pixels[at..at + 3].fill(0);
                } else {
                    self.pixels[at] = r;
                    self.pixels[at + 1] = g;
                    self.pixels[at + 2] = b;
                }
            }
        }
    }
}

fn transformed_source_point(
    mut x: u32,
    y: u32,
    width: u32,
    height: u32,
    transform: Transform,
) -> (u32, u32) {
    let oriented_width = match transform.rotation {
        Rotation::Deg90 | Rotation::Deg270 => height,
        _ => width,
    };
    if transform.mirror_x {
        x = oriented_width - 1 - x;
    }
    match transform.rotation {
        Rotation::Deg0 => (x, y),
        Rotation::Deg90 => (y, height - 1 - x),
        Rotation::Deg180 => (width - 1 - x, height - 1 - y),
        Rotation::Deg270 => (width - 1 - y, x),
    }
}

/// Прямоугольник — либо кусок источника, либо место на полотне.
struct Area {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

/// Какой кусок источника ложится и в какое место полотна.
///
/// Обрезка меняет источник, а вписывание — место на полотне: поля берутся
/// оттуда, что точка полотна за пределами картинки просто остаётся чёрной.
fn layout(src_w: u32, src_h: u32, dst_w: u16, dst_h: u16, fit: Fit) -> (Area, Area) {
    let source = Area {
        x: 0,
        y: 0,
        width: src_w,
        height: src_h,
    };
    let target = Area {
        x: 0,
        y: 0,
        width: dst_w as u32,
        height: dst_h as u32,
    };

    let src_aspect = src_w as f32 / src_h as f32;
    let dst_aspect = dst_w as f32 / dst_h as f32;
    let wider = src_aspect > dst_aspect;

    match (fit, wider) {
        (Fit::Stretch, _) => (source, target),
        (Fit::Cover, true) => {
            let width = ((src_h as f32 * dst_aspect) as u32).clamp(1, src_w);
            (
                Area {
                    x: (src_w - width) / 2,
                    y: 0,
                    width,
                    height: src_h,
                },
                target,
            )
        }
        (Fit::Cover, false) => {
            let height = ((src_w as f32 / dst_aspect) as u32).clamp(1, src_h);
            (
                Area {
                    x: 0,
                    y: (src_h - height) / 2,
                    width: src_w,
                    height,
                },
                target,
            )
        }
        (Fit::Contain, true) => {
            let height = ((dst_w as f32 / src_aspect) as u32).clamp(1, dst_h as u32);
            (
                source,
                Area {
                    x: 0,
                    y: (dst_h as u32 - height) / 2,
                    width: dst_w as u32,
                    height,
                },
            )
        }
        (Fit::Contain, false) => {
            let width = ((dst_h as f32 * src_aspect) as u32).clamp(1, dst_w as u32);
            (
                source,
                Area {
                    x: (dst_w as u32 - width) / 2,
                    y: 0,
                    width,
                    height: dst_h as u32,
                },
            )
        }
    }
}

/// Поля при вписывании: точка полотна вне картинки остаётся чёрной.
fn span(origin: u32, extent: u32, index: u32, total: u32) -> (u32, u32) {
    let from = origin + extent * index / total;
    let to = origin + extent * (index + 1) / total;
    (from, to.max(from + 1))
}

struct ColourTables {
    decode: [u16; 256],
    encode: Box<[u8; 65536]>,
}

fn colour_tables() -> &'static ColourTables {
    static TABLES: OnceLock<ColourTables> = OnceLock::new();
    TABLES.get_or_init(|| {
        let mut decode = [0u16; 256];
        for (value, slot) in decode.iter_mut().enumerate() {
            let encoded = value as f64 / 255.0;
            let linear = if encoded <= 0.04045 {
                encoded / 12.92
            } else {
                ((encoded + 0.055) / 1.055).powf(2.4)
            };
            *slot = (linear * 65535.0).round() as u16;
        }

        let mut encode = Box::new([0u8; 65536]);
        for (value, slot) in encode.iter_mut().enumerate() {
            let linear = value as f64 / 65535.0;
            let encoded = if linear <= 0.0031308 {
                linear * 12.92
            } else {
                1.055 * linear.powf(1.0 / 2.4) - 0.055
            };
            *slot = (encoded * 255.0).round().clamp(0.0, 255.0) as u8;
        }
        ColourTables { decode, encode }
    })
}

#[cfg(test)]
fn rgb_at(canvas: &Canvas, index: usize) -> &[u8] {
    &canvas.pixels[index * 3..index * 3 + 3]
}

#[cfg(test)]
fn assert_rgb_near(actual: &[u8], expected: [u8; 3]) {
    for (actual, expected) in actual.iter().zip(expected) {
        assert!(
            actual.abs_diff(expected) <= 1,
            "{actual} is not near {expected}"
        );
    }
}

impl PixelLayout {
    /// Смещения красного, зелёного и синего внутри точки и шаг между точками.
    fn offsets(self) -> (usize, usize, usize, Option<usize>, usize) {
        match self {
            PixelLayout::Bgra => (2, 1, 0, Some(3), 4),
            PixelLayout::Rgba => (0, 1, 2, Some(3), 4),
            PixelLayout::Bgrx => (2, 1, 0, None, 4),
            PixelLayout::Rgbx => (0, 1, 2, None, 4),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn solid(width: u32, height: u32, rgb: [u8; 3]) -> Vec<u8> {
        let mut data = vec![0u8; width as usize * height as usize * 4];
        for point in data.chunks_exact_mut(4) {
            point[0] = rgb[0];
            point[1] = rgb[1];
            point[2] = rgb[2];
            point[3] = 255;
        }
        data
    }

    fn view(data: &[u8], width: u32, height: u32) -> FrameView<'_> {
        FrameView {
            width,
            height,
            stride: width as usize * 4,
            layout: PixelLayout::Rgba,
            data,
            frame_rate_n: 60,
            frame_rate_d: 1,
            timestamp: 0,
        }
    }

    #[test]
    fn заливка_доезжает_до_каждой_точки_полотна() {
        let data = solid(64, 32, [10, 200, 30]);
        let frame = view(&data, 64, 32);
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(&frame, 192, 64, Fit::Stretch, Transform::default());
        assert_eq!(canvas.pixels.len(), 192 * 64 * 3);
        assert!(canvas.pixels.chunks_exact(3).all(|p| p == [10, 200, 30]));
    }

    #[test]
    fn шум_в_номинально_чёрном_пикселе_подавляется() {
        let shadow = solid(1, 1, [1, 3, 8]);
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(
            &view(&shadow, 1, 1),
            1,
            1,
            Fit::Stretch,
            Transform::default(),
        );
        assert_eq!(canvas.pixels, [0, 0, 0]);
    }

    #[test]
    fn тёмный_насыщенный_цвет_не_становится_чёрным() {
        let shadow = solid(1, 1, [2, 4, 13]);
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(
            &view(&shadow, 1, 1),
            1,
            1,
            Fit::Stretch,
            Transform::default(),
        );
        assert_eq!(canvas.pixels, [2, 4, 13]);
    }

    #[test]
    fn вписывание_оставляет_поля_с_той_стороны_где_картинка_уже() {
        // Источник 16:9 на полотне 3:1 — картинка уже полотна по ширине,
        // поэтому чёрные поля выходят слева и справа, а не сверху и снизу.
        let data = solid(1920, 1080, [255, 255, 255]);
        let frame = view(&data, 1920, 1080);
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(&frame, 192, 64, Fit::Contain, Transform::default());
        let point = |x: usize, y: usize| canvas.pixels[(y * 192 + x) * 3];
        assert_eq!(point(0, 32), 0, "левый край должен быть чёрным");
        assert_eq!(point(191, 32), 0, "правый край должен быть чёрным");
        assert_eq!(point(96, 32), 255, "середина должна быть залита");
        assert_eq!(
            point(96, 0),
            255,
            "по высоте картинка занимает полотно целиком"
        );
    }

    #[test]
    fn вписывание_высокого_кадра_оставляет_поля_сверху_и_снизу() {
        // Источник 4:1 шире полотна 2:1 — поля выходят сверху и снизу.
        let data = solid(800, 200, [255, 255, 255]);
        let frame = view(&data, 800, 200);
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(&frame, 128, 64, Fit::Contain, Transform::default());
        let point = |x: usize, y: usize| canvas.pixels[(y * 128 + x) * 3];
        assert_eq!(point(64, 0), 0, "верхний край должен быть чёрным");
        assert_eq!(point(64, 32), 255, "середина должна быть залита");
    }

    #[test]
    fn заполнение_с_обрезкой_не_оставляет_полей() {
        let data = solid(1920, 1080, [255, 255, 255]);
        let frame = view(&data, 1920, 1080);
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(&frame, 192, 64, Fit::Cover, Transform::default());
        assert!(
            canvas.pixels.iter().all(|&v| v == 255),
            "поля при обрезке недопустимы"
        );
    }

    #[test]
    fn точный_размер_не_меняет_rgb888() {
        let data = vec![130, 77, 3, 255, 4, 210, 99, 255];
        let frame = view(&data, 2, 1);
        let mut canvas = Canvas::new(2, 1);
        canvas.draw(&frame, 2, 1, Fit::Stretch, Transform::default());
        assert_rgb_near(rgb_at(&canvas, 0), [130, 77, 3]);
        assert_rgb_near(rgb_at(&canvas, 1), [4, 210, 99]);
    }

    #[test]
    fn альфа_ndi_композится_на_чёрный_фон() {
        let data = vec![255, 80, 40, 0, 255, 0, 0, 128];
        let frame = view(&data, 2, 1);
        let mut canvas = Canvas::new(2, 1);
        canvas.draw(&frame, 2, 1, Fit::Stretch, Transform::default());
        assert_eq!(rgb_at(&canvas, 0), [0, 0, 0]);
        assert_rgb_near(rgb_at(&canvas, 1), [188, 0, 0]);
    }

    #[test]
    fn служебный_x_байт_не_считается_альфой() {
        let data = vec![255, 255, 0, 0];
        let mut frame = view(&data, 1, 1);
        frame.layout = PixelLayout::Rgbx;
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(&frame, 1, 1, Fit::Stretch, Transform::default());
        assert_eq!(rgb_at(&canvas, 0), [255, 255, 0]);
    }

    #[test]
    fn уменьшение_усредняет_свет_а_не_кодовые_значения_srgb() {
        let data = vec![0, 0, 0, 255, 255, 255, 255, 255];
        let frame = view(&data, 2, 1);
        let mut canvas = Canvas::new(1, 1);
        canvas.draw(&frame, 1, 1, Fit::Stretch, Transform::default());
        assert_rgb_near(rgb_at(&canvas, 0), [188, 188, 188]);
    }

    #[test]
    fn повороты_и_зеркало_дают_ожидаемые_координаты() {
        let point = |rotation, mirror_x, x, y| {
            transformed_source_point(x, y, 3, 2, Transform { rotation, mirror_x })
        };
        assert_eq!(point(Rotation::Deg0, false, 0, 0), (0, 0));
        assert_eq!(point(Rotation::Deg0, true, 0, 0), (2, 0));
        assert_eq!(point(Rotation::Deg90, false, 0, 0), (0, 1));
        assert_eq!(point(Rotation::Deg180, false, 0, 0), (2, 1));
        assert_eq!(point(Rotation::Deg270, false, 0, 0), (2, 0));
    }
}
