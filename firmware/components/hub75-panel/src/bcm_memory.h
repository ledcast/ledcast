#pragma once
#include <cstdint>
#include "bcm_timing.h"

namespace hub75_bcm {
inline constexpr uint32_t kDescriptorBytes = 12; // ESP32-S3 dma_descriptor_t
inline constexpr uint32_t kMaxPayloadBytes = 4092;
// Тёмная добивка прохода — потеря яркости. Больше восьмой части прохода на
// подгонку не тратится, пока есть вариант дешевле.
inline constexpr uint32_t kMaxPadShare = 8;
// Развёртка не реже этого: 60 Гц на светодиодах видно как мерцание. При 60 и
// 120 кадрах/с её и так дают два прохода на кадр, при 30 нужно четыре.
inline constexpr uint32_t kMinRefreshHz = 120;
constexpr uint32_t align4(uint32_t bytes) { return (bytes + 3u) & ~3u; }

// Развёртка, подогнанная под частоту кадров.
//
// Кадр меняется только на границе прохода кольца DMA. Если проход не делит
// период кадра нацело, кадры встают то через k, то через k+1 проходов, и
// видео 60 кадров/с на сетке 137 Гц держит кадры неровно: 2-2-3. Поэтому
// проход растягивается тёмной добивкой (OE погашен, адрес не меняется) ровно
// до 1/(k·frame_rate) секунды: кадр встаёт на каждый k-й проход, сетка кадров
// совпадает с frame_rate точно.
struct DmaMemory {
    uint32_t framebuffer_bytes = 0;
    uint32_t descriptors_per_buffer = 0;  // вместе с дескрипторами добивки
    uint32_t pad_buffer_bytes = 0;        // один тёмный буфер на оба кольца
    uint32_t pad_clocks = 0;              // тактов добивки в проходе
    uint32_t pass_clocks = 0;             // тактов в проходе вместе с добивкой
    uint32_t total_bytes = 0;
    uint32_t refresh_hz = 0;              // проходов в секунду
    uint8_t passes_per_frame = 1;
    uint8_t transition = 0;
};

constexpr uint32_t bcm_planes(uint8_t depth, uint8_t transition) {
    uint32_t planes = depth;
    for (uint8_t plane = transition + 1; plane < depth; ++plane)
        planes += (1u << (plane - transition)) - 1u;
    return planes;
}

// One calculation shared by configuration admission and actual allocation.
// Use the same integer clock arithmetic and span traversal as scanout.
inline DmaMemory dma_memory(uint16_t width, uint8_t rows, uint8_t depth,
                            uint8_t buffers, uint32_t bus_hz, uint16_t frame_rate,
                            bool interleaved) {
    DmaMemory out{};
    if (!width || !rows || depth < 2 || depth > 8 || !buffers || !bus_hz || !frame_rate) return out;
    const uint64_t min_passes = (kMinRefreshHz + frame_rate - 1u) / frame_rate > 2u
                                    ? (kMinRefreshHz + frame_rate - 1u) / frame_rate
                                    : 2u;

    // Подбор точки перелома BCM. Проходов на кадр не меньше двух — развёртка
    // вдвое чаще кадров не мерцает, — а добивка не больше 1/kMaxPadShare.
    // Чем ниже точка перелома, тем больше разрядность младших планов, поэтому
    // берётся первая подходящая. Не подошла ни одна — та, что теряет меньше
    // яркости. Не успевает и одна — самая быстрая, без добивки: кадры идут
    // на каждом проходе, реже frame_rate.
    struct Choice { uint8_t transition; uint32_t passes; uint32_t pass; uint32_t pad; };
    Choice best{static_cast<uint8_t>(depth - 1), 1, 0, 0};
    bool chosen = false, have_fallback = false;
    for (uint8_t transition = 0; transition < depth && !chosen; ++transition) {
        const uint64_t natural = uint64_t(width) * bcm_planes(depth, transition) * rows;
        // Проходов на кадр не больше 255: на совсем короткой цепочке проход
        // длится микросекунды, и такой кадр растягивается добивкой.
        const uint64_t fit = bus_hz / (uint64_t(frame_rate) * natural);
        const uint64_t passes = fit > 255u ? 255u : fit;
        if (passes == 0) continue;
        const uint64_t per_second = passes * frame_rate;
        const uint64_t pass = (bus_hz + per_second / 2u) / per_second;
        // Добивка кратна двум словам: длина дескриптора кратна четырём байтам.
        const uint64_t pad = pass > natural ? ((pass - natural + 1u) & ~uint64_t(1)) : 0u;
        const Choice candidate{transition, uint32_t(passes), uint32_t(natural + pad), uint32_t(pad)};
        if (passes >= min_passes && pad * kMaxPadShare <= candidate.pass) {
            best = candidate;
            chosen = true;
        } else if (!have_fallback ||
                   uint64_t(pad) * best.pass < uint64_t(best.pad) * candidate.pass) {
            best = candidate;
            have_fallback = true;
        }
    }
    if (!chosen && !have_fallback)
        best.pass = uint32_t(uint64_t(width) * bcm_planes(depth, best.transition) * rows);

    out.transition = best.transition;
    out.passes_per_frame = uint8_t(best.passes);
    out.pass_clocks = best.pass;
    out.pad_clocks = best.pad;
    out.refresh_hz = best.pass ? (bus_hz + best.pass / 2u) / best.pass : 0;

    const uint32_t plane_bytes = uint32_t(width) * 2u;
    out.framebuffer_bytes = align4(plane_bytes * depth) * rows * buffers;
    for_each_span(rows, depth, out.transition, interleaved,
        [&](uint8_t, uint8_t, uint8_t planes) {
            out.descriptors_per_buffer += (plane_bytes * planes + kMaxPayloadBytes - 1u) / kMaxPayloadBytes;
        });
    const uint32_t pad_bytes = out.pad_clocks * 2u;
    out.descriptors_per_buffer += (pad_bytes + kMaxPayloadBytes - 1u) / kMaxPayloadBytes;
    out.pad_buffer_bytes = pad_bytes < kMaxPayloadBytes ? pad_bytes : kMaxPayloadBytes;
    out.total_bytes = out.framebuffer_bytes + align4(out.descriptors_per_buffer * kDescriptorBytes) * buffers +
                      out.pad_buffer_bytes;
    return out;
}
} // namespace hub75_bcm
