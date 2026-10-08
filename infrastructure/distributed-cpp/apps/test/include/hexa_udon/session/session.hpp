#pragma once

#include "hexa_udon/protocol/api_client.hpp"
#include "hexa_udon/session/persistence.hpp"
#include "hexa_udon/simulator/simulator.hpp"

#include <chrono>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hexa_udon::session {

enum class SessionState {
    Unconfigured,
    WaitingForSetting,
    AgentKindsReady,
    AgentKindsSubmitted,
    WaitingForMatch,
    WaitingForDay,
    PlanningDay,
    InitialAnswerSubmitted,
    WaitingForDayEnd,
    MatchFinished,
    RecoveryRequired,
    Failed,
};

enum class SubmissionClassification {
    Accepted,
    ServerRejected,
    AuthenticationFailed,
    AccessTimeError,
    CommunicationFailed,
    UnknownResponse,
    DryRun,
};

struct SimulationSummary {
    std::set<core::Quantity> brands;
    core::Quantity total_balls = 0;
    std::vector<core::AgentState> end_agents;
};

struct PlannerSubmissionMetadata {
    std::string planner_kind;
    std::uint64_t seed = 0;
    std::size_t candidate_limit = 0;
    std::int64_t budget_milliseconds = 0;
    std::array<std::int64_t, 3> official_score{};
    std::array<std::int64_t, 3> baseline_official_score{};
    std::array<std::int64_t, 3> internal_tie_break{};
    std::vector<std::vector<std::size_t>> visited_spots;
    std::vector<std::array<std::int64_t, 6>> rendezvous;
    std::vector<std::array<std::int64_t, 3>> supply_plans;
    std::vector<std::vector<std::int32_t>> route_objectives;
    std::array<std::uint64_t, 7> optimizer_counts{};
    std::vector<std::array<std::uint64_t, 6>> neighborhood_statistics;
    std::array<std::int64_t, 4> daily_readiness{};
    std::array<std::int64_t, 4> baseline_daily_readiness{};
    std::size_t official_score_tie_group_count = 0;
    std::string match_identity;
    std::string type_identity;
    std::string snapshot_identity;
    std::string agent_state_identity;
    std::string next_state_identity;
    std::string spot_inventory_identity;
    std::int64_t safety_reserve_milliseconds = 0;
    std::string selection_reason;
};

struct SubmissionRecord {
    std::uint64_t local_id = 0;
    std::string match_id;
    core::Quantity day = 0;
    std::string action_json;
    std::int64_t started_at_ms = 0;
    std::int64_t finished_at_ms = 0;
    std::optional<long> http_status;
    std::optional<std::int32_t> revision;
    std::optional<bool> submission_attempted;
    SubmissionClassification classification = SubmissionClassification::CommunicationFailed;
    SimulationSummary simulation;
    std::optional<PlannerSubmissionMetadata> planner;
};

struct AcceptedDay {
    core::Quantity day = 0;
    std::int32_t revision = 0;
    std::string action_json;
    SimulationSummary simulation;
    std::optional<PlannerSubmissionMetadata> planner;
};

struct TypeCandidateRecord {
    std::vector<std::int32_t> kinds;
    std::array<std::int64_t, 3> official_score{};
    std::array<std::int64_t, 3> internal_tie_break{};
    std::int64_t elapsed_microseconds = 0;
    std::string evaluation_method;
    std::string termination;
    std::string explanation;
};

struct TypeSelectionMetadata {
    std::uint64_t seed = 0;
    std::string initial_positions_hash;
    std::vector<std::int32_t> selected_kinds;
    std::array<std::int64_t, 3> selected_official_score{};
    std::string selected_method;
    std::size_t maximum_supply_agents = 1;
    std::size_t minimum_supply_agents = 0;
    std::vector<std::size_t> allowed_supply_counts;
    std::size_t total_candidates = 0;
    std::vector<std::size_t> total_by_supply_count;
    std::vector<std::size_t> evaluated_by_supply_count;
    std::int64_t budget_milliseconds = 0;
    std::int64_t post_reserve_milliseconds = 0;
    std::size_t unevaluated_candidates = 0;
    bool confidence_limited = false;
    std::string termination;
    std::string fallback_reason;
    std::string optimizer_status = "unknown-legacy";
    std::string selection_warning;
    std::size_t evaluated_candidates = 0;
    std::string candidate_set_hash;
    std::int64_t configured_budget_milliseconds = 0;
    std::int64_t effective_budget_milliseconds = 0;
    std::int64_t started_remaining_milliseconds = 0;
    std::int64_t baseline_pass_remaining_milliseconds = 0;
    std::int64_t final_remaining_milliseconds = 0;
    std::int64_t candidate_enumeration_microseconds = 0;
    std::int64_t greedy_microseconds = 0;
    std::int64_t refuel_microseconds = 0;
    std::int64_t optimizer_microseconds = 0;
    bool strict_simulator_timing_available = false;
    bool second_pass_attempted = false;
    std::size_t second_pass_top_k = 0;
    std::string second_pass_reason;
    std::vector<TypeCandidateRecord> candidates;
};

struct SessionSnapshot {
    static constexpr std::int32_t schema_version = 5;

    std::string match_id;
    std::optional<core::Quantity> last_observed_day;
    std::map<core::Quantity, AcceptedDay> accepted_days;
    std::vector<SubmissionRecord> submissions;
    std::optional<std::vector<core::AgentKind>> submitted_agent_kinds;
    std::optional<TypeSelectionMetadata> type_selection;
    bool agent_kinds_unknown = false;
    std::vector<nlohmann::json> daily_planning_diagnostics;
    nlohmann::json production_policy_identity = nullptr;
};

struct Deadline {
    std::chrono::steady_clock::time_point stop_at;

    [[nodiscard]] std::chrono::milliseconds remaining(
        std::chrono::steady_clock::time_point now) const;
};

class SessionController {
public:
    SessionController(protocol::ProconApiClient& api, core::MatchConfig config,
                      PersistenceOperations* persistence = nullptr,
                      protocol::OperationLogger* logger = nullptr);

    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] const SessionSnapshot& snapshot() const noexcept;
    [[nodiscard]] simulator::MatchProgress progress() const;
    [[nodiscard]] const std::optional<core::DailyState>& current_day() const noexcept;

    [[nodiscard]] protocol::Result<bool> observe_day(core::DailyState daily_state);
    [[nodiscard]] protocol::Result<bool> submit_agent_kinds(
        const std::vector<core::AgentKind>& kinds,
        std::optional<protocol::SteadyTime> deadline = std::nullopt);
    void record_type_selection(TypeSelectionMetadata metadata);
    void record_daily_planning(nlohmann::json diagnostic);
    // Reject changing an established production profile/policy during recovery.
    [[nodiscard]] bool bind_production_policy(const nlohmann::json& identity);
    [[nodiscard]] protocol::Result<bool> verify_agent_kinds() const;
    [[nodiscard]] protocol::Result<simulator::DayActionPlan> make_safe_wait_plan() const;
    [[nodiscard]] protocol::Result<SubmissionRecord> submit_safe_wait(
        bool dry_run, std::optional<protocol::SteadyTime> deadline = std::nullopt);
    [[nodiscard]] protocol::Result<SubmissionRecord> submit_plan(
        const simulator::DayActionPlan& plan, bool dry_run,
        std::optional<protocol::SteadyTime> deadline = std::nullopt,
        std::optional<PlannerSubmissionMetadata> planner = std::nullopt);
    [[nodiscard]] protocol::Result<bool> save(const std::filesystem::path& path) const;
    [[nodiscard]] protocol::Result<bool> restore(const std::filesystem::path& path);

    [[nodiscard]] Deadline deadline(
        std::chrono::system_clock::time_point wall_now,
        std::chrono::steady_clock::time_point steady_now,
        std::chrono::seconds safety_margin = std::chrono::seconds{5}) const;

private:
    [[nodiscard]] std::string match_id() const;
    [[nodiscard]] protocol::Result<simulator::DaySimulationResult> simulate(
        const simulator::DayActionPlan& plan) const;
    void rebuild_progress();
    void log_transition(SessionState from, SessionState to, const std::string& result) noexcept;

    protocol::ProconApiClient& api_;
    PosixPersistenceOperations owned_persistence_;
    PersistenceOperations* persistence_;
    protocol::NullOperationLogger null_logger_;
    protocol::OperationLogger* logger_;
    core::MatchConfig config_;
    SessionState state_ = SessionState::AgentKindsReady;
    SessionSnapshot snapshot_;
    simulator::MatchProgress progress_;
    std::optional<core::DailyState> current_day_;
    std::uint64_t next_submission_id_ = 1;
    mutable std::mutex submission_mutex_;
};

}  // namespace hexa_udon::session
