#include <cstdlib>
#include <iostream>

#include "retry_policy.h"

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using ledcast::netconfig::RetryAction;
    using ledcast::netconfig::retry_plan;

    const auto reconnect = retry_plan(0, 0);
    expect(reconnect.action == RetryAction::Connect, "first retry must connect");
    expect(reconnect.delay_seconds == 1, "first retry must wait one second");
    expect(reconnect.next_backoff_index == 1, "backoff must advance after retry");

    const auto capped = retry_plan(6, 4);
    expect(capped.action == RetryAction::Connect, "four failures must still connect");
    expect(capped.delay_seconds == 60, "backoff must cap at one minute");
    expect(capped.next_backoff_index == 6, "capped backoff must stay capped");

    const auto restart = retry_plan(6, 5);
    expect(restart.action == RetryAction::RestartRadio,
           "five failed connects must restart Wi-Fi instead of connecting again");
    expect(restart.delay_seconds == 60, "restart must keep periodic recovery armed");
    return 0;
}
