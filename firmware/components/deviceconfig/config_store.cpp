#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>

#include "deviceconfig.h"
#include "config_domains.h"
#include "config_flush_queue.h"
#include "config_serialize.h"
#include "config_snapshot.h"

// Хранение конфигурации в NVS: по ключу на домен (см. config_domains.h).
//
// Ресурс flash конечен, а веб-интерфейс сохраняет настройки щедро, поэтому
// перед записью текст домена сравнивается с уже лежащим и совпадение не
// пишется: ползунок яркости переписывает только домен вывода.

namespace ledcast {
namespace {

constexpr char kTag[] = "config";
constexpr char kNamespace[] = "ledcast";
// Прошивки до разделения на домены хранили всё одним документом под этим
// ключом. Он читается один раз — ради переноса сети — и стирается.
constexpr char kLegacyKey[] = "config";
// Самый длинный домен — показ: 1 КБ текста, в JSON ещё и экранированный.
constexpr size_t kJsonCapacity = 8192;

struct DomainInfo {
    const char* key;   // ключ NVS, не длиннее 15 символов; сменить — потерять домен
    const char* name;  // для журнала
};

DomainInfo info(ConfigDomain domain) {
    switch (domain) {
        case ConfigDomain::Net: return {"net", "сеть"};
        case ConfigDomain::Security: return {"security", "доступ"};
        case ConfigDomain::Line: return {"line", "строка"};
        case ConfigDomain::Output: return {"output", "вывод"};
        case ConfigDomain::Show: return {"show", "показ"};
        case ConfigDomain::Stream: return {"stream", "поток"};
    }
    return {"", ""};
}

class Nvs {
  public:
    esp_err_t open(nvs_open_mode_t mode) {
        const esp_err_t err = nvs_open(kNamespace, mode, &handle_);
        open_ = err == ESP_OK;
        return err;
    }
    ~Nvs() { if (open_) nvs_close(handle_); }
    nvs_handle_t get() const { return handle_; }

  private:
    nvs_handle_t handle_ = 0;
    bool open_ = false;
};

// Readers retain immutable revisions. Only the handle exchange is protected;
// applying settings and freeing old revisions happen outside this short lock.
struct Store {
    ConfigSnapshot current = config_snapshot_defaults();
    portMUX_TYPE snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
    SemaphoreHandle_t writer_lock = xSemaphoreCreateMutex();
    ConfigApplier applier = nullptr;

    ConfigSnapshot read() {
        portENTER_CRITICAL(&snapshot_lock);
        ConfigSnapshot result = current;
        portEXIT_CRITICAL(&snapshot_lock);
        return result;
    }

    void publish(ConfigSnapshot next) {
        portENTER_CRITICAL(&snapshot_lock);
        current.swap(next);
        portEXIT_CRITICAL(&snapshot_lock);
    }
};

class StoreLock {
  public:
    explicit StoreLock(Store& store) : handle_(store.writer_lock) {
        locked_ = handle_ != nullptr && xSemaphoreTake(handle_, portMAX_DELAY) == pdTRUE;
    }

    ~StoreLock() {
        if (locked_) {
            xSemaphoreGive(handle_);
        }
    }

    bool locked() const { return locked_; }

  private:
    SemaphoreHandle_t handle_;
    bool locked_ = false;
};

Store& store() {
    static Store instance;
    return instance;
}

esp_err_t read_key(nvs_handle_t handle, const char* key, std::unique_ptr<char[]>& text,
                   size_t& length) {
    size_t size = 0;
    esp_err_t err = nvs_get_blob(handle, key, nullptr, &size);
    if (err != ESP_OK) return err;
    if (size == 0 || size > kJsonCapacity) return ESP_ERR_INVALID_SIZE;
    text.reset(new (std::nothrow) char[size + 1]);
    if (!text) return ESP_ERR_NO_MEM;
    err = nvs_get_blob(handle, key, text.get(), &size);
    if (err != ESP_OK) return err;
    text[size] = '\0';
    length = size;
    return ESP_OK;
}

struct DomainText {
    std::unique_ptr<char[]> text;
    size_t length = 0;
};

// buffer — черновик на kJsonCapacity; в DomainText остаётся только нужное.
esp_err_t serialize(ConfigDomain domain, const DeviceConfig& cfg, char* buffer, DomainText& out) {
    const int length = config_domain_to_json(domain, cfg, buffer, kJsonCapacity);
    if (length < 0) {
        ESP_LOGE(kTag, "%s: не помещается в %u байт", info(domain).name,
                 static_cast<unsigned>(kJsonCapacity));
        return ESP_ERR_NO_MEM;
    }
    out.text.reset(new (std::nothrow) char[static_cast<size_t>(length) + 1]);
    if (!out.text) return ESP_ERR_NO_MEM;
    std::memcpy(out.text.get(), buffer, static_cast<size_t>(length) + 1);
    out.length = static_cast<size_t>(length);
    return ESP_OK;
}

esp_err_t write_key(nvs_handle_t handle, const char* key, const DomainText& text) {
    esp_err_t err = nvs_set_blob(handle, key, text.text.get(), text.length);
    if (err == ESP_OK) err = nvs_commit(handle);
    return err;
}

void load_domains(nvs_handle_t handle, DeviceConfig& cfg, DomainLoads& loads) {
    for (const ConfigDomain domain : kConfigDomains) {
        DomainLoad& load = loads[config_domain_index(domain)];
        std::unique_ptr<char[]> text;
        size_t length = 0;
        const esp_err_t err = read_key(handle, info(domain).key, text, length);
        if (err == ESP_ERR_NVS_NOT_FOUND) continue;
        if (err == ESP_OK && config_domain_from_json(domain, text.get(), length, cfg) == ESP_OK) {
            load.status = DomainStatus::Loaded;
            continue;
        }
        config_reset_domain(domain, cfg);
        load.status = DomainStatus::Unreadable;
        snprintf(load.reason, sizeof(load.reason), "%s",
                 err == ESP_OK ? "not a JSON object" : esp_err_to_name(err));
    }
}

// Разовый перенос со старого общего документа: только сеть и доступ к вебу,
// без них плату на площадке не найти. Остальное старое отбрасывается.
// true — старый документ есть, и после проверки его надо дописать и стереть.
bool import_legacy(nvs_handle_t handle, DeviceConfig& cfg, DomainLoads& loads) {
    std::unique_ptr<char[]> text;
    size_t length = 0;
    if (read_key(handle, kLegacyKey, text, length) != ESP_OK) return false;
    std::unique_ptr<DeviceConfig> legacy(new (std::nothrow) DeviceConfig{});
    if (!legacy) return false;
    if (config_legacy_from_json(text.get(), length, *legacy) == ESP_OK) {
        config_import_legacy(*legacy, cfg, loads);
    } else {
        ESP_LOGW(kTag, "старый документ настроек не разбирается, переносить нечего");
    }
    return true;
}

// Перенесённое (уже проверенное) ложится своими доменами, и лишь затем старый
// ключ стирается: пропадёт питание посередине — перенос просто повторится.
void finish_legacy(nvs_handle_t handle, const DeviceConfig& cfg) {
    std::unique_ptr<char[]> buffer(new (std::nothrow) char[kJsonCapacity]);
    if (!buffer) return;
    for (const ConfigDomain domain : {ConfigDomain::Net, ConfigDomain::Security}) {
        DomainText out;
        esp_err_t err = serialize(domain, cfg, buffer.get(), out);
        if (err == ESP_OK) err = write_key(handle, info(domain).key, out);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "%s: перенос не записался (%s), старый документ оставлен",
                     info(domain).name, esp_err_to_name(err));
            return;
        }
    }
    esp_err_t err = nvs_erase_key(handle, kLegacyKey);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "старый документ настроек не стёрся: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGW(kTag, "старый документ настроек стёрт: сеть и доступ перенесены, остальное заводское");
}

void log_load(ConfigDomain domain, const DomainLoad& load) {
    const char* name = info(domain).name;
    switch (load.status) {
        case DomainStatus::Loaded: break;
        case DomainStatus::Missing:
            ESP_LOGI(kTag, "%s: в NVS нет, заводские настройки", name);
            break;
        case DomainStatus::Imported:
            ESP_LOGW(kTag, "%s: перенесено из старого документа", name);
            break;
        case DomainStatus::Unreadable:
            ESP_LOGW(kTag, "%s: не читается (%s), заводские настройки", name, load.reason);
            break;
        case DomainStatus::Invalid:
            ESP_LOGW(kTag, "%s: не проходит проверку (%s), заводские настройки", name, load.reason);
            break;
        case DomainStatus::Conflict:
            ESP_LOGW(kTag, "%s: не сходится с другими доменами (%s), уступил", name, load.reason);
            break;
    }
}

}  // namespace

esp_err_t config_init() {
    Store& s = store();
    s.publish(config_snapshot_defaults());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    // Неисправное хранилище не повод не подняться: без веба и обновления по
    // воздуху до платы внутри конструкции уже не добраться, а заводских
    // настроек хватает, чтобы поднять точку доступа и починиться.
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "NVS недоступна (%s), работаем на заводской конфигурации",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    // Каждый домен читается и проверяется сам: негодный вывод или строка
    // откатываются к заводским поодиночке, сеть при этом остаётся.
    DeviceConfig loaded = config_defaults();
    DomainLoads loads{};
    Nvs nvs;
    bool legacy = false;
    err = nvs.open(NVS_READWRITE);
    if (err == ESP_OK) {
        load_domains(nvs.get(), loaded, loads);
        legacy = import_legacy(nvs.get(), loaded, loads);
    } else {
        ESP_LOGE(kTag, "настройки в NVS не открылись (%s), работаем на заводских",
                 esp_err_to_name(err));
    }
    config_settle(loaded, loads);
    if (legacy) finish_legacy(nvs.get(), loaded);
    for (const ConfigDomain domain : kConfigDomains) {
        log_load(domain, loads[config_domain_index(domain)]);
    }

    auto restored = config_snapshot_create(loaded);
    if (!restored) return ESP_ERR_NO_MEM;
    s.publish(std::move(restored));
    ESP_LOGI(kTag, "конфигурация LEDCAST загружена: %u модулей",
             static_cast<unsigned>(loaded.line.module_count));
    return ESP_OK;
}

ConfigSnapshot config_get() {
    return store().read();
}

// Настройки применяются мгновенно, а во флеш уходят с задержкой.
//
// Человек подбирает яркость ползунком и ждёт, что строка отзовётся сразу. Но
// каждая правка — это запись во флеш-память, а её ресурс конечен: сотня
// движений ползунка за минуту стёрла бы сектор впустую. Поэтому применение и
// сохранение разведены: живая конфигурация обновляется на месте, а запись
// откладывается до паузы в правках.
namespace {

constexpr uint64_t kFlushDelayUs = 3ULL * 1000000ULL;

esp_timer_handle_t g_flush_timer = nullptr;
// Один замок на очередь и запись: сравнение с лежащим во флеше и сама запись
// не перемежаются, иначе отменённая правка могла бы лечь поверх живой.
SemaphoreHandle_t g_flush_lock = nullptr;
ConfigFlushQueue g_pending[kConfigDomainCount];

esp_err_t flush_pending() {
    if (g_flush_lock == nullptr || xSemaphoreTake(g_flush_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_OK;
    }
    bool pending = false;
    for (const ConfigFlushQueue& queue : g_pending) pending = pending || !queue.empty();
    esp_err_t result = ESP_OK;
    Nvs nvs;
    if (pending && (result = nvs.open(NVS_READWRITE)) != ESP_OK) {
        ESP_LOGE(kTag, "настройки в NVS не открылись: %s", esp_err_to_name(result));
        pending = false;
    }
    for (const ConfigDomain domain : kConfigDomains) {
        if (!pending) break;
        ConfigFlushQueue& queue = g_pending[config_domain_index(domain)];
        DomainText text;
        if (!queue.take(text.text, text.length)) continue;
        const esp_err_t err = write_key(nvs.get(), info(domain).key, text);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "%s: не сохранилось: %s", info(domain).name, esp_err_to_name(err));
            queue.restore(std::move(text.text), text.length);
            result = err;
            continue;
        }
        ESP_LOGI(kTag, "%s: сохранено (%u байт)", info(domain).name,
                 static_cast<unsigned>(text.length));
    }
    xSemaphoreGive(g_flush_lock);
    return result;
}

void flush_timer_cb(void*) {
    const esp_err_t err = flush_pending();
    if (err != ESP_OK && g_flush_timer != nullptr) {
        esp_timer_start_once(g_flush_timer, kFlushDelayUs);
    }
}

esp_err_t prepare_flush() {
    if (g_flush_lock == nullptr) g_flush_lock = xSemaphoreCreateMutex();
    if (g_flush_lock == nullptr) return ESP_ERR_NO_MEM;
    if (g_flush_timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = flush_timer_cb;
        args.name = "config_flush";
        const esp_err_t error = esp_timer_create(&args, &g_flush_timer);
        if (error != ESP_OK) {
            g_flush_timer = nullptr;
            return error;
        }
    }
    return ESP_OK;
}

// Домен, чей текст совпал с лежащим во флеше, в очередь не встаёт.
void schedule_flush(DomainText (&texts)[kConfigDomainCount]) {
    if (xSemaphoreTake(g_flush_lock, portMAX_DELAY) != pdTRUE) return;
    Nvs nvs;
    const bool opened = nvs.open(NVS_READONLY) == ESP_OK;
    for (const ConfigDomain domain : kConfigDomains) {
        const size_t index = config_domain_index(domain);
        std::unique_ptr<char[]> stored;
        size_t stored_length = 0;
        const bool have_stored =
            opened && read_key(nvs.get(), info(domain).key, stored, stored_length) == ESP_OK;
        g_pending[index].push(std::move(texts[index].text), texts[index].length,
                              have_stored ? stored.get() : nullptr, stored_length);
    }
    xSemaphoreGive(g_flush_lock);
    esp_timer_stop(g_flush_timer);
    esp_timer_start_once(g_flush_timer, kFlushDelayUs);
}

}  // namespace

esp_err_t config_flush() {
    if (g_flush_timer != nullptr) {
        esp_timer_stop(g_flush_timer);
    }
    return flush_pending();
}

esp_err_t config_set(const DeviceConfig& cfg, const ConfigSnapshot& expected) {
    StoreLock lock(store());
    if (!lock.locked()) {
        return ESP_ERR_NO_MEM;
    }
    if (!(store().read() == expected)) return ESP_ERR_INVALID_STATE;

    char reason[128];
    esp_err_t err = config_validate(cfg, reason, sizeof(reason));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "конфигурация отклонена: %s", reason);
        return err;
    }
    // Fail before publishing/applying if persistence resources are unavailable.
    // Previously this failure was silently reported as a successful save.
    err = prepare_flush();
    if (err != ESP_OK) return err;

    // Do not retain the maximum serialization buffer while rebuilding video
    // buffers: each domain keeps only its own compact text.
    DomainText texts[kConfigDomainCount];
    {
        std::unique_ptr<char[]> buffer(new (std::nothrow) char[kJsonCapacity]);
        if (!buffer) return ESP_ERR_NO_MEM;
        for (const ConfigDomain domain : kConfigDomains) {
            err = serialize(domain, cfg, buffer.get(), texts[config_domain_index(domain)]);
            if (err != ESP_OK) return err;
        }
    }

    auto next = config_snapshot_create(cfg);
    if (!next) return ESP_ERR_NO_MEM;

    // Readers see only committed settings. A rejected hardware transition
    // must never escape through HTTP polling or be persisted to flash.
    if (store().applier != nullptr) {
        const esp_err_t err = store().applier(cfg);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "настройки не применились (%s), вернулись к прежним",
                     esp_err_to_name(err));
            return err;
        }
    }
    store().publish(std::move(next));

    schedule_flush(texts);
    return ESP_OK;
}

esp_err_t config_set_applier(ConfigApplier applier) {
    if (applier == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    store().applier = applier;
    return ESP_OK;
}

// Заводские значения пишутся тем же путём, что любая правка: применяются
// сразу и ложатся во флеш по доменам, сеть и доступ — тоже.
esp_err_t config_reset_factory() {
    const auto snapshot = config_get();
    return config_set(config_defaults(), snapshot);
}

}  // namespace ledcast
