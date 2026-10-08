#pragma once

#include "hexa_udon/protocol/api_client.hpp"
#include "hexa_udon/session/polling.hpp"
#include "hexa_udon/session/session.hpp"
#include "hexa_udon/app/lan_worker.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace hexa_udon::app {

enum class RunMode { DryRun, Execute };
enum class RunStatus { Completed, Stopped, RecoveryRequired, Failed };

// 種別決めも毎日の計画も solver（include/hexa_udon/solver.hpp）で行う
struct AutoClientConfig {
    std::string base_url;
    RunMode mode = RunMode::DryRun;
    std::filesystem::path state_directory;
    std::optional<std::vector<core::AgentKind>> explicit_kinds;
    std::chrono::milliseconds polling_interval{750};
    // 締切のこれだけ前までに提出を終える（procon2026 の client と同じ）
    std::chrono::seconds safety_margin{3};
    std::size_t maximum_get_attempts = 8;
    // 種別決めに使う時間。0 なら盤の大きさごとの締切（16/24/32: 60/90/120 秒）までの残りから safety_margin を引く
    std::chrono::milliseconds kind_budget{0};
    // 良くなった計画を出し直す間隔（秒）
    double interim_seconds = 3.0;
    std::uint64_t planner_seed = 30013;
    std::chrono::milliseconds type_submission_reserve{6000};
    std::vector<LanWorkerEndpoint> lan_workers;
    std::string lan_worker_secret_environment;
    // 0 なら日の残り時間いっぱい worker に計画させる。正の値はその上限
    std::chrono::milliseconds lan_worker_timeout{0};
};

struct RunResult {
    RunStatus status;
    std::string message;
};

class AppClock : public session::PollClock {
public:
    [[nodiscard]] virtual std::chrono::system_clock::time_point wall_now() const = 0;
};

class SystemAppClock final : public AppClock {
public:
    [[nodiscard]] protocol::SteadyTime now() const override;
    [[nodiscard]] std::chrono::system_clock::time_point wall_now() const override;
    void wait_until(protocol::SteadyTime time) override;
};

class SessionDirectoryLock {
public:
    SessionDirectoryLock() = default;
    ~SessionDirectoryLock();
    SessionDirectoryLock(const SessionDirectoryLock&) = delete;
    SessionDirectoryLock& operator=(const SessionDirectoryLock&) = delete;
    SessionDirectoryLock(SessionDirectoryLock&& other) noexcept;
    SessionDirectoryLock& operator=(SessionDirectoryLock&& other) noexcept;

    [[nodiscard]] static protocol::Result<SessionDirectoryLock> acquire(
        const std::filesystem::path& directory);
    [[nodiscard]] bool owns_lock() const noexcept;

private:
    explicit SessionDirectoryLock(int descriptor);
    int descriptor_ = -1;
};

[[nodiscard]] protocol::Result<std::vector<core::AgentKind>> parse_kind_list(
    const std::string& text);

[[nodiscard]] std::string classify_worker_termination(const std::string& termination);

class AutoCompetitionClient {
public:
    AutoCompetitionClient(protocol::ProconApiClient& api, AutoClientConfig config,
                          AppClock& clock, std::function<bool()> stop_requested,
                          std::ostream& output,
                          protocol::OperationLogger* logger = nullptr);

    [[nodiscard]] RunResult run();

private:
    [[nodiscard]] protocol::Result<core::MatchConfig> fetch_setting(
        const std::string& phase = "registration",
        std::optional<protocol::SteadyTime> deadline = std::nullopt);
    [[nodiscard]] protocol::Result<core::DailyState> fetch_state(
        const core::MatchConfig& config,
        std::optional<core::Quantity> current_day,
        std::chrono::system_clock::time_point wall_deadline);
    [[nodiscard]] bool has_unknown_submission(const session::SessionSnapshot& snapshot) const;
    void print_kinds(const std::vector<core::AgentKind>& kinds);
    void print_day_summary(const core::MatchConfig& config, const core::DailyState& daily,
                           const session::SubmissionRecord& record,
                           const simulator::MatchProgress& progress,
                           const nlohmann::json& planning_record = {});

    protocol::ProconApiClient& api_;
    AutoClientConfig config_;
    AppClock& clock_;
    std::function<bool()> stop_requested_;
    std::ostream& output_;
    protocol::NullOperationLogger null_logger_;
    protocol::OperationLogger* logger_;
};

}  // namespace hexa_udon::app
