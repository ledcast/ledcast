#include <cstdlib>
#include <iostream>

#include "http_admission.h"

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    using ledcast::webui::HttpAdmission;
    using ledcast::webui::http_admission;

    expect(http_admission(24 * 1024 - 1) == HttpAdmission::Reject,
           "socket must be rejected below protected heap reserve");
    expect(http_admission(24 * 1024) == HttpAdmission::Accept,
           "socket must be accepted at protected heap reserve");
    return 0;
}
