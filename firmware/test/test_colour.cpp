#include "colour.h"

#include <set>

#include "check.h"

namespace {

using namespace ledcast::colour;

// Вход видеопотока — sRGB/Rec.709-подобный видеосигнал от NDI или захвата экрана.
// Панель ждёт линейную ШИМ-яркость, поэтому таблица обязана быть именно
// обратной sRGB-кривой, а не CIE L*: у них разные тени.
void gamma_is_srgb() {
    CHECK(kGamma16[0] == 0);
    CHECK(kGamma16[1] == 20);
    CHECK(kGamma16[10] == 199);
    CHECK(kGamma16[11] == 219);
    CHECK(kGamma16[16] == 340);
    CHECK(kGamma16[32] == 947);
    CHECK(kGamma16[50] == 2090);
    CHECK(kGamma16[128] == 14146);
    CHECK(kGamma16[255] == 65535);
    for (int v = 0; v < 255; ++v) {
        CHECK(kGamma16[v] <= kGamma16[v + 1]);
    }
}

// Матрица Bayer — перестановка 0..15, иначе смещения порога не покроют шаг
// квантования равномерно и на плоской заливке появится сетка.
void bayer_is_permutation() {
    std::set<int> seen;
    for (int i = 0; i < 16; ++i) {
        const int m = (kBayerBias[i] - 1) / 2;
        CHECK(kBayerBias[i] == 2 * m + 1);
        seen.insert(m);
    }
    CHECK(seen.size() == 16u);
    CHECK(*seen.begin() == 0);
    CHECK(*seen.rbegin() == 15);
}

// Квантование монотонно по входу при любом фиксированном смещении порога.
void quantize_monotonic() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        for (int i = 0; i < 16; ++i) {
            const uint16_t bias = bayer_bias(q, kBayerBias[i]);
            uint16_t prev = 0;
            for (int v = 0; v < 256; ++v) {
                const uint16_t out = map8(static_cast<uint8_t>(v), bias, q);
                CHECK(out >= prev);
                CHECK(out <= q.max);
                prev = out;
            }
        }
    }
}

// Конвейер выходит в оба края шкалы при любой разрядности: чёрное остаётся
// чёрным, белое — полной яркостью.
void endpoints_reach_full_scale() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        for (int i = 0; i < 16; ++i) {
            const uint16_t bias = bayer_bias(q, kBayerBias[i]);
            CHECK(map8(0, bias, q) == 0);
            CHECK(map8(255, bias, q) == q.max);
        }
    }
}

// Число различимых уровней. До семи бит включительно дизеринг вытягивает ровно
// 2^d уровней. На восьми битах обратная sRGB-кривая оставляет недостижимыми три
// значения ШИМ, но сохраняет больше уровней, чем прежняя CIE-кривая.
void distinct_levels() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        std::set<uint16_t> dithered;
        std::set<uint16_t> plain;
        for (int v = 0; v < 256; ++v) {
            for (int i = 0; i < 16; ++i) {
                dithered.insert(map8(static_cast<uint8_t>(v), bayer_bias(q, kBayerBias[i]), q));
            }
            plain.insert(map8(static_cast<uint8_t>(v), round_bias(q), q));
        }
        const size_t full = static_cast<size_t>(1) << depth;
        if (depth <= 7) {
            CHECK(dithered.size() == full);
        } else {
            CHECK(dithered.size() == 253u);
        }
        // Дизеринг не может уменьшить число уровней — только добавить.
        CHECK(dithered.size() >= plain.size());
    }
}

// Плоский участок: одинаковый вход по всей плитке 4x4 не должен ломать
// монотонность соседних входов. Средняя яркость плитки растёт вместе с входом.
void bayer_keeps_flat_ramp_monotonic() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        uint32_t prev_sum = 0;
        for (int v = 0; v < 256; ++v) {
            uint32_t sum = 0;
            for (int i = 0; i < 16; ++i) {
                sum += map8(static_cast<uint8_t>(v), bayer_bias(q, kBayerBias[i]), q);
            }
            CHECK(sum >= prev_sum);
            prev_sum = sum;
        }
    }
}

// Плитка на плоской заливке даёт не более двух соседних уровней: смещение
// порога всегда меньше одного шага квантования.
void bayer_spans_one_step() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        for (int v = 0; v < 256; ++v) {
            uint16_t lo = q.max;
            uint16_t hi = 0;
            for (int i = 0; i < 16; ++i) {
                const uint16_t out = map8(static_cast<uint8_t>(v), bayer_bias(q, kBayerBias[i]), q);
                lo = out < lo ? out : lo;
                hi = out > hi ? out : hi;
            }
            CHECK(hi - lo <= 1);
        }
    }
}

// Индекс плитки берётся масками, без остатка от деления, и повторяется с
// периодом 4 по обеим осям.
void bayer_index_tiles_by_four() {
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK(bayer_index(static_cast<uint16_t>(x), static_cast<uint16_t>(y)) ==
                  bayer_index(static_cast<uint16_t>(x + 4), static_cast<uint16_t>(y)));
            CHECK(bayer_index(static_cast<uint16_t>(x), static_cast<uint16_t>(y)) ==
                  bayer_index(static_cast<uint16_t>(x), static_cast<uint16_t>(y + 4)));
        }
    }
    std::set<uint8_t> seen;
    for (uint16_t y = 0; y < 4; ++y) {
        for (uint16_t x = 0; x < 4; ++x) {
            seen.insert(bayer_index(x, y));
        }
    }
    CHECK(seen.size() == 16u);
}

void eight_bit_output_has_no_spatial_dot_pattern() {
    const Quantizer q8 = quantizer_for(8);
    for (uint16_t y = 0; y < 8; ++y) {
        for (uint16_t x = 0; x < 8; ++x) {
            CHECK_EQ(quantization_bias(q8, 8, x, y), round_bias(q8));
        }
    }
    // На урезанной глубине порог по-прежнему распределён по площади.
    const Quantizer q6 = quantizer_for(6);
    std::set<uint16_t> biases;
    for (uint16_t y = 0; y < 4; ++y) {
        for (uint16_t x = 0; x < 4; ++x) {
            biases.insert(quantization_bias(q6, 6, x, y));
        }
    }
    CHECK_EQ(biases.size(), 16u);
}

void channels_never_leak_into_each_other() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        const uint16_t bias = quantization_bias(q, depth, 1, 2);
        const uint16_t red = map8(255, bias, q);
        const uint16_t zero = map8(0, bias, q);
        const uint32_t word = plane_word(red, zero, zero);
        for (uint8_t plane = 0; plane < depth; ++plane) {
            CHECK(((word >> (3u * plane)) & 0x06u) == 0u);
        }
    }
}

// Разрядность разбирается один раз до цикла: Quantizer полностью описывает
// сдвиги и потолок, и внутри цикла по точкам про глубину ничего не известно.
void quantizer_precomputes_depth() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        CHECK(q.shift == 16 - depth);
        CHECK(q.max == (1u << depth) - 1u);
        CHECK(q.bias_shift == q.shift - 5);
        CHECK(round_bias(q) == (1u << (q.shift - 1)));
    }
}

// Полный перебор 256 входов на семи разрядностях против эталонной формулы.
void matches_reference_for_all_inputs() {
    for (uint8_t depth = kMinDepth; depth <= kMaxDepth; ++depth) {
        const Quantizer q = quantizer_for(depth);
        const uint32_t shift = 16u - depth;
        const uint32_t maxv = (1u << depth) - 1u;
        for (int v = 0; v < 256; ++v) {
            for (int i = 0; i < 16; ++i) {
                const uint32_t bias = (static_cast<uint32_t>(kBayerBias[i]) << shift) / 32u;
                uint32_t want = (static_cast<uint32_t>(kGamma16[v]) + bias) >> shift;
                if (want > maxv) {
                    want = maxv;
                }
                CHECK(map8(static_cast<uint8_t>(v), bayer_bias(q, kBayerBias[i]), q) == want);
            }
        }
    }
}

// Раскладка по битпланам таблицей обязана совпасть с побитовым разбором: бит d
// значения шкалы панели лежит в бите 3d, и тройка каналов вынимается из слова
// одним сдвигом.
void spread_matches_bit_by_bit() {
    for (int v = 0; v <= 255; ++v) {
        const uint32_t s = plane_spread(static_cast<uint16_t>(v));
        for (int d = 0; d < 8; ++d) {
            CHECK(((s >> (3 * d)) & 1u) == static_cast<uint32_t>((v >> d) & 1));
        }
        // Между битами значения — только нули: иначе каналы наложились бы.
        CHECK((s & ~0x00249249u) == 0u);
    }
}

// Слово прогона: тройка каналов на плане d вынимается как (word >> 3d) & 7 и
// совпадает с прежней сборкой бит за битом.
void plane_word_matches_reference() {
    for (int r = 0; r <= 255; r += 7) {
        for (int g = 0; g <= 255; g += 11) {
            for (int b = 0; b <= 255; b += 13) {
                const uint32_t word = plane_word(static_cast<uint16_t>(r), static_cast<uint16_t>(g),
                                                 static_cast<uint16_t>(b));
                for (int d = 0; d < 8; ++d) {
                    const uint32_t want = static_cast<uint32_t>(((b >> d) & 1) << 2) |
                                          static_cast<uint32_t>(((g >> d) & 1) << 1) |
                                          static_cast<uint32_t>((r >> d) & 1);
                    CHECK(((word >> (3 * d)) & 7u) == want);
                }
            }
        }
    }
}

}  // namespace

int main() {
    gamma_is_srgb();
    bayer_is_permutation();
    quantize_monotonic();
    endpoints_reach_full_scale();
    distinct_levels();
    bayer_keeps_flat_ramp_monotonic();
    bayer_spans_one_step();
    bayer_index_tiles_by_four();
    eight_bit_output_has_no_spatial_dot_pattern();
    channels_never_leak_into_each_other();
    quantizer_precomputes_depth();
    matches_reference_for_all_inputs();
    spread_matches_bit_by_bit();
    plane_word_matches_reference();
    return hosttest::report("colour");
}
