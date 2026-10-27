#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "deviceconfig.h"
#include "webui_internal.h"

namespace ledcast::webui {
namespace {

constexpr char kTag[] = "webui";

esp_err_t http_chunk_flush(void* ctx, const char* data, size_t len) {
    auto* req = static_cast<httpd_req_t*>(ctx);
    return httpd_resp_send_chunk(req, data, static_cast<ssize_t>(len));
}

void restart_timer_cb(void*) { esp_restart(); }

}  // namespace

JsonSink make_http_sink(httpd_req_t* req, char* buffer, size_t capacity) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return JsonSink(buffer, capacity, http_chunk_flush, req);
}

esp_err_t finish_http_sink(httpd_req_t* req, JsonSink& sink) {
    const esp_err_t err = sink.finish();
    httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

esp_err_t send_json_error(httpd_req_t* req, const char* status, const char* message) {
    char buf[320];
    JsonSink json(buf, sizeof(buf), nullptr, nullptr);
    json.object_open();
    json.boolean("ok", false);
    json.string("error", message);
    json.object_close();
    json.finish();

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json.data(), static_cast<ssize_t>(json.length()));
}

esp_err_t send_json_ok(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t send_json_result(httpd_req_t* req, esp_err_t err, const char* context) {
    if (err == ESP_OK) {
        return send_json_ok(req);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_error(req, "409 Conflict",
                               "Состояние устройства изменилось. Обновите настройки и повторите действие.");
    }
    char message[160];
    snprintf(message, sizeof(message), "%s: %s", context, esp_err_to_name(err));
    ESP_LOGW(kTag, "%s", message);
    return send_json_error(req, "500 Internal Server Error", message);
}

int recv_body_chunk(httpd_req_t* req, char* out, size_t want) {
    uint8_t idle = 0;
    while (true) {
        const int n = httpd_req_recv(req, out, want);
        if (n > 0) {
            return n;
        }
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return kRecvClosed;
        }
        // Телефон ушёл из зоны: TCP не рвётся, сокет живой, данных нет. Без
        // счётчика приём крутился бы вечно и держал бы занятыми и обработчик,
        // и мьютекс обновления.
        if (++idle >= kRecvIdleAttempts) {
            return kRecvTimeout;
        }
    }
}

char* read_json_body(httpd_req_t* req, size_t* out_len) {
    const size_t total = static_cast<size_t>(req->content_len);
    if (total == 0) {
        send_json_error(req, "400 Bad Request", "Пустое тело запроса");
        return nullptr;
    }
    if (total > kMaxJsonBody) {
        send_json_error(req, "413 Payload Too Large", "Документ слишком большой");
        return nullptr;
    }

    char* buf = static_cast<char*>(malloc(total + 1));
    if (buf == nullptr) {
        send_json_error(req, "503 Service Unavailable", "Не хватило памяти под запрос");
        return nullptr;
    }

    size_t received = 0;
    while (received < total) {
        const int n = recv_body_chunk(req, buf + received, total - received);
        if (n <= 0) {
            free(buf);
            if (n == kRecvTimeout) {
                send_json_error(req, "408 Request Timeout", "Тело запроса так и не пришло");
            } else {
                send_json_error(req, "400 Bad Request", "Соединение оборвалось при приёме тела");
            }
            return nullptr;
        }
        received += static_cast<size_t>(n);
    }
    buf[total] = '\0';
    if (out_len != nullptr) {
        *out_len = total;
    }
    return buf;
}

esp_err_t schedule_restart(uint32_t delay_ms) {
    // Ответ должен успеть уйти клиенту, иначе интерфейс покажет обрыв вместо
    // сообщения о перезагрузке.
    static esp_timer_handle_t timer = nullptr;
    esp_err_t err = config_flush();
    if (err != ESP_OK) {
        return err;
    }
    if (timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = restart_timer_cb;
        args.name = "webui_restart";
        err = esp_timer_create(&args, &timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    return esp_timer_start_once(timer, static_cast<uint64_t>(delay_ms) * 1000);
}

}  // namespace ledcast::webui
