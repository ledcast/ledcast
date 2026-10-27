#pragma once

#include <esp_err.h>

// LEDCAST Node HTTP API v1 and embedded setup UI.
// Public reads: status, hardware, system, Wi-Fi scan/profiles.
// Mutations are password/bearer protected on LAN and directly available only
// when the request was addressed to the controller's own AP interface.
namespace ledcast {
esp_err_t webui_start();
esp_err_t webui_rebind();
esp_err_t webui_redraw_test();
const char* webui_test_pattern();
// Загрузка прошивки останавливает видеопоток и шоу, а строка показывает ход
// загрузки. begin вызывается до записи, end — только если загрузка сорвалась:
// удачная заканчивается перезагрузкой.
void webui_set_ota_handlers(void (*begin)(), void (*end)());
}  // namespace ledcast
