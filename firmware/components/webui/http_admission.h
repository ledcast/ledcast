#pragma once

#include <cstdint>
#include "../deviceconfig/include/runtime_heap_budget.h"

namespace ledcast::webui {

enum class HttpAdmission : uint8_t { Accept, Reject };

constexpr uint32_t kMinHttpAdmissionFreeHeapBytes = runtime_memory::kServiceHeadroom;

constexpr HttpAdmission http_admission(uint32_t free_heap_bytes) {
    return free_heap_bytes < kMinHttpAdmissionFreeHeapBytes ? HttpAdmission::Reject
                                                            : HttpAdmission::Accept;
}

}  // namespace ledcast::webui
