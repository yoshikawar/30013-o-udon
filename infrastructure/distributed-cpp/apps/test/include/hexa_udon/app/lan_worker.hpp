#pragma once

#include "hexa_udon/core/map_definition.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace hexa_udon::app {

struct LanWorkerEndpoint {
    std::string host;
    unsigned short port = 0;
};

struct LanWorkerConfig {
    LanWorkerEndpoint listen;
    std::string secret_environment;
    std::size_t maximum_frame_bytes = 262144;
    std::size_t worker_index = 0;
    std::size_t worker_count = 1;
    std::filesystem::path diagnostic_log;
    std::string run_id;
    std::string profile_identity;
    std::string evaluator_identity;
};

struct LanWorkerReply {
    bool success = false;
    nlohmann::json payload;
    std::string error;
    // Safe diagnostic classification; never contains request data or secrets.
    std::string failure_classification;
};

[[nodiscard]] std::optional<LanWorkerEndpoint> parse_lan_worker_endpoint(
    const std::string& value);
[[nodiscard]] std::optional<LanWorkerEndpoint> parse_lan_worker_listen_endpoint(
    const std::string& value);
[[nodiscard]] bool is_allowed_worker_address(const std::string& host);
[[nodiscard]] std::string worker_auth_digest(const std::string& secret,
                                              const nlohmann::json& request);
[[nodiscard]] std::string map_identity_digest(const core::MapDefinition& map);
[[nodiscard]] std::string worker_build_fingerprint();
[[nodiscard]] std::string worker_evaluator_identity();
[[nodiscard]] int worker_protocol_schema_version() noexcept;

[[nodiscard]] LanWorkerReply request_lan_worker(
    const LanWorkerEndpoint& endpoint,
    const std::string& secret,
    const nlohmann::json& request,
    std::chrono::milliseconds timeout,
    std::size_t maximum_frame_bytes = 262144);

[[nodiscard]] LanWorkerReply request_lan_worker_preflight(
    const LanWorkerEndpoint& endpoint,
    const std::string& secret,
    std::size_t expected_worker_index,
    std::size_t expected_worker_count,
    std::chrono::milliseconds timeout);

[[nodiscard]] std::chrono::milliseconds remaining_worker_timeout(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point now) noexcept;

[[nodiscard]] std::chrono::steady_clock::time_point worker_reply_deadline(
    std::chrono::steady_clock::time_point planning_started,
    std::chrono::milliseconds worker_budget,
    std::chrono::milliseconds reply_grace = std::chrono::milliseconds{100}) noexcept;

[[nodiscard]] std::string classify_worker_failure(
    const std::string& error, bool deadline_reached) ;

int run_lan_worker(const LanWorkerConfig& config,
                   const std::function<bool()>& stop_requested,
                   std::ostream& output);

}  // namespace hexa_udon::app
