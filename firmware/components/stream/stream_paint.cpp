#include "stream_paint.h"
#include "linear.h"

namespace ledcast::stream {
Canvas paint_canvas() { return {linear_width(), linear_height()}; }
void paint_span(uint16_t x, uint16_t y, uint16_t count, const uint8_t* rgb) {
    linear_paint(x, y, count, rgb);
}
}  // namespace ledcast::stream
