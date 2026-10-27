#include <cstring>

#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "deviceconfig.h"
#include "webui.h"
#include "webui_internal.h"

namespace ledcast::webui {
namespace {

constexpr char kTag[] = "webui.ota";

// Заголовок образа ESP-IDF: байт 0 — магия, байты 12..13 — идентификатор чипа.
constexpr uint8_t kImageMagic = 0xE9;
constexpr size_t kHeaderCheckBytes = 16;
constexpr size_t kChipIdOffset = 12;

void (*s_ota_begin)() = nullptr;
void (*s_ota_end)() = nullptr;

// Пока идёт загрузка, строка показывает её ход; сорвалась — возвращается к
// тому, что показывала.
class OtaScreen {
public:
    OtaScreen() {
        if (s_ota_begin != nullptr) s_ota_begin();
        paint_ota_progress(0);
    }
    ~OtaScreen() {
        if (!done_ && s_ota_end != nullptr) s_ota_end();
    }
    void progress(size_t received, size_t total) {
        const uint8_t percent = static_cast<uint8_t>(uint64_t(received) * 100u / total);
        if (percent == shown_) return;
        shown_ = percent;
        paint_ota_progress(percent);
    }
    void done() {
        done_ = true;
        paint_ota_progress(100);
    }

private:
    uint8_t shown_ = 0;
    bool done_ = false;
};

SemaphoreHandle_t ota_lock() {
    static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    return lock;
}

bool header_looks_like_firmware(const uint8_t* bytes) {
    if (bytes[0] != kImageMagic) {
        return false;
    }
    const uint16_t chip_id = static_cast<uint16_t>(bytes[kChipIdOffset]) |
                             static_cast<uint16_t>(bytes[kChipIdOffset + 1] << 8);
    return chip_id == ESP_CHIP_ID_ESP32S3;
}

}  // namespace

esp_err_t handle_ota(httpd_req_t* req) {
    const size_t total = static_cast<size_t>(req->content_len);
    if (total < kHeaderCheckBytes) {
        return send_json_error(req, "400 Bad Request", "Образ прошивки пустой или обрезанный");
    }

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        return send_json_error(req, "500 Internal Server Error", "Нет свободного раздела под OTA");
    }
    if (total > target->size) {
        return send_json_error(req, "413 Payload Too Large", "Образ больше раздела прошивки");
    }

    SemaphoreHandle_t lock = ota_lock();
    if (lock == nullptr || xSemaphoreTake(lock, 0) != pdTRUE) {
        return send_json_error(req, "409 Conflict", "Обновление уже идёт");
    }

    OtaScreen screen;
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, total, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return send_json_result(req, err, "Не открыл раздел под запись");
    }

    char chunk[kIoChunk];
    size_t received = 0;
    uint8_t header[kHeaderCheckBytes];
    size_t header_len = 0;

    while (received < total) {
        const size_t want = total - received < sizeof(chunk) ? total - received : sizeof(chunk);
        const int n = recv_body_chunk(req, chunk, want);
        if (n <= 0) {
            // Обрыв на середине не страшен: писали в неактивный раздел, загрузчик
            // по-прежнему стартует текущую прошивку.
            esp_ota_abort(handle);
            xSemaphoreGive(lock);
            if (n == kRecvTimeout) {
                return send_json_error(req, "408 Request Timeout",
                                       "Телефон замолчал, прошивка не заменена");
            }
            return send_json_error(req, "400 Bad Request", "Связь оборвалась, прошивка не заменена");
        }

        // На слабом Wi-Fi первая порция бывает короче заголовка, поэтому он
        // копится, а не проверяется по одному recv.
        if (header_len < kHeaderCheckBytes) {
            const size_t free_space = kHeaderCheckBytes - header_len;
            const size_t take = static_cast<size_t>(n) < free_space ? static_cast<size_t>(n)
                                                                    : free_space;
            memcpy(header + header_len, chunk, take);
            header_len += take;
            if (header_len == kHeaderCheckBytes && !header_looks_like_firmware(header)) {
                esp_ota_abort(handle);
                xSemaphoreGive(lock);
                return send_json_error(req, "400 Bad Request",
                                       "Это не образ прошивки для ESP32-S3");
            }
        }

        err = esp_ota_write(handle, chunk, static_cast<size_t>(n));
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            xSemaphoreGive(lock);
            return send_json_result(req, err, "Ошибка записи прошивки");
        }
        received += static_cast<size_t>(n);
        screen.progress(received, total);
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return send_json_result(req, err, "Образ не прошёл проверку");
    }

    err = config_flush();
    if (err != ESP_OK) {
        xSemaphoreGive(lock);
        return send_json_result(req, err, "Не сохранил состояние перед обновлением");
    }

    err = esp_ota_set_boot_partition(target);
    xSemaphoreGive(lock);
    if (err != ESP_OK) {
        return send_json_result(req, err, "Не переключил загрузочный раздел");
    }
    screen.done();

    // Запас стека печатается именно здесь: заливка — самый глубокий путь
    // обработчика, и переполнение стоило бы платы, до которой не добраться.
    ESP_LOGI(kTag, "прошивка записана в %s, %u байт, запас стека %u байт", target->label,
             static_cast<unsigned>(received),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    err = schedule_restart(1200);
    if (err != ESP_OK) {
        return send_json_result(req, err, "Не назначил перезагрузку после обновления");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, "{\"ok\":true,\"restarting\":true}", HTTPD_RESP_USE_STRLEN);
}

}  // namespace ledcast::webui

namespace ledcast {

void webui_set_ota_handlers(void (*begin)(), void (*end)()) {
    webui::s_ota_begin = begin;
    webui::s_ota_end = end;
}

}  // namespace ledcast
