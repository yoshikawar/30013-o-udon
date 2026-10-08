#pragma once

#include "hexa_udon/protocol/api_client.hpp"

#include <chrono>
#include <cstddef>
#include <functional>

namespace hexa_udon::session {

struct PollingPolicy {
    std::chrono::milliseconds normal_interval{750};
    std::chrono::milliseconds initial_backoff{500};
    std::chrono::milliseconds maximum_backoff{4000};
    std::size_t maximum_attempts = 8;
};

class PollClock {
public:
    virtual ~PollClock() = default;
    [[nodiscard]] virtual protocol::SteadyTime now() const = 0;
    virtual void wait_until(protocol::SteadyTime time) = 0;
};

class SystemPollClock final : public PollClock {
public:
    [[nodiscard]] protocol::SteadyTime now() const override;
    void wait_until(protocol::SteadyTime time) override;
};

// Initial-setting helper only. Post-type waiting must use GET / via fetch_state.
[[nodiscard]] protocol::Result<core::MatchConfig> poll_initial_setting_until_ready(
    protocol::ProconApiClient& api, PollClock& clock, protocol::SteadyTime deadline,
    const PollingPolicy& policy = {});

[[nodiscard]] protocol::Result<core::DailyState> poll_next_day(
    protocol::ProconApiClient& api, const core::MatchConfig& config,
    core::Quantity current_day, PollClock& clock, protocol::SteadyTime deadline,
    const PollingPolicy& policy = {});

}  // namespace hexa_udon::session
