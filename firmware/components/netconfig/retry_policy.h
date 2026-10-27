#pragma once

#include <cstdint>

namespace ledcast::netconfig {

enum class RetryAction : uint8_t { Connect, RestartRadio };

struct RetryPlan {
    RetryAction action;
    uint32_t delay_seconds;
    uint8_t next_backoff_index;
};

constexpr RetryPlan retry_plan(uint8_t backoff_index, uint8_t connect_failures) {
    constexpr uint8_t kLastBackoffIndex = 6;
    constexpr uint8_t kRestartAfterFailures = 5;
    constexpr uint32_t kBackoffSeconds[] = {1, 2, 5, 10, 20, 30, 60};
    const uint8_t index = backoff_index > kLastBackoffIndex ? kLastBackoffIndex : backoff_index;
    return {
        connect_failures >= kRestartAfterFailures ? RetryAction::RestartRadio : RetryAction::Connect,
        kBackoffSeconds[index],
        index < kLastBackoffIndex ? static_cast<uint8_t>(index + 1) : index,
    };
}

}  // namespace ledcast::netconfig
