#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>

namespace ledcast {

inline constexpr uint8_t kMaxLineModules = 24;
inline constexpr uint8_t kMaxChainModules = kMaxLineModules;
inline constexpr uint16_t kMaxModuleHeight = 64;
inline constexpr uint32_t kDmaPoolBytes = 92 * 1024;
// LCD_CAM делит 160 МГц нацело только на эти частоты из годных для HUB75.
inline constexpr uint32_t kBusFrequencies[] = {16000000, 20000000};
// Частота кадров вывода — одна на видеопоток и автономный показ. 30 — для
// длинных строк: кадр задержки показа занимает память строки, и при 30 кадрах
// та же задержка стоит вдвое меньше.
inline constexpr uint8_t kFrameRates[] = {30, 60, 120};
inline constexpr bool config_frame_rate_valid(uint8_t rate) {
    for (const uint8_t candidate : kFrameRates)
        if (candidate == rate) return true;
    return false;
}
inline constexpr uint8_t kWifiProfileLimit = 10;
inline constexpr size_t kShowTextBytes = 1024;

enum class ColorOrder : uint8_t { RGB, RBG, GRB, GBR, BRG, BGR };
enum class ScanLayout : uint8_t {
    Auto, TwoScan, FourScan16, FourScan32, FourScan40, FourScan64,
    P10Base8, P10Base8Plain, P10Base16, P10Quarter, Kaler2Scan,
    Stripe, FlippedStripe, Checkered, Spiral, ZStripe, ZStripeUneven, Coreman,
};
inline constexpr ScanLayout kScanLayoutLast = ScanLayout::Coreman;
enum class ShiftDriver : uint8_t { Generic, ICN2038S, FM6124, FM6126A, MBI5124, DP3246 };
enum class LineDecoder : uint8_t { Type138, Direct, SM5266P, SM5368 };

struct ModuleType {
    uint16_t width = 32;
    uint16_t height = 16;
    uint8_t scan_divisor = 4;
    ColorOrder color_order = ColorOrder::BGR;
    bool oe_active_low = true;
    bool data_active_high = true;
    ScanLayout scan_layout = ScanLayout::Spiral;
    ShiftDriver shift_driver = ShiftDriver::Generic;
    LineDecoder line_decoder = LineDecoder::Type138;
};

struct LineModule {
    uint8_t physical_module = 0;
    bool rotate_180 = false;
    bool mirror_x = false;
    bool mirror_y = false;
};

struct LineConfig {
    ModuleType module;
    uint8_t module_count = 1;
    LineModule modules[kMaxLineModules]{};
};

struct DisplayConfig {
    uint8_t bit_depth = 8;
    uint8_t buffers = 2;
    uint32_t bus_hz = 20000000;
    bool clk_phase = true;
    uint8_t latch_blanking = 4;
    uint8_t brightness = 128;
    uint8_t frame_rate = 120;
};

struct StreamConfig {
    bool enabled = true;
    uint16_t port = 4048;
    uint16_t timeout_ms = 2000;
};

enum class ShowMode : uint8_t { Static, ScrollRightToLeft };

struct ShowConfig {
    bool enabled = false;
    ShowMode mode = ShowMode::Static;
    uint8_t font_size_px = 14;
    uint16_t speed_px_s = 50;
    // Статика: сколько секунд стоит фраза, 1…3600. Бегущая строка: сколько
    // секунд одна фраза едет сама за собой, прежде чем въедет следующая;
    // 0 — фразы идут по очереди.
    uint16_t duration_s = 8;
    uint8_t red = 255;
    uint8_t green = 255;
    uint8_t blue = 255;
    char text[kShowTextBytes + 1]{};
};

struct WifiProfile { char ssid[33]{}; char password[65]{}; };
struct WifiConfig {
    char ap_password[65] = "88888888";
    uint8_t ap_channel = 6;
    // Имя строки для людей; пусто — её сетевое имя (ledcast-xxxx).
    char hostname[32] = "";
    WifiProfile profiles[kWifiProfileLimit]{};
    uint8_t profile_count = 0;
};

struct SecurityConfig {
    bool web_auth_enabled = false;
    char web_user[24] = "admin";
    char web_password[33]{};
};

struct DeviceConfig {
    LineConfig line;
    DisplayConfig display;
    StreamConfig stream;
    ShowConfig show;
    WifiConfig wifi;
    SecurityConfig security;
};

inline constexpr uint16_t module_label(uint8_t module) { return module + 1u; }

DeviceConfig config_defaults();
esp_err_t config_init();
// Keeps one immutable revision alive while other tasks commit new settings.
// Acquiring/copying a handle does not allocate or copy DeviceConfig.
class ConfigSnapshot {
  public:
    ConfigSnapshot() = default;
    ConfigSnapshot(const ConfigSnapshot& other);
    ConfigSnapshot(ConfigSnapshot&& other) noexcept;
    ConfigSnapshot& operator=(ConfigSnapshot other) noexcept;
    ~ConfigSnapshot();
    const DeviceConfig& operator*() const;
    const DeviceConfig* operator->() const;
    explicit operator bool() const { return node_ != nullptr; }
    bool operator==(const ConfigSnapshot& other) const { return node_ == other.node_; }
    void swap(ConfigSnapshot& other) noexcept;

  private:
    struct Node;
    explicit ConfigSnapshot(Node* node) : node_(node) {}
    Node* node_ = nullptr;
    friend ConfigSnapshot config_snapshot_create(const DeviceConfig& cfg);
    friend ConfigSnapshot config_snapshot_defaults();
};
// Retain the handle for any references. Before initialization returns defaults.
ConfigSnapshot config_get();
// A concurrent commit rejects this update with ESP_ERR_INVALID_STATE.
esp_err_t config_set(const DeviceConfig& cfg, const ConfigSnapshot& expected);
esp_err_t config_flush();
// Все домены к заводским, включая сеть и вход в веб: это крайняя мера (10 с
// кнопки, «Заводской сброс» в вебе), после неё плата ждёт на своей точке
// доступа с заводским паролем. Забыть только сети — обнулить wifi.profile_count.
esp_err_t config_reset_factory();

uint32_t config_dma_bytes(const LineConfig& line, const DisplayConfig& display, uint8_t bit_depth);
struct DmaGeometry { uint16_t width; uint8_t rows; };
DmaGeometry config_dma_geometry(const ModuleType& module);
uint32_t config_dma_bytes_per_module(const ModuleType& module, uint8_t bit_depth, uint8_t buffers);
uint8_t config_chain_capacity(const LineConfig& line, const DisplayConfig& display,
                              uint8_t bit_depth, uint8_t buffers);
uint8_t config_max_bit_depth(const LineConfig& line, const DisplayConfig& display);
uint8_t config_dma_overhead(ScanLayout layout);
uint32_t config_bcm_plane_weight(uint8_t bit_depth, uint8_t transition_bit, uint8_t plane);
uint32_t config_bcm_extra_passes(uint8_t bit_depth, uint8_t transition_bit);

esp_err_t config_validate(const DeviceConfig& cfg, char* reason, size_t reason_len);
using ConfigApplier = esp_err_t (*)(const DeviceConfig& cfg);
esp_err_t config_set_applier(ConfigApplier applier);

}  // namespace ledcast
