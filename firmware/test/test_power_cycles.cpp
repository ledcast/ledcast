#include "../components/health/include/power_cycles.h"
#include <cassert>
int main() {
    using namespace ledcast::power_cycles;
    // Включения питания идут по счёту, третье — автономный запуск.
    assert(count(true, 0) == 1);
    assert(count(true, 1) == 2);
    assert(count(true, 2) == kForStandalone);
    // Любой другой сброс обрывает серию.
    assert(count(false, 2) == 0);
    // Мусор в NVS не выводит счёт за пределы серии.
    assert(count(true, 200) >= 1 && count(true, 200) <= kForStandalone);
    // До конца окна хранится счёт; автономный запуск серию закрывает.
    assert(stored_after(1) == 1);
    assert(stored_after(2) == 2);
    assert(stored_after(kForStandalone) == 0);
    assert(stored_after(0) == 0);
}
