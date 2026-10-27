#include <atomic>
#include <cstring>

#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "stream_node.h"
#include "deviceconfig.h"
#include "display.h"
#include "display_rebuild.h"
#include "dmapool.h"
#include "health.h"
#include "linear.h"
#include "netconfig.h"
#include "power_cycle_screen.h"
#include "show.h"
#include "webui.h"

namespace {
constexpr char kTag[] = "ledcast";
ledcast::DeviceConfig s_applied{};
bool s_applied_valid = false;
std::atomic<bool> s_services_ready{false};
std::atomic<bool> s_output_ready{false};
StaticSemaphore_t s_runtime_mutex_storage;
SemaphoreHandle_t s_runtime_mutex = nullptr;

class RuntimeLock {
public:
    RuntimeLock() { xSemaphoreTake(s_runtime_mutex, portMAX_DELAY); }
    ~RuntimeLock() { xSemaphoreGive(s_runtime_mutex); }
};

esp_err_t log_result(const char* subsystem, esp_err_t error) {
    if (error != ESP_OK) ESP_LOGE(kTag, "%s: %s", subsystem, esp_err_to_name(error));
    return error;
}

void on_stream_timeout() {
    if (ledcast::show_active()) return;
    ledcast::linear_blackout();
    ledcast::health_led_set(ledcast::LedPattern::Running);
}

void on_network_interface_changed() {
    // Wi-Fi can publish its first interface event before app_main has created
    // the HTTP server. There are no sockets to rebind during that boot window.
    if (!s_services_ready.load(std::memory_order_acquire)) return;
    // AP+STA -> STA (and the reverse fallback) invalidates sockets bound while
    // the old interface existed. Rebind both transports without discarding the
    // browser session or the selected hardware configuration.
    {
        RuntimeLock lock;
        log_result("stream network stop", ledcast::stream_stop());
    }
    // HTTP shutdown can wait for a settings request. Never hold the runtime
    // lock while waiting for that request to complete.
    const esp_err_t web = log_result("web UI network rebind", ledcast::webui_rebind());
    RuntimeLock lock;
    const auto& config = s_applied;
    if (web == ESP_OK && s_output_ready.load(std::memory_order_acquire)) {
        if (!config.show.enabled) {
            log_result("stream network rebind", ledcast::stream_start(config.stream, on_stream_timeout));
        }
    }
}

// На время загрузки прошивки строка показывает только её ход: поток и шоу
// стоят, буферы кадров уходят в кучу — запись во flash и HTTP берут память.
void on_ota_begin() {
    RuntimeLock lock;
    ledcast::display_test_hold(true);
    ledcast::show_pause_for_test(true);
    log_result("stream stop for update", ledcast::stream_stop());
    log_result("stream buffers for update", ledcast::stream_release_buffers());
}

// Загрузка сорвалась: строка возвращается к тому, что показывала.
void on_ota_end() {
    RuntimeLock lock;
    const auto& config = s_applied;
    const bool test = ledcast::webui_test_pattern()[0] != '\0';
    ledcast::display_test_hold(test || ledcast::show_active());
    ledcast::show_pause_for_test(test);
    if (test) {
        log_result("test redraw after update", ledcast::webui_redraw_test());
    } else if (config.show.enabled) {
        ledcast::show_redraw();
    } else if (s_output_ready.load(std::memory_order_acquire) && config.stream.enabled) {
        log_result("stream after update", ledcast::stream_start(config.stream, on_stream_timeout));
    }
}

esp_err_t apply_config_attempt(const ledcast::DeviceConfig& config, bool restoring = false) {
    const bool first = !s_applied_valid;
    const bool line_changed = first || std::memcmp(&s_applied.line, &config.line, sizeof(config.line)) != 0;
    const bool canvas_changed = first ||
        s_applied.line.module.width != config.line.module.width ||
        s_applied.line.module.height != config.line.module.height ||
        s_applied.line.module_count != config.line.module_count;
    const bool brightness_changed = first || s_applied.display.brightness != config.display.brightness;
    auto previous_display = s_applied.display;
    auto next_display = config.display;
    previous_display.brightness = next_display.brightness;
    const bool display_changed = first || std::memcmp(&previous_display, &next_display, sizeof(next_display)) != 0;
    const bool output_changed = line_changed || display_changed;
    const bool show_changed = first || std::memcmp(&s_applied.show, &config.show, sizeof(config.show)) != 0;
    // Stream buffers contain the logical RGB canvas, not HUB75 scan data. Module
    // order/orientation, colour wiring, bit depth, bus speed and refresh can be
    // changed without destroying the UDP receiver. Only a canvas-size change
    // invalidates its buffers.
    const bool stream_was_running = s_applied_valid && s_applied.stream.enabled && !s_applied.show.enabled;
    const bool stream_should_run = config.stream.enabled && !config.show.enabled;
    // Пересборка драйвера выделяет DMA заново, а на короткой строке ей не
    // хватает памяти, пока заняты буферы кадров потока. Видео на это время
    // прерывается: буферы уходят в кучу и после пересборки выделяются снова.
    const bool display_rebuilt = !first &&
        ledcast::display_needs_rebuild(s_applied.line.module, s_applied.line.module_count,
                                       s_applied.display, config.line.module,
                                       config.line.module_count, config.display);
    const bool stop_stream = stream_was_running &&
        (canvas_changed || display_rebuilt || !stream_should_run);
    if (stop_stream) {
        const esp_err_t stopped = ledcast::stream_stop();
        if (stopped != ESP_OK) return stopped;
        if (display_rebuilt) {
            const esp_err_t released = ledcast::stream_release_buffers();
            if (released != ESP_OK) return released;
        }
    }
    if (output_changed) {
        // One generation change: no Show/test/NDI frame can span the mapper
        // update and driver rebuild. Per-run pixel locking is not sufficient.
        if (!ledcast::display_config_begin()) return ESP_ERR_INVALID_STATE;
        esp_err_t error = ledcast::linear_configure(config.line);
        if (error == ESP_OK)
            error = ledcast::display_reconfigure(config.line.module, config.line.module_count, config.display);
        s_output_ready.store(error == ESP_OK, std::memory_order_release);
        ledcast::display_config_end();
        if (error != ESP_OK) return error;
        if (stream_should_run && stop_stream) {
            const esp_err_t started = ledcast::stream_start(config.stream, on_stream_timeout);
            if (started != ESP_OK) return started;
        }
        const esp_err_t redrawn = ledcast::webui_redraw_test();
        if (redrawn != ESP_OK) return redrawn;
        if (config.show.enabled) ledcast::show_redraw();
    } else if (brightness_changed) {
        // Brightness only rewrites OE bits in both DMA copies. It must not tear
        // down UDP, rebuild descriptors or interrupt a live stream.
        ledcast::display_update_brightness(config.display.brightness);
    }
    if (show_changed) {
        const esp_err_t error = ledcast::show_reconfigure(config.show);
        if (error != ESP_OK) return error;
        if (stream_should_run && !stream_was_running) {
            const esp_err_t started = ledcast::stream_start(config.stream, on_stream_timeout);
            if (started != ESP_OK) return started;
        }
    }
    if (!output_changed && !show_changed && !config.show.enabled &&
        (first || std::memcmp(&s_applied.stream, &config.stream, sizeof(config.stream)) != 0)) {
        const esp_err_t error = ledcast::stream_reconfigure(config.stream); if (error != ESP_OK) return error;
    }
    if (!restoring && (first || std::memcmp(&s_applied.wifi, &config.wifi, sizeof(config.wifi)) != 0)) {
        const esp_err_t error = ledcast::net_reconfigure(config.wifi); if (error != ESP_OK) return error;
    }
    s_applied = config;
    s_applied_valid = true;
    return ESP_OK;
}

esp_err_t apply_config(const ledcast::DeviceConfig& config) {
    RuntimeLock lock;
    const bool can_restore = s_applied_valid;
    const esp_err_t error = apply_config_attempt(config);
    if (error == ESP_OK || !can_restore) return error;

    // config_set has not published the candidate. A failed rebuild may still
    // have replaced runtime buffers, so restore them before returning failure.
    const esp_err_t stopped = ledcast::stream_stop();
    if (stopped != ESP_OK) {
        ESP_LOGE(kTag, "configuration rollback: stream stop failed: %s", esp_err_to_name(stopped));
        return error;
    }
    s_applied_valid = false;
    const esp_err_t restored = apply_config_attempt(s_applied, true);
    if (restored != ESP_OK) {
        ESP_LOGE(kTag, "configuration rollback failed: %s", esp_err_to_name(restored));
    }
    return error;
}

// Автономный запуск включает встроенное шоу насовсем, если в нём есть текст:
// переключатель в интерфейсе показывает правду, и выключить шоу можно там же.
void enable_standalone_show() {
    const auto snapshot = ledcast::config_get();
    if (snapshot->show.enabled || !ledcast::show_has_text(snapshot->show)) return;
    ledcast::DeviceConfig config = *snapshot;
    config.show.enabled = true;
    if (log_result("standalone show", ledcast::config_set(config, snapshot)) == ESP_OK)
        log_result("standalone show flush", ledcast::config_flush());
}

void on_button(ledcast::ButtonEvent event) {
    if (event == ledcast::ButtonEvent::Hold5s) {
        const auto snapshot = ledcast::config_get();
        ledcast::DeviceConfig config = *snapshot;
        config.wifi.profile_count = 0;
        if (ledcast::config_set(config, snapshot) == ESP_OK && ledcast::config_flush() == ESP_OK) esp_restart();
    } else if (event == ledcast::ButtonEvent::Hold10s) {
        if (ledcast::config_reset_factory() == ESP_OK && ledcast::config_flush() == ESP_OK) esp_restart();
    }
}
}  // namespace

extern "C" void app_main() {
    s_runtime_mutex = xSemaphoreCreateMutexStatic(&s_runtime_mutex_storage);
    const ledcast::BootMode mode = ledcast::health_init();
    ledcast::health_led_set(ledcast::LedPattern::Booting);
    log_result("configuration", ledcast::config_init());
    const uint8_t power_cycles = ledcast::health_power_cycles();
    const bool standalone = power_cycles == ledcast::power_cycles::kForStandalone;
    if (standalone) enable_standalone_show();
    const auto snapshot = ledcast::config_get();
    const ledcast::DeviceConfig& config = *snapshot;
    log_result("DMA pool", ledcast::dma_pool_init());
    const bool output_ready = mode == ledcast::BootMode::Normal &&
        ledcast::linear_configure(config.line) == ESP_OK &&
        log_result("HUB75", ledcast::display_init(config.line.module, config.line.module_count,
                                                   config.display)) == ESP_OK;
    s_output_ready.store(output_ready, std::memory_order_release);
    if (output_ready) log_result("show", ledcast::show_start(config.show));
    if (power_cycles != 0 && !standalone) ledcast::power_cycle_window_start(power_cycles, output_ready);
    if (standalone && output_ready && !config.show.enabled) ledcast::power_cycle_standalone_mark();
    if (standalone) ledcast::net_force_setup_ap();
    // DMA must be reserved before Wi-Fi starts, but lwIP must be initialized
    // before the stream receiver opens its UDP socket.
    ledcast::net_set_interface_changed_handler(on_network_interface_changed);
    const esp_err_t network = log_result("network", ledcast::net_start(config.wifi));
    const esp_err_t web = log_result("web UI", ledcast::webui_start());
    if (network == ESP_OK && web == ESP_OK) {
        s_services_ready.store(true, std::memory_order_release);
        if (output_ready && !config.show.enabled) log_result("stream", ledcast::stream_start(config.stream, on_stream_timeout));
    }
    log_result("configuration applier", ledcast::config_set_applier(apply_config));
    ledcast::webui_set_ota_handlers(on_ota_begin, on_ota_end);
    ledcast::health_button_start(on_button);
    s_applied = config;
    s_applied_valid = true;
    ledcast::health_mark_running();
    ledcast::health_led_set(mode == ledcast::BootMode::Safe ? ledcast::LedPattern::Error : ledcast::LedPattern::Running);
    ESP_LOGI(kTag, "LEDCAST Node ready: %ux%u", ledcast::linear_width(), ledcast::linear_height());
}
