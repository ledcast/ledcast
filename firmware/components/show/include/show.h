#pragma once

#include <esp_err.h>

#include "deviceconfig.h"

namespace ledcast {

esp_err_t show_start(const ShowConfig& config);
esp_err_t show_reconfigure(const ShowConfig& config);
bool show_active();
void show_pause_for_test(bool paused);
void show_redraw();

// Есть что показывать: хотя бы один знак, кроме пробелов и переводов строк.
inline bool show_has_text(const ShowConfig& config) {
    for (const char* p = config.text; *p != '\0'; ++p)
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return true;
    return false;
}

}  // namespace ledcast
