// Привязка display к библиотеке mrcodetastic/ESP32-HUB75-MatrixPanel-DMA.
//
// Библиотека работает с «электрической» геометрией панели, а не с физической.
// У модуля со сканом 1/4 драйвер сдвигового регистра тянет одну строку данных
// длиной в две видимые строки, поэтому в HUB75_I2S_CFG уходит модуль вдвое
// шире и вдвое ниже реального: наш P10 32x16 библиотека видит как 64x8.
// Обратный пересчёт координат — таблицей, построенной на старте вызовами
// ScanTypeMapping из ESP32-HUB75-VirtualMatrixPanel_T.hpp.

#include "hub75_backend.h"

#include <atomic>
#include <cstring>
#include <new>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_private/gdma.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ESP32-HUB75-VirtualMatrixPanel_T.hpp>
#include <bcm_timing.h>

#include "colour.h"
#include "dmapool.h"

#include "hd_wf2_pins.h"

namespace ledcast::hub75_backend {
namespace {

constexpr char kTag[] = "display.hub75";

// Библиотека отдаёт координату в int16_t, дальше начнётся отрицательный x.
constexpr uint32_t kMaxDmaChainWidth = 32767;

// Ниже двух бит библиотека не опускается, да и смысла нет: два бита это уже
// четыре уровня на канал.

// Меньше одного такта гашения нельзя: библиотека отсчитывает такты циклом
// do-while по uint8_t и на нуле уходит в 255, то есть пишет биты OE далеко за
// границей строки.
constexpr uint8_t kMinLatchBlanking = 1;

bool valid_bus(uint32_t hz) {
    for (const uint32_t candidate : kBusFrequencies)
        if (candidate == hz) return true;
    return false;
}

struct Plan {
    PANEL_SCAN_TYPE scan = STANDARD_TWO_SCAN;
    uint16_t mx_width = 0;        // ширина модуля глазами библиотеки
    uint16_t mx_height = 0;       // высота модуля глазами библиотеки
    uint16_t num_rows = 0;        // адресных строк в кадре
    uint16_t chain_width = 0;     // ширина цепочки в координатах вызывающей стороны
    uint16_t dma_chain_width = 0; // она же в электрических пикселях
    uint16_t module_gain = 0;     // прибавка к x за каждый пройденный модуль
    uint16_t block = 0;           // шаг блока для раскладок P10; 0 — библиотечная раскладка
    bool block_reverse = false;   // порядок точек внутри блока обратный
    ScanLayout mapper = ScanLayout::Auto;  // раскладка из rpi-rgb-led-matrix
};

MatrixPanel_I2S_DMA* g_panel = nullptr;

// Пересчёт строки цепочки в строку библиотеки и сдвиг x, общий для всей строки.
uint16_t g_row_y[kMaxModuleHeight];
uint16_t g_row_x_bias[kMaxModuleHeight];

uint16_t g_module_gain = 0;
// Обратная величина ширины модуля: деление x на неё в горячем пути заменено
// умножением со сдвигом. Точно для любого x < 65536.
uint32_t g_width_recip = 0;

uint8_t g_brightness = 0;

// Держит ли драйвер две копии кадра. Читается на каждом закрытии кадра, поэтому
// лежит рядом с указателем на панель, а не достаётся из её конфигурации.
bool g_double_buffer = false;

// Таблица построчного вывода: для каждой строки модуля — координата полотна
// первого модуля цепочки, отдельно строка полотна и шаг за один модуль.
// Раскладка внутри модуля от номера модуля не зависит ни в одной из веток
// пересчёта, поэтому таблицы хватает на модуль, а не на всю цепочку: 32×16
// точек это килобайт против сорока четырёх.
uint16_t* g_run_x = nullptr;
uint16_t g_row_dma_y[kMaxModuleHeight];
uint16_t g_run_width = 0;    // ширина модуля в точках
uint16_t g_run_height = 0;   // высота модуля в точках
uint16_t g_run_stride = 0;   // прибавка к координате полотна за один модуль
uint16_t g_chain_width = 0;  // ширина всей цепочки в точках
uint16_t g_run_modules = 0;  // число модулей в цепочке; g_chain_width / g_run_width

// Куда уходит каждый из трёх каналов: индекс — номер канала на входе,
// значение — линия панели. Меняется на лету, поэтому atomic.
std::atomic<uint32_t> g_lane_map{0x00020100u};  // по умолчанию BGR

// Готовое sRGB -> 8 битпланов. Видео меняет цвет почти на каждой точке,
// поэтому кэш «прошлый RGB» не помогает. Эта таблица сливает гамму,
// округление и раскладку по планам в одну выборку из внутренней RAM.
DRAM_ATTR uint32_t g_srgb_plane[256];

void build_srgb_plane_table() {
    const auto q = ledcast::colour::quantizer_for(8);
    const uint16_t bias = ledcast::colour::round_bias(q);
    for (uint16_t value = 0; value < 256; ++value) {
        const uint16_t level = ledcast::colour::map8(static_cast<uint8_t>(value), bias, q);
        g_srgb_plane[value] = ledcast::colour::plane_spread(level);
    }
}

inline uint16_t module_index(uint16_t x) {
    return static_cast<uint16_t>((static_cast<uint64_t>(x) * g_width_recip) >> 32);
}

uint16_t g_block = 0;   // шаг блока в точках
bool g_block_reverse = false;

// Раскладки, перенесённые из rpi-rgb-led-matrix. Каждая раскладывает модуль в
// своё «электрическое» полотно, поэтому вместе с формулой хранится и множитель
// ширины: у P10 с четвертным сканом полотно вчетверо шире модуля.
ScanLayout g_mapper = ScanLayout::Auto;
uint16_t g_map_width = 0;   // ширина модуля в точках
uint16_t g_map_height = 0;  // высота модуля в точках
uint16_t g_stretch_width = 0;  // ширина модуля в координатах полотна

struct MapperTraits {
    uint8_t stretch;  // во сколько раз полотно шире модуля
    uint8_t rows;     // адресных строк в кадре
};

MapperTraits mapper_traits(ScanLayout layout, uint16_t height) {
    switch (layout) {
        case ScanLayout::P10Quarter:
        case ScanLayout::Kaler2Scan: return {4, 4};
        case ScanLayout::Stripe:
        case ScanLayout::FlippedStripe:
        case ScanLayout::Checkered:
        case ScanLayout::Spiral:
        case ScanLayout::ZStripe:
        case ScanLayout::ZStripeUneven:
        case ScanLayout::Coreman:
            return {2, static_cast<uint8_t>(height / 4u)};
        default: return {0, 0};
    }
}

// Формулы один в один из lib/multiplex-mappers.cc, только на наших типах.
inline void map_hzeller(ScanLayout layout, uint16_t x, uint16_t y, uint16_t& out_x,
                        uint16_t& out_y) {
    const int cols = g_map_width;
    const int rows = g_map_height;
    const int ix = static_cast<int>(x);
    const int iy = static_cast<int>(y);
    int mx = ix;
    int my = iy;

    switch (layout) {
        case ScanLayout::P10Quarter: {
            mx = (ix / 8) * 32 + (ix % 8) + (3 - (iy / 4)) * 8;
            my = iy % 4;
            break;
        }
        case ScanLayout::Kaler2Scan: {
            const int offset = ((iy % 4) / 2) == 0 ? -1 : 1;
            const int delta_offset = offset < 0 ? 7 : 8;
            const int delta_column = ((iy % 8) / 4) == 0 ? 64 : 0;
            my = iy % 2 + (iy / 8) * 2;
            mx = delta_column + (16 * (ix / 8)) + delta_offset + ((ix % 8) * offset);
            break;
        }
        case ScanLayout::Stripe: {
            const bool top = (iy % (rows / 2)) < rows / 4;
            mx = top ? ix + cols : ix;
            my = (iy / (rows / 2)) * (rows / 4) + iy % (rows / 4);
            break;
        }
        case ScanLayout::FlippedStripe: {
            const bool top = (iy % (rows / 2)) >= rows / 4;
            mx = top ? ix + cols : ix;
            my = (iy / (rows / 2)) * (rows / 4) + iy % (rows / 4);
            break;
        }
        case ScanLayout::Checkered: {
            const bool top = (iy % (rows / 2)) < rows / 4;
            const bool left = ix < cols / 2;
            mx = top ? (left ? ix + cols / 2 : ix + cols) : (left ? ix : ix + cols / 2);
            my = (iy / (rows / 2)) * (rows / 4) + iy % (rows / 4);
            break;
        }
        case ScanLayout::Spiral: {
            const bool top = (iy % (rows / 2)) < rows / 4;
            const int quarter_w = cols / 4;
            const int quarter = ix / quarter_w;
            const int offset = ix % quarter_w;
            mx = 2 * quarter * quarter_w + (top ? quarter_w - 1 - offset : quarter_w + offset);
            my = (iy / (rows / 2)) * (rows / 4) + iy % (rows / 4);
            break;
        }
        case ScanLayout::ZStripe:
        case ScanLayout::ZStripeUneven: {
            constexpr int tile_w = 8;
            constexpr int tile_h = 4;
            const int even_offset = 8;
            const int odd_offset = layout == ScanLayout::ZStripe ? 0 : 8;
            const int odd_block = (iy / tile_h) % 2;
            const int even_shift = (1 - odd_block) * even_offset;
            const int odd_shift = odd_block * odd_offset;
            mx = ix + ((ix + even_shift) / tile_w) * tile_w + odd_shift;
            my = (iy % tile_h) + tile_h * (iy / (tile_h * 2));
            break;
        }
        case ScanLayout::Coreman: {
            const bool left = ix < cols / 2;
            if (iy <= 7 || (iy >= 16 && iy <= 23)) {
                mx = (ix / (cols / 2)) * cols + (ix % (cols / 2));
                my = (iy / (rows / 2)) * (rows / 4) + (iy % (rows / 4));
            } else {
                mx = left ? ix + cols / 2 : ix + cols;
                my = (iy / (rows / 2)) * (rows / 4) + iy % (rows / 4);
            }
            break;
        }
        default: break;
    }

    out_x = static_cast<uint16_t>(mx < 0 ? 0 : mx);
    out_y = static_cast<uint16_t>(my < 0 ? 0 : my);
}

inline uint16_t remap_block_x(uint16_t x, uint16_t y) {
    const uint16_t block = static_cast<uint16_t>(x / g_block);
    const uint16_t inner = static_cast<uint16_t>(x % g_block);
    const uint16_t base = static_cast<uint16_t>(block * 2u * g_block);
    if ((y & 4u) == 0) {
        const uint16_t shifted = g_block_reverse
                                     ? static_cast<uint16_t>(g_block - 1u - inner)
                                     : inner;
        return static_cast<uint16_t>(base + g_block + shifted);
    }
    return static_cast<uint16_t>(base + inner);
}


esp_err_t make_plan(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg, Plan& plan) {
    ESP_RETURN_ON_FALSE(module_count > 0, ESP_ERR_INVALID_ARG, kTag, "пустая цепочка");
    ESP_RETURN_ON_FALSE(module.width >= 2 && module.width <= 255, ESP_ERR_INVALID_ARG, kTag,
                        "ширина модуля %u вне 2..255", static_cast<unsigned>(module.width));
    ESP_RETURN_ON_FALSE(module.height >= 2 && module.height <= kMaxModuleHeight && (module.height % 2) == 0,
                        ESP_ERR_INVALID_ARG, kTag, "высота модуля %u не чётная или больше %u",
                        static_cast<unsigned>(module.height), static_cast<unsigned>(kMaxModuleHeight));

    // Раскладку выбирает пользователь: у панелей с четвертным сканом порядок
    // точек свой у каждой модели, и подобрать его можно только глазами по
    // тест-паттерну. Auto оставляет прежнее поведение — угадать по высоте и скану.
    switch (module.scan_layout) {
        case ScanLayout::TwoScan:
            plan.scan = STANDARD_TWO_SCAN;
            break;
        case ScanLayout::FourScan16: plan.scan = FOUR_SCAN_16PX_HIGH; break;
        case ScanLayout::FourScan32: plan.scan = FOUR_SCAN_32PX_HIGH; break;
        case ScanLayout::FourScan40: plan.scan = FOUR_SCAN_40PX_HIGH; break;
        case ScanLayout::FourScan64: plan.scan = FOUR_SCAN_64PX_HIGH; break;
        case ScanLayout::P10Base8:
            plan.scan = FOUR_SCAN_16PX_HIGH;
            plan.block = 8;
            plan.block_reverse = true;
            break;
        case ScanLayout::P10Base8Plain:
            plan.scan = FOUR_SCAN_16PX_HIGH;
            plan.block = 8;
            plan.block_reverse = false;
            break;
        case ScanLayout::P10Base16:
            plan.scan = FOUR_SCAN_16PX_HIGH;
            plan.block = 16;
            plan.block_reverse = true;
            break;
        case ScanLayout::P10Quarter:
        case ScanLayout::Kaler2Scan:
        case ScanLayout::Stripe:
        case ScanLayout::FlippedStripe:
        case ScanLayout::Checkered:
        case ScanLayout::Spiral:
        case ScanLayout::ZStripe:
        case ScanLayout::ZStripeUneven:
        case ScanLayout::Coreman: {
            const MapperTraits traits = mapper_traits(module.scan_layout, module.height);
            ESP_RETURN_ON_FALSE(traits.stretch != 0 && traits.rows != 0, ESP_ERR_INVALID_ARG, kTag,
                                "раскладка не подходит модулю высотой %u",
                                static_cast<unsigned>(module.height));
            plan.scan = STANDARD_TWO_SCAN;  // пересчёт делаем сами, библиотека просто гонит полотно
            plan.mapper = module.scan_layout;
            plan.mx_width = static_cast<uint16_t>(module.width * traits.stretch);
            plan.mx_height = static_cast<uint16_t>(traits.rows * 2u);
            plan.module_gain = 0;
            break;
        }
        case ScanLayout::Auto: {
            // Всё решает одно число: сколько строк приходится на адрес.
            // Две — обычная панель, четыре — четвертной скан, и тогда лишние
            // строки спрятаны в ширину сдвигового регистра.
            const uint16_t rows_per_address =
                module.scan_divisor != 0 ? module.height / module.scan_divisor : 0;
            if (rows_per_address == 2) {
                plan.scan = STANDARD_TWO_SCAN;
            } else if (rows_per_address == 4) {
                plan.scan = module.height == 40   ? FOUR_SCAN_40PX_HIGH
                            : module.height == 64 ? FOUR_SCAN_64PX_HIGH
                            : module.height == 32 ? FOUR_SCAN_32PX_HIGH
                                                  : FOUR_SCAN_16PX_HIGH;
            } else {
                ESP_LOGE(kTag, "модуль %ux%u скан 1/%u даёт %u строк на адрес, поддержаны 2 и 4",
                         static_cast<unsigned>(module.width), static_cast<unsigned>(module.height),
                         static_cast<unsigned>(module.scan_divisor),
                         static_cast<unsigned>(rows_per_address));
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }
    }

    if (plan.scan == STANDARD_TWO_SCAN && plan.mapper == ScanLayout::Auto) {
        plan.mx_width = module.width;
        plan.mx_height = module.height;
        plan.module_gain = 0;
    }

    if (plan.scan != STANDARD_TWO_SCAN) {
        plan.mx_width = static_cast<uint16_t>(module.width * 2u);
        plan.mx_height = static_cast<uint16_t>(module.height / 2u);
        plan.module_gain = module.width;
    }

    const auto geometry = config_dma_geometry(module);
    ESP_RETURN_ON_FALSE(plan.mx_width == geometry.width && plan.mx_height == geometry.rows * 2u,
                        ESP_ERR_INVALID_STATE, kTag, "DMA geometry disagrees with memory admission");
    plan.num_rows = geometry.rows;
    const uint32_t chain_width = static_cast<uint32_t>(module.width) * module_count;
    const uint32_t dma_chain_width = static_cast<uint32_t>(plan.mx_width) * module_count;
    ESP_RETURN_ON_FALSE(dma_chain_width <= kMaxDmaChainWidth, ESP_ERR_INVALID_ARG, kTag,
                        "цепочка из %u модулей не влезает в адресацию DMA-буфера",
                        static_cast<unsigned>(module_count));
    plan.chain_width = static_cast<uint16_t>(chain_width);
    plan.dma_chain_width = static_cast<uint16_t>(dma_chain_width);

    ESP_RETURN_ON_FALSE(cfg.bit_depth >= 2 && cfg.bit_depth <= 8, ESP_ERR_INVALID_ARG, kTag,
                        "глубина %u бит вне 2..8", static_cast<unsigned>(cfg.bit_depth));
    ESP_RETURN_ON_FALSE(cfg.buffers == 1 || cfg.buffers == 2, ESP_ERR_INVALID_ARG, kTag,
                        "копий кадра %u: драйвер держит одну или две",
                        static_cast<unsigned>(cfg.buffers));
    ESP_RETURN_ON_FALSE(valid_bus(cfg.bus_hz), ESP_ERR_INVALID_ARG, kTag,
                        "частота шины %u Гц: допустимы 16 и 20 МГц", static_cast<unsigned>(cfg.bus_hz));
    ESP_RETURN_ON_FALSE(cfg.latch_blanking >= kMinLatchBlanking && cfg.latch_blanking <= MAX_LAT_BLANKING,
                        ESP_ERR_INVALID_ARG, kTag, "latch blanking %u вне %u..%u тактов",
                        static_cast<unsigned>(cfg.latch_blanking), static_cast<unsigned>(kMinLatchBlanking),
                        static_cast<unsigned>(MAX_LAT_BLANKING));
    ESP_RETURN_ON_FALSE(config_frame_rate_valid(cfg.frame_rate), ESP_ERR_INVALID_ARG, kTag,
                        "частота кадров %u: допустимы 30, 60 и 120", static_cast<unsigned>(cfg.frame_rate));
    return ESP_OK;
}

// Строит таблицу строк и проверяет допущение быстрого пересчёта: библиотека
// сдвигает x линейно по номеру модуля, а от y зависит только слагаемое-сдвиг.
// Проверка стоит микросекунды на старте и ловит расхождение с библиотекой,
// которое иначе проявилось бы мусором на панелях.
template <PANEL_SCAN_TYPE Scan>
bool build_rows(const Plan& plan, uint16_t module_width, uint16_t module_height) {
    const int base = static_cast<int>(module_width);

    for (uint16_t y = 0; y < module_height; ++y) {
        VirtualCoords probe;
        probe.y = static_cast<int16_t>(y);
        const VirtualCoords mapped = ScanTypeMapping<Scan>::apply(probe, base);
        g_row_y[y] = static_cast<uint16_t>(mapped.y);
        g_row_x_bias[y] = static_cast<uint16_t>(mapped.x);
    }

    const uint16_t checks[] = {
        0u,
        1u,
        static_cast<uint16_t>(module_width - 1u),
        module_width,
        static_cast<uint16_t>(module_width + 1u),
        static_cast<uint16_t>(2u * module_width - 1u),
        static_cast<uint16_t>(plan.chain_width - 1u),
    };

    for (uint16_t y = 0; y < module_height; ++y) {
        for (const uint16_t x : checks) {
            if (x >= plan.chain_width) {
                continue;
            }
            VirtualCoords probe;
            probe.x = static_cast<int16_t>(x);
            probe.y = static_cast<int16_t>(y);
            const VirtualCoords mapped = ScanTypeMapping<Scan>::apply(probe, base);
            const uint16_t fast_x = static_cast<uint16_t>(x + plan.module_gain * module_index(x) + g_row_x_bias[y]);
            if (fast_x != static_cast<uint16_t>(mapped.x) || g_row_y[y] != static_cast<uint16_t>(mapped.y)) {
                ESP_LOGE(kTag, "раскладка скана из библиотеки разошлась с быстрым пересчётом на x=%u y=%u",
                         static_cast<unsigned>(x), static_cast<unsigned>(y));
                return false;
            }
        }
    }
    return true;
}

// Раскладка, разобранная сообществом для P10 32×16 с четвертным сканом
// (обсуждение #622 в репозитории библиотеки). Ключевое отличие от библиотечной:
// блок пикселей равен восьми точкам, а не ширине модуля, и в половине блоков
// порядок точек обратный. Универсальной раскладки для таких панелей нет —
// у каждой модели своя, поэтому вариант выбирается в настройках.
bool build_block_rows(uint16_t module_height, uint16_t block, bool reverse) {
    if (block == 0) {
        return false;
    }
    g_block = block;
    g_block_reverse = reverse;
    for (uint16_t y = 0; y < module_height; ++y) {
        g_row_y[y] = static_cast<uint16_t>((y >> 3) * 4u + (y & 0b11u));
        g_row_x_bias[y] = 0;  // сдвиг считается формулой блока, таблица не нужна
    }
    return true;
}

bool build_rows(const Plan& plan, const ModuleType& module) {
    g_mapper = plan.mapper;
    g_map_width = module.width;
    g_map_height = module.height;
    g_stretch_width = plan.mx_width;
    if (plan.mapper != ScanLayout::Auto) {
        return true;
    }
    if (plan.block != 0) {
        return build_block_rows(module.height, plan.block, plan.block_reverse);
    }
    switch (plan.scan) {
        case STANDARD_TWO_SCAN: return build_rows<STANDARD_TWO_SCAN>(plan, module.width, module.height);
        case FOUR_SCAN_16PX_HIGH: return build_rows<FOUR_SCAN_16PX_HIGH>(plan, module.width, module.height);
        case FOUR_SCAN_32PX_HIGH: return build_rows<FOUR_SCAN_32PX_HIGH>(plan, module.width, module.height);
        case FOUR_SCAN_40PX_HIGH: return build_rows<FOUR_SCAN_40PX_HIGH>(plan, module.width, module.height);
        case FOUR_SCAN_64PX_HIGH: return build_rows<FOUR_SCAN_64PX_HIGH>(plan, module.width, module.height);
        default: return false;
    }
}

// Точка цепочки в точку полотна драйвера. По этой же функции строится и
// проверяется таблица построчного вывода, поэтому она одна на оба пути.
inline void map_chain_to_dma(uint16_t x, uint16_t y, uint16_t& dma_x, uint16_t& dma_y) {
    if (g_mapper != ScanLayout::Auto) {
        // Маппер работает в координатах одного модуля, поэтому номер модуля в
        // цепочке добавляется отдельно уже в координатах полотна.
        const uint16_t index = module_index(x);
        map_hzeller(g_mapper, static_cast<uint16_t>(x - index * g_map_width), y, dma_x, dma_y);
        dma_x = static_cast<uint16_t>(dma_x + index * g_stretch_width);
    } else if (g_block != 0) {
        dma_x = remap_block_x(x, y);
        dma_y = g_row_y[y];
    } else {
        dma_x = static_cast<uint16_t>(x + g_module_gain * module_index(x) + g_row_x_bias[y]);
        dma_y = g_row_y[y];
    }
}

void free_run_table() {
    heap_caps_free(g_run_x);
    g_run_x = nullptr;
    g_run_width = 0;
    g_run_height = 0;
    g_run_stride = 0;
    g_chain_width = 0;
    g_run_modules = 0;
}

// Таблица построчного вывода строится по тому же пересчёту, что и попиксельный
// путь, и тут же сверяется с ним на всей цепочке. Проверка стоит миллисекунду
// на старте и ловит расхождение логом, а не мусором на панелях: допущение
// «раскладка внутри модуля от номера модуля не зависит» верно для всех веток
// пересчёта, но держится на их внутреннем устройстве, а не на контракте.
bool build_run_table(const Plan& plan, const ModuleType& module) {
    free_run_table();

    const size_t cells = static_cast<size_t>(module.width) * module.height;
    g_run_x = static_cast<uint16_t*>(heap_caps_malloc(cells * sizeof(uint16_t), MALLOC_CAP_INTERNAL));
    if (g_run_x == nullptr) {
        ESP_LOGE(kTag, "нет памяти под таблицу построчного вывода (%u байт)",
                 static_cast<unsigned>(cells * sizeof(uint16_t)));
        return false;
    }
    g_run_width = module.width;
    g_run_height = module.height;
    g_chain_width = plan.chain_width;
    // chain_width всегда кратна module.width — так его строит make_plan.
    g_run_modules = static_cast<uint16_t>(plan.chain_width / module.width);

    // Шаг за модуль берётся из той же ветки пересчёта, что и сама раскладка.
    if (plan.mapper != ScanLayout::Auto) {
        g_run_stride = plan.mx_width;
    } else if (plan.block != 0) {
        g_run_stride = static_cast<uint16_t>(2u * module.width);
    } else {
        g_run_stride = static_cast<uint16_t>(module.width + plan.module_gain);
    }

    for (uint16_t y = 0; y < module.height; ++y) {
        uint16_t dma_x = 0;
        uint16_t dma_y = 0;
        for (uint16_t px = 0; px < module.width; ++px) {
            map_chain_to_dma(px, y, dma_x, dma_y);
            g_run_x[static_cast<uint32_t>(y) * module.width + px] = dma_x;
        }
        g_row_dma_y[y] = dma_y;
    }

    for (uint16_t y = 0; y < module.height; ++y) {
        for (uint16_t x = 0; x < plan.chain_width; ++x) {
            uint16_t slow_x = 0;
            uint16_t slow_y = 0;
            map_chain_to_dma(x, y, slow_x, slow_y);
            const uint16_t index = module_index(x);
            const uint16_t fast_x = static_cast<uint16_t>(
                g_run_x[static_cast<uint32_t>(y) * module.width + (x - index * module.width)] +
                index * g_run_stride);
            if (fast_x != slow_x || g_row_dma_y[y] != slow_y) {
                ESP_LOGE(kTag,
                         "построчный вывод разошёлся с попиксельным на x=%u y=%u: %u/%u против %u/%u",
                         static_cast<unsigned>(x), static_cast<unsigned>(y),
                         static_cast<unsigned>(fast_x), static_cast<unsigned>(g_row_dma_y[y]),
                         static_cast<unsigned>(slow_x), static_cast<unsigned>(slow_y));
                free_run_table();
                return false;
            }
        }
    }
    return true;
}

// Библиотека занимает канал GDMA в begin() и не отдаёт его в деструкторе, а на
// исчерпании пула молча продолжает работу с неинициализированным дескриптором.
// Пробный захват канала превращает это в честную ошибку старта.
esp_err_t probe_gdma_channel() {
    gdma_channel_alloc_config_t alloc{};
    alloc.direction = GDMA_CHANNEL_DIRECTION_TX;
    gdma_channel_handle_t channel = nullptr;
    const esp_err_t err = gdma_new_ahb_channel(&alloc, &channel);
    if (err != ESP_OK) {
        return err;
    }
    gdma_del_channel(channel);
    return ESP_OK;
}

int8_t addr_msb_gpio(uint16_t num_rows) {
    if (num_rows < 2) {
        return -1;
    }
    const int index = 31 - __builtin_clz(static_cast<uint32_t>(num_rows - 1));
    if (index > 4) {
        return -1;
    }
    return board::kAddrPins[index];
}

}  // namespace

esp_err_t validate(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg) {
    Plan plan{};
    return make_plan(module, module_count, cfg, plan);
}

void set_color_order(ColorOrder order) {
    // Строка перестановки перечисляет, какой цвет висит на первой, второй и
    // третьей линии модуля, значит на линию надо подать одноимённый входной
    // канал: значение — позиция буквы названия в «RGB».
    static constexpr uint8_t kLaneOfComponent[6][3] = {
        {0, 1, 2},  // RGB
        {0, 2, 1},  // RBG
        {1, 0, 2},  // GRB
        {1, 2, 0},  // GBR
        {2, 0, 1},  // BRG
        {2, 1, 0},  // BGR
    };
    const uint8_t index = static_cast<uint8_t>(order) < 6 ? static_cast<uint8_t>(order) : 0;
    const uint8_t* lane = kLaneOfComponent[index];
    g_lane_map.store((static_cast<uint32_t>(lane[0]) << 16) | (static_cast<uint32_t>(lane[1]) << 8) |
                         lane[2],
                     std::memory_order_relaxed);
}

// Раскладка живёт в таблицах пересчёта координат и буфера DMA не касается,
// поэтому подбирать её можно на светящейся панели.
esp_err_t set_scan_layout(const ModuleType& module, uint16_t module_count) {
    if (g_panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Plan plan{};
    DisplayConfig probe{};  // проверяем только геометрию, электрика не меняется
    probe.bit_depth = 3;
    probe.latch_blanking = kMinLatchBlanking;
    ESP_RETURN_ON_ERROR(make_plan(module, module_count, probe, plan), kTag, "раскладка не принята");

    g_module_gain = plan.module_gain;
    g_width_recip = 0xFFFFFFFFu / module.width + 1u;
    g_block = plan.block;
    g_block_reverse = plan.block_reverse;
    ESP_RETURN_ON_FALSE(build_rows(plan, module), ESP_ERR_INVALID_STATE, kTag,
                        "раскладка скана не сошлась");
    ESP_RETURN_ON_FALSE(build_run_table(plan, module), ESP_ERR_INVALID_STATE, kTag,
                        "таблица построчного вывода не сошлась");
    g_panel->clearScreen();
    return ESP_OK;
}

// Настройки библиотеки под заданную длину цепочки и глубину. Всё, что не
// зависит от подбора — ноги, полярности, драйвер строк, частота шины, — здесь
// же: подбор перебирает только длину и глубину.
HUB75_I2S_CFG make_mx(const ModuleType& module, const Plan& plan, uint16_t modules,
                      const DisplayConfig& cfg, uint8_t depth, bool double_buffer) {
    HUB75_I2S_CFG mx;
    mx.mx_width = plan.mx_width;
    mx.mx_height = plan.mx_height;
    mx.chain_length = modules;

    // Ноги подключаем прямо, а порядок цветов переставляем при записи пикселя:
    // так его можно менять на живой панели, не пересобирая драйвер.
    mx.gpio = {board::kPinR1, board::kPinG1,  board::kPinB1, board::kPinR2, board::kPinG2,
               board::kPinB2, board::kPinA,   board::kPinB,  board::kPinC,  board::kPinD,
               board::kPinE,  board::kPinLat, board::kPinOe, board::kPinClk};

    switch (module.shift_driver) {
        case ShiftDriver::ICN2038S: mx.driver = HUB75_I2S_CFG::ICN2038S; break;
        case ShiftDriver::FM6124: mx.driver = HUB75_I2S_CFG::FM6124; break;
        case ShiftDriver::FM6126A: mx.driver = HUB75_I2S_CFG::FM6126A; break;
        case ShiftDriver::MBI5124: mx.driver = HUB75_I2S_CFG::MBI5124; break;
        case ShiftDriver::DP3246: mx.driver = HUB75_I2S_CFG::DP3246; break;
        case ShiftDriver::Generic: mx.driver = HUB75_I2S_CFG::SHIFTREG; break;
    }
    mx.oe_active_high = !module.oe_active_low;
    mx.invert_rgb = !module.data_active_high;
    switch (module.line_decoder) {
        case LineDecoder::Direct: mx.line_decoder = HUB75_I2S_CFG::TYPE_DIRECT; break;
        case LineDecoder::SM5266P: mx.line_decoder = HUB75_I2S_CFG::SM5266P; break;
        case LineDecoder::SM5368: mx.line_decoder = HUB75_I2S_CFG::SM5368; break;
        case LineDecoder::Type138: mx.line_decoder = HUB75_I2S_CFG::TYPE138; break;
    }
    mx.double_buff = double_buffer;
    mx.i2sspeed = static_cast<HUB75_I2S_CFG::clk_speed>(cfg.bus_hz);
    mx.latch_blanking = cfg.latch_blanking;
    // В конфигурации хранится признак инверсии фазы, у библиотеки — сама фаза,
    // и её штатное значение соответствует неинвертированной настройке.
    mx.clkphase = !cfg.clk_phase;
    mx.frame_rate = cfg.frame_rate;
    mx.setPixelColorDepthBits(depth);
    return mx;
}

esp_err_t start(const ModuleType& module, uint16_t module_count, const DisplayConfig& cfg, Info& info) {
    ESP_RETURN_ON_FALSE(g_panel == nullptr, ESP_ERR_INVALID_STATE, kTag, "драйвер уже запущен");

    Plan plan{};
    ESP_RETURN_ON_ERROR(make_plan(module, module_count, cfg, plan), kTag, "настройки панели не приняты");

    ESP_RETURN_ON_ERROR(probe_gdma_channel(), kTag, "свободных каналов GDMA нет");

    // All payloads are owned by one checked DMA generation allocator.
    ESP_RETURN_ON_FALSE(ledcast::dma_pool_ready(), ESP_ERR_INVALID_STATE, kTag,
                        "пул DMA не выделен, панель не поднять");

    // Кадр уходит на всю цепочку целиком, укоротить его нельзя.
    //
    // Панели HUB75 соединены последовательно и работают как один длинный
    // сдвиговый регистр: данные входят в ближнюю к плате панель и тактами
    // проталкиваются дальше. Кадр короче цепочки не «не доходит» до дальних
    // панелей — он выталкивает в них содержимое ближних, и они показывают эхо
    // предыдущих кадров. Чтобы дальние были тёмными, в них надо вдвигать нули
    // каждый кадр, то есть держать буфер на всю длину. Запрошенная глубина
    // и число буферов сохраняются: скрыто ухудшать качество при нехватке нельзя.
    const uint8_t depth = cfg.bit_depth;
    MatrixPanel_I2S_DMA* panel = nullptr;
    // Include both pixel payloads and descriptor rings before starting DMA.
    LineConfig line{};
    line.module = module;
    line.module_count = module_count;
    const uint32_t needed = config_dma_bytes(line, cfg, depth);
    if (needed <= ledcast::dma_pool_capacity()) {
        const HUB75_I2S_CFG mx = make_mx(module, plan, module_count, cfg, depth, cfg.buffers > 1);
        panel = new (std::nothrow) MatrixPanel_I2S_DMA(mx);
        if (panel != nullptr && !panel->begin()) {
            // Битпланы и дескрипторы лежат в пуле, а не в объекте: удаление
            // панели снимает только её саму, пул освободит следующая раскладка.
            delete panel;
            panel = nullptr;
        }
    }
    if (panel == nullptr) {
        ESP_LOGW(kTag, "%u бит на цепочку не поднялись: буфер %u байт, пул %u байт",
                 static_cast<unsigned>(depth), static_cast<unsigned>(needed),
                 static_cast<unsigned>(ledcast::dma_pool_capacity()));
        return ESP_ERR_NO_MEM;
    }

    g_panel = panel;
    g_double_buffer = cfg.buffers > 1;
    g_module_gain = plan.module_gain;
    g_width_recip = 0xFFFFFFFFu / module.width + 1u;
    set_color_order(module.color_order);
    build_srgb_plane_table();
    if (!build_rows(plan, module) || !build_run_table(plan, module)) {
        delete g_panel;
        g_panel = nullptr;
        ESP_LOGE(kTag, "раскладка скана не сошлась");
        return ESP_ERR_INVALID_STATE;
    }

    g_panel->setBrightness(cfg.brightness);
    g_brightness = cfg.brightness;

    info.bit_depth = g_panel->getCfg().getPixelColorDepthBits();
    info.effective_bit_depth = static_cast<uint8_t>(
        info.bit_depth - g_panel->getLsbMsbTransitionBit());
    info.dma_bytes = needed;
    info.scan_period_us = g_panel->scanPeriodUs();
    info.passes_per_frame = g_panel->passesPerFrame();
    info.num_rows = plan.num_rows;
    info.addr_msb_gpio = addr_msb_gpio(plan.num_rows);
    info.addr_rising_edges_per_refresh = 1;
    const auto decoder = g_panel->getCfg().line_decoder;
    const bool parallel = decoder != HUB75_I2S_CFG::SM5266P &&
                          decoder != HUB75_I2S_CFG::SM5368;
    if (parallel && info.addr_msb_gpio >= 0) {
        info.addr_rising_edges_per_refresh = hub75_bcm::address_rising_edges(
            plan.num_rows, info.bit_depth, g_panel->getLsbMsbTransitionBit(), hub75_bcm::kInterleavedRows,
            decoder == HUB75_I2S_CFG::TYPE_DIRECT, plan.num_rows / 2);
    }

    if (info.bit_depth != cfg.bit_depth) {
        ESP_LOGW(kTag, "запрошено %u бит на канал, памяти хватило на %u",
                 static_cast<unsigned>(cfg.bit_depth), static_cast<unsigned>(info.bit_depth));
    }
    ESP_LOGI(kTag, "панель %ux%u на модуль, цепочка %u, %u бит, буфер %u КБ, расчётная частота %d Гц",
             static_cast<unsigned>(plan.mx_width), static_cast<unsigned>(plan.mx_height),
             static_cast<unsigned>(module_count), static_cast<unsigned>(info.bit_depth),
             static_cast<unsigned>(info.dma_bytes / 1024), g_panel->calculated_refresh_rate);
    return ESP_OK;
}

void stop() {
    MatrixPanel_I2S_DMA* panel = g_panel;
    if (panel == nullptr) {
        return;
    }
    g_panel = nullptr;
    g_double_buffer = false;
    panel->stopDMAoutput();
    delete panel;
    g_brightness = 0;
    free_run_table();
}

// Построчный вывод.
//
// Попиксельный путь на каждую точку заново считает номер модуля, раскладку
// внутри модуля, строку полотна, половину RGB1/RGB2, маску и указатели на
// битпланы. Всё это постоянно вдоль строки, поэтому здесь считается один раз на
// прогон, а на точку остаётся выборка из таблицы, три перевода шкалы и по
// одному read-modify-write на бит глубины.
//
// Трогаются только биты RGB под маской: OE, LAT и адресные биты в тех же словах
// расставляет clearScreen и setBrightness, и затирать их нельзя.
// Погасить прогон точек. То же, что write_run чёрным, но без чтения цвета и без
// перевода шкалы: фон под строку кладётся каждый кадр, и это половина всей
// работы рендера — гасить дешевле, чем считать чёрный цвет по общему пути.
IRAM_ATTR void clear_run(uint16_t x, uint16_t y, uint16_t count, int8_t step) {
    MatrixPanel_I2S_DMA* panel = g_panel;
    if (panel == nullptr || g_run_x == nullptr || count == 0 || y >= g_run_height) {
        return;
    }

    const uint16_t* row_map = g_run_x + static_cast<uint32_t>(y) * g_run_width;
    const uint8_t rows_per_frame = panel->rowsPerFrame();

    uint16_t dma_row = g_row_dma_y[y];
    uint16_t clear_mask = BITMASK_RGB1_CLEAR;
    if (dma_row >= rows_per_frame) {
        dma_row = static_cast<uint16_t>(dma_row - rows_per_frame);
        clear_mask = BITMASK_RGB2_CLEAR;
    }

    const uint8_t depth = panel->getCfg().getPixelColorDepthBits();
    ESP32_I2S_DMA_STORAGE_TYPE* plane[16];
    for (uint8_t d = 0; d < depth; ++d) {
        plane[d] = panel->rowDataPtr(dma_row, d);
        if (plane[d] == nullptr) {
            return;
        }
    }
    const uint16_t span = panel->pixelsPerRow();

    int32_t chain_x = static_cast<int32_t>(x);
    uint16_t index = 0;
    uint16_t px_in_module = 0;
    bool position_known = false;

    for (uint16_t i = 0; i < count; ++i, chain_x += step) {
        if (chain_x < 0 || chain_x >= static_cast<int32_t>(g_chain_width)) {
            position_known = false;
            continue;
        }
        const uint16_t cx = static_cast<uint16_t>(chain_x);
        if (!position_known) {
            index = module_index(cx);
            px_in_module = static_cast<uint16_t>(cx - index * g_run_width);
            position_known = true;
        }
        const uint16_t dma_x =
            static_cast<uint16_t>(row_map[px_in_module] + index * g_run_stride);
        if (dma_x < span) {
            for (uint8_t d = 0; d < depth; ++d) {
                ESP32_I2S_DMA_STORAGE_TYPE* p = plane[d] + dma_x;
                *p = static_cast<ESP32_I2S_DMA_STORAGE_TYPE>(*p & clear_mask);
            }
        }
        if (step > 0) {
            if (++px_in_module == g_run_width) {
                px_in_module = 0;
                ++index;
            }
        } else if (step < 0) {
            if (px_in_module == 0) {
                px_in_module = static_cast<uint16_t>(g_run_width - 1);
                if (index == 0) {
                    position_known = false;
                } else {
                    --index;
                }
            } else {
                --px_in_module;
            }
        }
    }
}

// Прогон точек вдоль цепочки.
IRAM_ATTR void write_run(uint16_t x, uint16_t y, uint16_t count, int8_t step, const uint8_t* rgb) {
    MatrixPanel_I2S_DMA* panel = g_panel;
    if (panel == nullptr || g_run_x == nullptr || rgb == nullptr || count == 0) {
        return;
    }
    if (y >= g_run_height) {
        return;
    }

    const uint16_t* row_map = g_run_x + static_cast<uint32_t>(y) * g_run_width;
    const uint8_t rows_per_frame = panel->rowsPerFrame();

    uint16_t dma_row = g_row_dma_y[y];
    uint16_t clear_mask = BITMASK_RGB1_CLEAR;
    uint8_t lane_shift = 0;
    if (dma_row >= rows_per_frame) {
        dma_row = static_cast<uint16_t>(dma_row - rows_per_frame);
        clear_mask = BITMASK_RGB2_CLEAR;
        lane_shift = BITS_RGB2_OFFSET;
    }

    const uint8_t depth = panel->getCfg().getPixelColorDepthBits();
    ESP32_I2S_DMA_STORAGE_TYPE* plane[16];
    for (uint8_t d = 0; d < depth; ++d) {
        plane[d] = panel->rowDataPtr(dma_row, d);
        if (plane[d] == nullptr) {
            return;
        }
    }

    const uint16_t span = panel->pixelsPerRow();

    // Перестановка каналов снимается один раз: она меняется настройкой, а не
    // внутри кадра.
    const uint32_t lanes = g_lane_map.load(std::memory_order_relaxed);
    const uint8_t lane_r = static_cast<uint8_t>((lanes >> 16) & 3u);
    const uint8_t lane_g = static_cast<uint8_t>((lanes >> 8) & 3u);
    const uint8_t lane_b = static_cast<uint8_t>(lanes & 3u);

    // Разрядность разбирается один раз на прогон: внутри цикла по точкам не
    // остаётся ни одной ветки по глубине.
    const ledcast::colour::Quantizer quant = ledcast::colour::quantizer_for(depth);
    // Строка полотна вдоль прогона одна, поэтому старшая половина индекса плитки
    // дизеринга постоянна.
    const uint8_t bayer_row = static_cast<uint8_t>((y & 3u) << 2);

    // Готовое слово битпланов для последнего цвета прогона.
    //
    // Прогон почти всегда одноцветный — так рисуют заливка, отрезки эффектов,
    // фон под текстом. Пересчитывать на каждой точке гамму, порог и раскладку по
    // битпланам заново значит выполнять одну и ту же работу сотнями раз подряд.
    //
    // Порог дизеринга меняется с периодом четыре вдоль строки, поэтому у одного
    // цвета четыре разных слова. Считаются они лениво: короткий прогон не платит
    // за фазы, до которых не дошёл.
    uint8_t last_rgb[3] = {0, 0, 0};
    bool have_last = false;
    uint8_t phase_ready = 0;
    uint32_t word_cache[4] = {};

    // Номер модуля катится вместе с точкой, а не вычисляется делением на каждой:
    // деление здесь превращается в 64-битное умножение, и на двадцати тысячах
    // точек кадра это заметная доля всего рендера.
    int32_t chain_x = static_cast<int32_t>(x);
    uint16_t index = 0;
    uint16_t px_in_module = 0;
    bool position_known = false;

    for (uint16_t i = 0; i < count; ++i, chain_x += step, rgb += 3) {
        if (chain_x < 0 || chain_x >= static_cast<int32_t>(g_chain_width)) {
            position_known = false;
            continue;
        }
        const uint16_t cx = static_cast<uint16_t>(chain_x);
        if (!position_known) {
            index = module_index(cx);
            px_in_module = static_cast<uint16_t>(cx - index * g_run_width);
            position_known = true;
        }
        const uint16_t dma_x =
            static_cast<uint16_t>(row_map[px_in_module] + index * g_run_stride);

        if (dma_x < span) {
            uint32_t word = 0;
            if (depth == 8) {
                // На полных 8 битах Bayer нет, поэтому x/y на цвет не влияют.
                // Три выборки и два OR заменяют весь конвейер гаммы и квантования.
                const uint8_t in[3] = {rgb[0], rgb[1], rgb[2]};
                word = g_srgb_plane[in[lane_r]] |
                       (g_srgb_plane[in[lane_g]] << 1) |
                       (g_srgb_plane[in[lane_b]] << 2);
            } else {
            if (!have_last || rgb[0] != last_rgb[0] || rgb[1] != last_rgb[1] ||
                rgb[2] != last_rgb[2]) {
                last_rgb[0] = rgb[0];
                last_rgb[1] = rgb[1];
                last_rgb[2] = rgb[2];
                have_last = true;
                phase_ready = 0;
            }
            // Фаза плитки берётся от координаты полотна, а не от координаты
            // буфера: скан 1/4 переставляет точки, и плитка по буферу легла бы
            // на панель рваной.
            const uint8_t phase = static_cast<uint8_t>(cx & 3u);
            if ((phase_ready & (1u << phase)) == 0) {
                const uint8_t in[3] = {last_rgb[0], last_rgb[1], last_rgb[2]};
                const uint16_t bias = ledcast::colour::quantization_bias(
                    quant, depth, phase, static_cast<uint16_t>(bayer_row >> 2));
                const uint16_t rv = ledcast::colour::map8(in[lane_r], bias, quant);
                const uint16_t gv = ledcast::colour::map8(in[lane_g], bias, quant);
                const uint16_t bv = ledcast::colour::map8(in[lane_b], bias, quant);
                word_cache[phase] = ledcast::colour::plane_word(rv, gv, bv);
                phase_ready = static_cast<uint8_t>(phase_ready | (1u << phase));
            }
                word = word_cache[phase];
            }
            // План вынимается из готового слова одним сдвигом: разбор значения
            // бит за битом внутри цикла по планам стоил дороже всей остальной
            // записи точки вместе взятой.
            for (uint8_t d = 0; d < depth; ++d, word >>= 3) {
                ESP32_I2S_DMA_STORAGE_TYPE* p = plane[d] + dma_x;
                *p = static_cast<ESP32_I2S_DMA_STORAGE_TYPE>(
                    (*p & clear_mask) |
                    static_cast<ESP32_I2S_DMA_STORAGE_TYPE>((word & 7u) << lane_shift));
            }
        }

        // Шаг по цепочке — ровно на точку в ту или другую сторону, поэтому
        // границу модуля достаточно проверить сравнением.
        if (step > 0) {
            if (++px_in_module == g_run_width) {
                px_in_module = 0;
                ++index;
            }
        } else if (step < 0) {
            if (px_in_module == 0) {
                px_in_module = static_cast<uint16_t>(g_run_width - 1);
                if (index == 0) {
                    position_known = false;
                } else {
                    --index;
                }
            } else {
                --px_in_module;
            }
        }
    }
}

IRAM_ATTR esp_err_t flip() {
    MatrixPanel_I2S_DMA* panel = g_panel;
    if (panel == nullptr || !g_double_buffer) {
        return ESP_OK;
    }
    // Обмен возвращается только тогда, когда вывод действительно перешёл на
    // новую копию. При отказе переход остаётся pending: оба DMA-буфера закрыты
    // для CPU, пока prepare_frame() не подтвердит по GDMA, какая копия стала
    // видимой. Это теряет кадр, но никогда не пишет в активный scanout.
    return panel->flipDMABuffer();
}

esp_err_t prepare_frame(bool& resolved) {
    resolved = false;
    MatrixPanel_I2S_DMA* panel = g_panel;
    if (panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return panel->resolvePendingDMABuffer(resolved);
}

void set_brightness(uint8_t value) {
    // Смена яркости переписывает биты OE во всём DMA-буфере, а пульт шлёт её
    // десятками раз в секунду — трогаем драйвер только на реальном изменении.
    if (g_panel == nullptr || value == g_brightness) {
        return;
    }
    g_brightness = value;
    g_panel->setBrightness(value);
}

}  // namespace ledcast::hub75_backend
