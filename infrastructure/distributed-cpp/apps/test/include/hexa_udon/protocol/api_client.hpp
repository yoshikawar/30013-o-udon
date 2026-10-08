#pragma once

#include "hexa_udon/protocol/http_transport.hpp"
#include "hexa_udon/protocol/json_codec.hpp"
#include "hexa_udon/protocol/request_control.hpp"

#include <mutex>

namespace hexa_udon::protocol {

struct ApiConfig {
    std::string base_url;
    std::string token;
    std::chrono::milliseconds connect_timeout{2000};
    std::chrono::milliseconds total_timeout{5000};
    std::size_t max_response_bytes = 1024 * 1024;
    std::string user_agent = "hexa-udon/0.1";
    std::chrono::milliseconds minimum_request_interval{250};
};

class ProconApiClient {
public:
    ProconApiClient(HttpTransport& transport, ApiConfig config,
                    RequestRateLimiter* limiter = nullptr,
                    OperationLogger* logger = nullptr);

    [[nodiscard]] Result<core::MatchConfig> get_setting(
        std::optional<SteadyTime> deadline = std::nullopt,
        const std::string& phase = "pre-match-setting",
        std::size_t attempt = 1);
    [[nodiscard]] Result<bool> post_agent_kinds(
        const std::vector<core::AgentKind>& kinds,
        std::optional<SteadyTime> deadline = std::nullopt,
        const std::string& phase = "type-submit");
    [[nodiscard]] Result<core::DailyState> get_state(
        const core::MatchConfig& config,
        std::optional<SteadyTime> deadline = std::nullopt,
        const std::string& phase = "daily-state",
        std::size_t attempt = 1);
    [[nodiscard]] Result<std::int32_t> post_actions(
        const simulator::DayActionPlan& plan,
        std::optional<SteadyTime> deadline = std::nullopt,
        const std::string& phase = "daily-submit",
        std::optional<std::uint64_t> local_submission_id = std::nullopt);

private:
    [[nodiscard]] Result<HttpResponse> request(
        HttpMethod method, const std::string& path, const std::string& body = {},
        std::optional<SteadyTime> deadline = std::nullopt,
        const std::string& phase = "",
        std::optional<std::uint64_t> local_submission_id = std::nullopt,
        std::size_t attempt = 1);
    [[nodiscard]] Error http_error(const HttpResponse& response) const;
    [[nodiscard]] Error redact(Error error) const;
    void log_post_response(const std::string& path, const HttpResponse& response,
                           const std::string& phase, std::optional<bool> attempted,
                           const std::string& result,
                           std::optional<std::uint64_t> local_submission_id) noexcept;

    HttpTransport& transport_;
    ApiConfig config_;
    RequestRateLimiter owned_limiter_;
    NullOperationLogger null_logger_;
    RequestRateLimiter* limiter_;
    OperationLogger* logger_;
    std::mutex post_mutex_;
};

}  // namespace hexa_udon::protocol
