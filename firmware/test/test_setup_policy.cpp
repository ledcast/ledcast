#include "../components/netconfig/setup_policy.h"
#include <cassert>
int main() {
    using namespace ledcast::netconfig;
    assert(!may_retry(false, false, false, 1, 70000000, 5000000));
    assert(!may_retry(false, false, true, 0, 70000000, 5000000));
    assert(!may_retry(false, true, false, 0, 70000000, 5000000));
    assert(!may_retry(true, false, false, 0, 70000000, 5000000));
    assert(!may_retry(false, false, false, 0, 4999999, 5000000));
    assert(!may_retry(false, false, false, 0, 70000000, 0));
    assert(may_retry(false, false, false, 0, 5000000, 5000000));
    assert(retry_delay_us(1) == 15000000);
    assert(retry_delay_us(2) == 30000000);
    assert(retry_delay_us(9999) == 60000000);
}
