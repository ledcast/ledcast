#include "check.h"
#include "linear.h"

namespace ledcast {
void display_write_run(uint16_t, uint16_t, uint16_t, int8_t, const uint8_t*) {}
void display_clear_run(uint16_t, uint16_t, uint16_t, int8_t) {}
esp_err_t display_commit() { return ESP_OK; }
}

using namespace ledcast;
int main() {
    LineConfig line{}; line.module_count=3; line.module.width=4; line.module.height=2;
    // Cards are visual left-to-right.  With the controller at the right edge,
    // physical module 1 is rightmost and the normal visual order is 3,2,1.
    line.modules[0].physical_module=2;
    line.modules[1].physical_module=1;
    line.modules[2].physical_module=0;
    CHECK(linear_configure(line)==ESP_OK);
    auto p=linear_to_chain(0,0); CHECK(p.valid); CHECK_EQ(p.x,0); CHECK_EQ(p.y,0);
    p=linear_to_chain(4,0); CHECK_EQ(p.x,4);
    p=linear_to_chain(8,0); CHECK_EQ(p.x,8);
    line.modules[0].rotate_180=true; CHECK(linear_configure(line)==ESP_OK);
    p=linear_to_chain(0,0); CHECK_EQ(p.x,3); CHECK_EQ(p.y,1);
    line.modules[0].mirror_x=true; CHECK(linear_configure(line)==ESP_OK);
    p=linear_to_chain(0,0); CHECK_EQ(p.x,0); CHECK_EQ(p.y,1);
    line.modules[0].mirror_y=true; CHECK(linear_configure(line)==ESP_OK);
    p=linear_to_chain(0,0); CHECK_EQ(p.x,0); CHECK_EQ(p.y,0);
    line.modules[2].physical_module=1; CHECK(linear_configure(line)==ESP_ERR_INVALID_ARG);
    return hosttest::report("linear");
}
