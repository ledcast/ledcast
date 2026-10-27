#pragma once

#include <cstdint>
#include <esp_err.h>
#include "deviceconfig.h"

namespace ledcast {

struct LineCoord { uint16_t x; uint16_t y; bool valid; };

esp_err_t linear_configure(const LineConfig& config);
uint16_t linear_width();
uint16_t linear_module_width();
uint16_t linear_height();
LineCoord linear_to_chain(uint16_t x, uint16_t y);
void linear_paint(uint16_t x, uint16_t y, uint16_t count, const uint8_t* rgb);
void linear_blackout();

}  // namespace ledcast
