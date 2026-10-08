#include "hexa_udon/session/polling.hpp"

#include <algorithm>
#include <thread>

namespace hexa_udon::session {
namespace {

bool retryable(protocol::ErrorCode code) {
    using protocol::ErrorCode;
    return code == ErrorCode::Http5xx || code == ErrorCode::DnsFailure ||
           code == ErrorCode::ConnectionRefused || code == ErrorCode::ConnectionTimeout ||
           code == ErrorCode::TransferTimeout || code == ErrorCode::Disconnected ||
           code == ErrorCode::AccessTime || code == ErrorCode::Http429;
}

std::chrono::milliseconds poll_wait_for(const protocol::Error& error,
                                        std::chrono::milliseconds backoff,
                                        std::chrono::milliseconds minimum_interval) {
    if (error.retry_after_ms && *error.retry_after_ms > 0)
        return std::chrono::milliseconds{*error.retry_after_ms};
    if (error.retry_after_ms && *error.retry_after_ms == 0)
        return std::max(std::chrono::milliseconds{1}, minimum_interval);
    return std::max(std::chrono::milliseconds{1}, backoff);
}

protocol::Result<bool> wait(PollClock& clock, protocol::SteadyTime deadline,
                            std::chrono::milliseconds duration,
                            const char* reason) {
    const auto target = clock.now() + duration;
    if (target >= deadline) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::DeadlineExceeded, reason});
    }
    clock.wait_until(target);
    return protocol::Result<bool>::success(true);
}

}  // namespace

protocol::SteadyTime SystemPollClock::now() const { return std::chrono::steady_clock::now(); }
void SystemPollClock::wait_until(protocol::SteadyTime time) { std::this_thread::sleep_until(time); }

protocol::Result<core::MatchConfig> poll_initial_setting_until_ready(
    protocol::ProconApiClient& api, PollClock& clock, protocol::SteadyTime deadline,
    const PollingPolicy& policy) {
    auto backoff = policy.initial_backoff;
    for (std::size_t attempt = 0; attempt < policy.maximum_attempts; ++attempt) {
        if (clock.now() >= deadline) {
            break;
        }
        auto result = api.get_setting(deadline, "initial-setting", attempt + 1);
        if (result) {
            if (result.value().starts_at != 0) {
                return result;
            }
            auto waited = wait(clock, deadline, backoff, "setting retry exceeds deadline");
            if (!waited) {
                return protocol::Result<core::MatchConfig>::failure(waited.error());
            }
            backoff = std::min(backoff * 2, policy.maximum_backoff);
            continue;
        }
        if (!retryable(result.error().code)) {
            return result;
        }
        auto waited = wait(clock, deadline,
                           poll_wait_for(result.error(), backoff, policy.normal_interval),
                           "setting retry exceeds deadline");
        if (!waited) {
            return protocol::Result<core::MatchConfig>::failure(waited.error());
        }
        backoff = std::min(backoff * 2, policy.maximum_backoff);
    }
    return protocol::Result<core::MatchConfig>::failure(
        {protocol::ErrorCode::DeadlineExceeded, "setting polling limit reached"});
}

protocol::Result<core::DailyState> poll_next_day(
    protocol::ProconApiClient& api, const core::MatchConfig& config,
    core::Quantity current_day, PollClock& clock, protocol::SteadyTime deadline,
    const PollingPolicy& policy) {
    auto backoff = policy.initial_backoff;
    for (std::size_t attempt = 0; attempt < policy.maximum_attempts; ++attempt) {
        if (clock.now() >= deadline) {
            break;
        }
        auto result = api.get_state(config, deadline, "daily-state", attempt + 1);
        if (result) {
            if (result.value().day > current_day) {
                return result;
            }
            if (result.value().day < current_day) {
                return protocol::Result<core::DailyState>::failure(
                    {protocol::ErrorCode::Conflict, "server day moved backwards"});
            }
            auto waited = wait(clock, deadline, backoff, "daily state retry exceeds deadline");
            if (!waited) {
                return protocol::Result<core::DailyState>::failure(waited.error());
            }
            backoff = std::min(backoff * 2, policy.maximum_backoff);
            continue;
        }
        if (!retryable(result.error().code)) {
            return result;
        }
        auto waited = wait(clock, deadline,
                           poll_wait_for(result.error(), backoff, policy.normal_interval),
                           "daily state retry exceeds deadline");
        if (!waited) {
            return protocol::Result<core::DailyState>::failure(waited.error());
        }
        backoff = std::min(backoff * 2, policy.maximum_backoff);
    }
    return protocol::Result<core::DailyState>::failure(
        {protocol::ErrorCode::DeadlineExceeded, "day polling limit reached"});
}

}  // namespace hexa_udon::session
