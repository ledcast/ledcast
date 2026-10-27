#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>
#include <esp_http_server.h>

#include "http_admission.h"
#include "webui_json.h"

namespace ledcast::webui {

// Собранный интерфейс встроен в образ одним gzip-файлом.

// Размер порции при потоковом чтении тела запроса и при отдаче файлов.
inline constexpr size_t kIoChunk = 1460;
// Потолок на JSON-тело запроса: импорт конфигурации — самый крупный документ.
inline constexpr size_t kMaxJsonBody = 12 * 1024;
inline constexpr size_t kJsonSinkBuffer = 512;
// Статус — самый крупный и самый частый документ (около 1,9 КБ). При буфере в
// 512 байт он уходил четырьмя чанками, а httpd на каждый чанк делает три
// отдельных send (длина, данные, перевод строки): пятнадцать записей в сокет
// вместо трёх, и на каждой lwip берёт из кучи свой pbuf. Буфер лежит на стеке
// задачи httpd (8192, свободно 4408 в покое) — эта память уже выделена, в
// отличие от буферов lwip.
inline constexpr size_t kStatusSinkBuffer = 2048;

// Ниже этого порога свободной кучи сокет отклоняется до заголовков, а запрос,
// принятый до просадки, получает 503 до обработчика.
//
// Считано так: одно соединение в работе стоит до SND_BUF (2880) на очередь
// отправки плюс килобайт scratch-буфера заголовков плюс окно приёма — около
// 4 КБ. Всплеск приёма UDP при старте видеопотока занимает до глубины очереди
// (16) на буфер Wi-Fi, это ещё около 26 КБ. Порог обязан лежать выше приёмочных
// 16 КБ с запасом на оба слагаемых, иначе гарантия «не ниже 16 КБ» держится на
// удаче. Отказ стоит одного короткого ответа и закрытия сокета — дешевле, чем
// принятый запрос, который доест остаток.
inline constexpr size_t kMaxRequestHeaderBytes = 2048;

// Сколько ждать очередную порцию тела. Загрузка идёт медленнее обычного запроса,
// но замолчавший клиент не должен держать обработчик дольше минуты.
inline constexpr int kRecvWaitTimeoutSec = 5;
inline constexpr int kSendWaitTimeoutSec = 5;
inline constexpr uint8_t kRecvIdleAttempts = 4;

// --- ответы -----------------------------------------------------------------

esp_err_t send_json_error(httpd_req_t* req, const char* status, const char* message);
esp_err_t send_json_ok(httpd_req_t* req);
esp_err_t send_json_result(httpd_req_t* req, esp_err_t err, const char* context);

// Sink, пишущий прямо в HTTP-чанки. Завершать httpd_resp_send_chunk(req, nullptr, 0).
JsonSink make_http_sink(httpd_req_t* req, char* buffer, size_t capacity);
esp_err_t finish_http_sink(httpd_req_t* req, JsonSink& sink);

// --- тело запроса -----------------------------------------------------------

// Коды recv_body_chunk вместо числа принятых байт.
inline constexpr int kRecvClosed = -1;
inline constexpr int kRecvTimeout = -2;

// Одна порция тела запроса. Молчание клиента дольше
// kRecvWaitTimeoutSec * kRecvIdleAttempts даёт kRecvTimeout, а не вечный цикл.
int recv_body_chunk(httpd_req_t* req, char* out, size_t want);

// Читает тело целиком в буфер из кучи (нужен cJSON). При ошибке сам отвечает
// клиенту и возвращает nullptr. Освобождать через free().
char* read_json_body(httpd_req_t* req, size_t* out_len);

// --- служебное --------------------------------------------------------------

esp_err_t schedule_restart(uint32_t delay_ms);

// Id строки из /api/v1/status (MAC станции в hex); из него же выводится ключ Console.
void device_id(char out[18]);

// --- обработчики ------------------------------------------------------------

esp_err_t handle_status(httpd_req_t* req);
esp_err_t handle_hardware_get(httpd_req_t* req);
esp_err_t handle_hardware_put(httpd_req_t* req);
esp_err_t handle_brightness_put(httpd_req_t* req);
esp_err_t handle_system_get(httpd_req_t* req);
esp_err_t handle_system_put(httpd_req_t* req);
esp_err_t handle_factory_reset(httpd_req_t* req);
esp_err_t handle_reboot(httpd_req_t* req);
esp_err_t handle_wifi_scan(httpd_req_t* req);
esp_err_t handle_wifi_profiles_get(httpd_req_t* req);
esp_err_t handle_wifi_profiles_post(httpd_req_t* req);
esp_err_t handle_wifi_profile_delete(httpd_req_t* req);
esp_err_t handle_wifi_profiles_delete(httpd_req_t* req);
esp_err_t handle_wifi_profile_connect(httpd_req_t* req);
esp_err_t handle_test(httpd_req_t* req);
esp_err_t handle_show_get(httpd_req_t* req);
esp_err_t handle_show_put(httpd_req_t* req);
esp_err_t handle_show_enabled_put(httpd_req_t* req);
esp_err_t handle_ota(httpd_req_t* req);
// Экран загрузки прошивки на всю строку (ota_screen.cpp).
esp_err_t paint_ota_progress(uint8_t percent);

// Общий сборщик статуса: один и тот же документ уходит и в REST, и в WebSocket.
void write_status(JsonSink& json);

}  // namespace ledcast::webui
