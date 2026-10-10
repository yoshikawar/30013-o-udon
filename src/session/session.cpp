#include "hexa_udon/session/session.hpp"

#include "hexa_udon/protocol/json_codec.hpp"
#include "hexa_udon/protocol/file_security.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <system_error>

namespace hexa_udon::session {
namespace {

using Json = nlohmann::json;

std::int64_t now_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Json agent_json(const core::AgentState& agent) {
    return {{"kind", core::to_int(agent.kind)},
            {"position", agent.position.value},
            {"fuel", agent.fuel}};
}

Json summary_json(const SimulationSummary& summary) {
    Json brands = Json::array();
    for (const auto brand : summary.brands) {
        brands.push_back(brand);
    }
    Json agents = Json::array();
    for (const auto& agent : summary.end_agents) {
        agents.push_back(agent_json(agent));
    }
    return {{"brands", std::move(brands)},
            {"totalBalls", summary.total_balls},
            {"endAgents", std::move(agents)}};
}

Json planner_json(const PlannerSubmissionMetadata& planner) {
    return {{"kind", planner.planner_kind},
            {"seed", planner.seed},
            {"candidateLimit", planner.candidate_limit},
            {"budgetMilliseconds", planner.budget_milliseconds},
            {"officialScore", planner.official_score},
            {"internalTieBreak", planner.internal_tie_break},
            {"visitedSpots", planner.visited_spots},
            {"rendezvous", planner.rendezvous},
            {"supplyPlans", planner.supply_plans},
            {"routeObjectives", planner.route_objectives},
            {"optimizerCounts", planner.optimizer_counts},
            {"neighborhoodStatistics", planner.neighborhood_statistics},
            {"dailyReadiness", planner.daily_readiness},
            {"baselineOfficialScore", planner.baseline_official_score},
            {"baselineDailyReadiness", planner.baseline_daily_readiness},
            {"officialScoreTieGroupCount", planner.official_score_tie_group_count},
            {"matchIdentity", planner.match_identity},
            {"typeIdentity", planner.type_identity},
            {"snapshotIdentity", planner.snapshot_identity},
            {"agentStateIdentity", planner.agent_state_identity},
            {"nextStateIdentity", planner.next_state_identity},
            {"spotInventoryIdentity", planner.spot_inventory_identity},
            {"safetyReserveMilliseconds", planner.safety_reserve_milliseconds},
            {"selectionReason", planner.selection_reason}};
}

Json type_selection_json(const TypeSelectionMetadata& selection) {
    Json candidates = Json::array();
    for (const auto& candidate : selection.candidates) {
        candidates.push_back({{"types", candidate.kinds},
            {"officialScore", candidate.official_score},
            {"internalTieBreak", candidate.internal_tie_break},
            {"elapsedMicroseconds", candidate.elapsed_microseconds},
            {"method", candidate.evaluation_method},
            {"termination", candidate.termination},
            {"explanation", candidate.explanation}});
    }
    Json result{{"seed", selection.seed},
            {"initialPositionsHash", selection.initial_positions_hash},
            {"selectedTypes", selection.selected_kinds},
            {"selectedOfficialScore", selection.selected_official_score},
            {"selectedMethod", selection.selected_method},
            {"selectionMethod", selection.selected_method},
            {"optimizerStatus", selection.optimizer_status},
            {"selectionWarning", selection.selection_warning},
            {"evaluatedCandidates", selection.evaluated_candidates},
            {"candidateSetHash", selection.candidate_set_hash},
            {"configuredBudgetMilliseconds", selection.configured_budget_milliseconds},
            {"effectiveBudgetMilliseconds", selection.effective_budget_milliseconds},
            {"startedRemainingMilliseconds", selection.started_remaining_milliseconds},
            {"baselinePassRemainingMilliseconds", selection.baseline_pass_remaining_milliseconds},
            {"finalRemainingMilliseconds", selection.final_remaining_milliseconds},
            {"candidateEnumerationMicroseconds", selection.candidate_enumeration_microseconds},
            {"greedyMicroseconds", selection.greedy_microseconds},
            {"refuelMicroseconds", selection.refuel_microseconds},
            {"optimizerMicroseconds", selection.optimizer_microseconds},
            {"strictSimulatorTimingAvailable", selection.strict_simulator_timing_available},
            {"secondPassAttempted", selection.second_pass_attempted},
            {"secondPassTopK", selection.second_pass_top_k},
            {"secondPassReason", selection.second_pass_reason},
            {"maximumSupplyAgents", selection.maximum_supply_agents},
            {"minimumSupplyAgents", selection.minimum_supply_agents},
            {"allowedSupplyCounts", selection.allowed_supply_counts},
            {"totalCandidates", selection.total_candidates},
            {"totalBySupplyCount", selection.total_by_supply_count},
            {"evaluatedBySupplyCount", selection.evaluated_by_supply_count},
            {"budgetMilliseconds", selection.budget_milliseconds},
            {"postReserveMilliseconds", selection.post_reserve_milliseconds},
            {"unevaluatedCandidates", selection.unevaluated_candidates},
            {"confidenceLimited", selection.confidence_limited},
            {"termination", selection.termination},
            {"candidates", std::move(candidates)}};
    if (!selection.fallback_reason.empty()) result["fallbackReason"] = selection.fallback_reason;
    return result;
}

protocol::Result<PlannerSubmissionMetadata> decode_planner(const Json& json) {
    try {
        PlannerSubmissionMetadata result;
        result.planner_kind = json.at("kind").get<std::string>();
        result.seed = json.at("seed").get<std::uint64_t>();
        result.candidate_limit = json.at("candidateLimit").get<std::size_t>();
        result.budget_milliseconds = json.at("budgetMilliseconds").get<std::int64_t>();
        result.official_score = json.at("officialScore").get<std::array<std::int64_t, 3>>();
        result.internal_tie_break = json.at("internalTieBreak").get<std::array<std::int64_t, 3>>();
        result.visited_spots = json.at("visitedSpots").get<std::vector<std::vector<std::size_t>>>();
        if (json.contains("rendezvous")) {
            result.rendezvous = json.at("rendezvous").get<std::vector<std::array<std::int64_t, 6>>>();
        }
        if (json.contains("supplyPlans")) {
            result.supply_plans = json.at("supplyPlans").get<std::vector<std::array<std::int64_t, 3>>>();
        }
        if (json.contains("routeObjectives")) {
            result.route_objectives = json.at("routeObjectives").get<std::vector<std::vector<std::int32_t>>>();
        }
        if (json.contains("optimizerCounts")) {
            result.optimizer_counts = json.at("optimizerCounts").get<std::array<std::uint64_t, 7>>();
        }
        if (json.contains("neighborhoodStatistics")) {
            result.neighborhood_statistics = json.at("neighborhoodStatistics").get<std::vector<std::array<std::uint64_t, 6>>>();
        }
        result.daily_readiness = json.value("dailyReadiness", std::array<std::int64_t, 4>{});
        result.baseline_official_score = json.value("baselineOfficialScore", std::array<std::int64_t, 3>{});
        result.baseline_daily_readiness = json.value("baselineDailyReadiness", std::array<std::int64_t, 4>{});
        result.official_score_tie_group_count = json.value("officialScoreTieGroupCount", std::size_t{0});
        result.match_identity = json.value("matchIdentity", "");
        result.type_identity = json.value("typeIdentity", "");
        result.snapshot_identity = json.value("snapshotIdentity", "");
        result.agent_state_identity = json.value("agentStateIdentity", "");
        result.next_state_identity = json.value("nextStateIdentity", "");
        result.spot_inventory_identity = json.value("spotInventoryIdentity", "");
        result.safety_reserve_milliseconds = json.value("safetyReserveMilliseconds", 0LL);
        result.selection_reason = json.value("selectionReason", "");
        return protocol::Result<PlannerSubmissionMetadata>::success(std::move(result));
    } catch (const Json::exception& error) {
        return protocol::Result<PlannerSubmissionMetadata>::failure(
            {protocol::ErrorCode::Persistence,
             std::string{"invalid planner metadata: "} + error.what()});
    }
}

protocol::Result<SimulationSummary> decode_summary(const Json& json) {
    try {
        if (!json.is_object() || !json.at("brands").is_array() ||
            !json.at("totalBalls").is_number_integer() || !json.at("endAgents").is_array()) {
            return protocol::Result<SimulationSummary>::failure(
                {protocol::ErrorCode::Persistence, "invalid simulation summary"});
        }
        SimulationSummary summary;
        summary.total_balls = json.at("totalBalls").get<core::Quantity>();
        for (const auto& value : json.at("brands")) {
            if (!value.is_number_integer()) {
                throw Json::type_error::create(302, "brand must be integer", &value);
            }
            summary.brands.insert(value.get<core::Quantity>());
        }
        for (const auto& value : json.at("endAgents")) {
            const auto kind_value = value.at("kind");
            if (!kind_value.is_number_integer()) {
                throw Json::type_error::create(302, "kind must be integer", &kind_value);
            }
            const auto kind = core::agent_kind_from_int(kind_value.get<std::int32_t>());
            if (!kind) {
                return protocol::Result<SimulationSummary>::failure(
                    {protocol::ErrorCode::Persistence, "invalid persisted agent kind"});
            }
            summary.end_agents.push_back({*kind,
                                          {value.at("position").get<core::Quantity>()},
                                          value.at("fuel").get<core::Quantity>()});
        }
        return protocol::Result<SimulationSummary>::success(std::move(summary));
    } catch (const Json::exception& error) {
        return protocol::Result<SimulationSummary>::failure(
            {protocol::ErrorCode::Persistence, std::string{"invalid simulation summary: "} + error.what()});
    }
}

SubmissionClassification classification_for(const protocol::Error& error) {
    switch (error.code) {
        case protocol::ErrorCode::RejectedRevision:
            return SubmissionClassification::ServerRejected;
        case protocol::ErrorCode::Auth:
            return SubmissionClassification::AuthenticationFailed;
        case protocol::ErrorCode::AccessTime:
            return SubmissionClassification::AccessTimeError;
        case protocol::ErrorCode::Http429:
            return SubmissionClassification::AccessTimeError;
        case protocol::ErrorCode::UnknownResponse:
            return SubmissionClassification::UnknownResponse;
        default:
            return SubmissionClassification::CommunicationFailed;
    }
}

}  // namespace

std::chrono::milliseconds Deadline::remaining(std::chrono::steady_clock::time_point now) const {
    if (now >= stop_at) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(stop_at - now);
}

SessionController::SessionController(protocol::ProconApiClient& api, core::MatchConfig config,
                                     PersistenceOperations* persistence,
                                     protocol::OperationLogger* logger)
    : api_(api),
      persistence_(persistence == nullptr ? &owned_persistence_ : persistence),
      logger_(logger == nullptr ? &null_logger_ : logger),
      config_(std::move(config)) {
    snapshot_.match_id = match_id();
}

void SessionController::log_transition(
    SessionState from, SessionState to, const std::string& result) noexcept {
    protocol::OperationLogEntry entry;
    entry.timestamp_utc = protocol::utc_timestamp();
    entry.operation = "session-transition";
    entry.result = result;
    if (to == SessionState::RecoveryRequired || to == SessionState::Failed)
        entry.level = protocol::OperationLogEntry::Level::Warning;
    entry.state_transition = std::to_string(static_cast<int>(from)) + "->" +
                             std::to_string(static_cast<int>(to));
    if (result == "agent-post-unknown" || result == "action-post-outcome-unknown") {
        entry.response_classification = "unknown-post-outcome";
        entry.submission_attempted = std::nullopt;
    } else if (to == SessionState::RecoveryRequired) {
        entry.response_classification = "recovery-required";
    }
    if (current_day_) entry.day = current_day_->day;
    logger_->write(entry);
}

SessionState SessionController::state() const noexcept { return state_; }
const SessionSnapshot& SessionController::snapshot() const noexcept { return snapshot_; }
const std::optional<core::DailyState>& SessionController::current_day() const noexcept {
    return current_day_;
}

std::string SessionController::match_id() const {
    std::string value = std::to_string(config_.map.height()) + "x" +
                        std::to_string(config_.map.width()) + ":";
    for (const auto position : config_.initial_agent_positions) {
        value += std::to_string(position.value) + ",";
    }
    return value;
}

protocol::Result<bool> SessionController::observe_day(core::DailyState daily_state) {
    const auto validation = core::validate(daily_state, config_);
    if (!validation.empty()) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::CoreValidation, validation.front().message});
    }
    if (snapshot_.last_observed_day && daily_state.day < *snapshot_.last_observed_day) {
        const auto previous = state_;
        state_ = SessionState::RecoveryRequired;
        log_transition(previous, state_, "day-backwards");
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Conflict, "server day moved backwards"});
    }
    const bool changed = !snapshot_.last_observed_day || daily_state.day != *snapshot_.last_observed_day;
    current_day_ = std::move(daily_state);
    const auto kinds_match = verify_agent_kinds();
    if (!kinds_match) {
        state_ = SessionState::RecoveryRequired;
        return kinds_match;
    }
    snapshot_.last_observed_day = current_day_->day;
    state_ = snapshot_.accepted_days.contains(current_day_->day)
                 ? SessionState::InitialAnswerSubmitted
                 : SessionState::PlanningDay;
    return protocol::Result<bool>::success(changed);
}

protocol::Result<bool> SessionController::submit_agent_kinds(
    const std::vector<core::AgentKind>& kinds,
    std::optional<protocol::SteadyTime> deadline) {
    std::lock_guard submission_lock(submission_mutex_);
    if (state_ == SessionState::RecoveryRequired) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Conflict,
             "recovery is required; automatic type resubmission is disabled",
             std::nullopt, false});
    }
    if (kinds.size() != config_.initial_agent_positions.size()) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::CoreValidation, "agent kind count does not match setting"});
    }
    // startsAt（1 日目が始まる時刻）を過ぎたら種別は受け付けられない。
    // 競技サーバーは種別の受付中から startsAt を入れて返すので、0 でないことでは判定しない。
    // ここでは POST していないので、送っていない（false）として返す
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (config_.starts_at != 0 && now >= config_.starts_at) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Conflict, "agent kinds cannot be submitted after the match started",
             std::nullopt, false});
    }
    if (snapshot_.submitted_agent_kinds) {
        if (*snapshot_.submitted_agent_kinds == kinds) {
            return protocol::Result<bool>::success(false);
        }
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Conflict, "different agent kinds were already submitted"});
    }
    auto result = api_.post_agent_kinds(kinds, deadline, "type-submit");
    if (!result) {
        if (!result.error().submission_attempted.has_value()) {
            snapshot_.agent_kinds_unknown = true;
            const auto previous = state_;
            state_ = SessionState::RecoveryRequired;
            log_transition(previous, state_, "agent-post-unknown");
        }
        return result;
    }
    snapshot_.submitted_agent_kinds = kinds;
    snapshot_.agent_kinds_unknown = false;
    const auto previous = state_;
    state_ = SessionState::AgentKindsSubmitted;
    log_transition(previous, state_, "agent-kinds-accepted");
    return protocol::Result<bool>::success(true);
}

void SessionController::record_daily_planning(nlohmann::json diagnostic) {
    if (diagnostic.is_object() && diagnostic.contains("day")) {
        const auto day = diagnostic.value("day", core::Quantity{-1});
        for (auto iterator = snapshot_.daily_planning_diagnostics.begin();
             iterator != snapshot_.daily_planning_diagnostics.end(); ++iterator) {
            if (!iterator->is_object() || iterator->value("day", core::Quantity{-2}) != day)
                continue;
            *iterator = std::move(diagnostic);
            auto duplicate = iterator + 1;
            while (duplicate != snapshot_.daily_planning_diagnostics.end()) {
                if (duplicate->is_object() && duplicate->value("day", core::Quantity{-2}) == day)
                    duplicate = snapshot_.daily_planning_diagnostics.erase(duplicate);
                else
                    ++duplicate;
            }
            return;
        }
    }
    snapshot_.daily_planning_diagnostics.push_back(std::move(diagnostic));
}
bool SessionController::bind_production_policy(const nlohmann::json& identity) {
    if (!snapshot_.production_policy_identity.is_null())
        return snapshot_.production_policy_identity == identity;
    if (identity.is_null()) return true;
    if (snapshot_.submitted_agent_kinds || snapshot_.last_observed_day || !snapshot_.submissions.empty())
        return false; // Legacy sessions cannot be silently promoted to v2.
    snapshot_.production_policy_identity = identity;
    return true;
}

void SessionController::record_type_selection(TypeSelectionMetadata metadata) {
    snapshot_.type_selection = std::move(metadata);
}

protocol::Result<bool> SessionController::verify_agent_kinds() const {
    if (!snapshot_.submitted_agent_kinds || !current_day_) {
        return protocol::Result<bool>::success(true);
    }
    if (current_day_->own_agents.size() != snapshot_.submitted_agent_kinds->size()) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Conflict, "MatchState agent count differs from submitted kinds"});
    }
    for (std::size_t index = 0; index < current_day_->own_agents.size(); ++index) {
        if (current_day_->own_agents[index].kind != snapshot_.submitted_agent_kinds->at(index)) {
            return protocol::Result<bool>::failure(
                {protocol::ErrorCode::Conflict, "MatchState agent kind differs from submitted order"});
        }
    }
    return protocol::Result<bool>::success(true);
}

protocol::Result<simulator::DayActionPlan> SessionController::make_safe_wait_plan() const {
    if (!current_day_) {
        return protocol::Result<simulator::DayActionPlan>::failure(
            {protocol::ErrorCode::Conflict, "no current day state"});
    }
    const auto day = current_day_->day;
    if (day < 0 || static_cast<std::size_t>(day) >= config_.day_steps.size()) {
        return protocol::Result<simulator::DayActionPlan>::failure(
            {protocol::ErrorCode::CoreValidation, "day has no configured step count"});
    }
    const auto steps = config_.day_steps[static_cast<std::size_t>(day)];
    if (steps <= 0) {
        return protocol::Result<simulator::DayActionPlan>::failure(
            {protocol::ErrorCode::CoreValidation, "day step count must be positive"});
    }
    simulator::DayActionPlan plan(current_day_->own_agents.size());
    for (auto& actions : plan) {
        actions.push_back(simulator::WaitAction{steps});
    }
    return protocol::Result<simulator::DayActionPlan>::success(std::move(plan));
}

protocol::Result<simulator::DaySimulationResult> SessionController::simulate(
    const simulator::DayActionPlan& plan) const {
    if (!current_day_) {
        return protocol::Result<simulator::DaySimulationResult>::failure(
            {protocol::ErrorCode::Conflict, "no current day state"});
    }
    const auto day_index = static_cast<std::size_t>(current_day_->day);
    const simulator::DaySimulationInput input{config_.map,
                                               config_.spots,
                                               config_.fuel_limit,
                                               config_.day_steps.at(day_index),
                                               current_day_->own_agents,
                                               current_day_->traffic};
    auto result = simulator::simulate_day(input, plan);
    if (!result) {
        return protocol::Result<simulator::DaySimulationResult>::failure(
            {protocol::ErrorCode::CoreValidation,
             "safe plan failed strict simulation: " + result.error().message});
    }
    return protocol::Result<simulator::DaySimulationResult>::success(std::move(result).value());
}

protocol::Result<SubmissionRecord> SessionController::submit_safe_wait(
    bool dry_run, std::optional<protocol::SteadyTime> deadline) {
    auto plan = make_safe_wait_plan();
    if (!plan) {
        return protocol::Result<SubmissionRecord>::failure(plan.error());
    }
    return submit_plan(plan.value(), dry_run, deadline);
}

protocol::Result<SubmissionRecord> SessionController::submit_plan(
    const simulator::DayActionPlan& plan, bool dry_run,
    std::optional<protocol::SteadyTime> deadline,
    std::optional<PlannerSubmissionMetadata> planner) {
    std::lock_guard submission_lock(submission_mutex_);
    if (state_ == SessionState::RecoveryRequired) {
        return protocol::Result<SubmissionRecord>::failure(
            {protocol::ErrorCode::Conflict,
             "recovery is required; automatic action resubmission is disabled",
             std::nullopt, false});
    }
    auto simulation = simulate(plan);
    if (!simulation) {
        auto error = simulation.error();
        error.submission_attempted = false;
        return protocol::Result<SubmissionRecord>::failure(std::move(error));
    }
    auto encoded = protocol::encode_actions(plan);
    if (!encoded) {
        auto error = encoded.error();
        error.submission_attempted = false;
        return protocol::Result<SubmissionRecord>::failure(std::move(error));
    }

    SubmissionRecord record;
    record.local_id = next_submission_id_++;
    record.match_id = snapshot_.match_id;
    record.day = current_day_->day;
    record.action_json = encoded.value();
    record.started_at_ms = now_milliseconds();
    record.simulation = {simulation.value().distinct_brands,
                         simulation.value().total_balls,
                         simulation.value().end_agents};
    record.planner = std::move(planner);

    if (dry_run) {
        record.submission_attempted = false;
        record.finished_at_ms = now_milliseconds();
        record.classification = SubmissionClassification::DryRun;
        snapshot_.submissions.push_back(record);
        return protocol::Result<SubmissionRecord>::success(record);
    }

    protocol::OperationLogEntry pending;
    pending.timestamp_utc = protocol::utc_timestamp();
    pending.operation = "daily-submit";
    pending.method = "POST";
    pending.path = "/";
    pending.day = record.day;
    pending.local_submission_id = record.local_id;
    pending.phase = "daily-submit";
    pending.result = "not-started";
    pending.response_classification = "pending";
    pending.submission_attempted = false;
    logger_->write(pending);

    auto submitted = api_.post_actions(plan, deadline, "daily-submit", record.local_id);
    record.finished_at_ms = now_milliseconds();
    if (!submitted) {
        record.submission_attempted = submitted.error().submission_attempted;
        record.classification = classification_for(submitted.error());
        pending.result = record.submission_attempted.has_value()
            ? (*record.submission_attempted ? "post-error" : "not-sent")
            : "unknown-post-outcome";
        pending.response_classification = record.submission_attempted == false
            ? "pre-submit-error" : (record.submission_attempted == true
                ? "post-error" : "unknown-post-outcome");
        pending.stop_reason = submitted.error().message;
        pending.submission_attempted = record.submission_attempted;
        logger_->update(pending);
        snapshot_.submissions.push_back(record);
        if (!record.submission_attempted.has_value()) {
            const auto previous = state_;
            state_ = SessionState::RecoveryRequired;
            log_transition(previous, state_, "action-post-outcome-unknown");
        }
        return protocol::Result<SubmissionRecord>::failure(submitted.error());
    }

    record.http_status = 200;
    record.revision = submitted.value();
    record.submission_attempted = true;
    record.classification = SubmissionClassification::Accepted;
    pending.result = "accepted";
    pending.response_classification = "success";
    pending.http_status = record.http_status;
    pending.revision = record.revision;
    pending.submission_attempted = true;
    logger_->update(pending);
    snapshot_.submissions.push_back(record);
    auto existing = snapshot_.accepted_days.find(record.day);
    if (existing == snapshot_.accepted_days.end() || submitted.value() > existing->second.revision) {
        snapshot_.accepted_days[record.day] =
            {record.day, submitted.value(), record.action_json, record.simulation, record.planner};
        rebuild_progress();
    }
    const auto previous = state_;
    state_ = SessionState::InitialAnswerSubmitted;
    protocol::OperationLogEntry log;
    log.timestamp_utc = protocol::utc_timestamp();
    log.operation = "submit-actions";
    log.method = "POST";
    log.path = "/";
    log.day = record.day;
    log.local_submission_id = record.local_id;
    log.http_status = record.http_status;
    log.revision = record.revision;
    log.elapsed = std::chrono::milliseconds{record.finished_at_ms - record.started_at_ms};
    log.result = "accepted";
    log.submission_attempted = true;
    log.state_transition = std::to_string(static_cast<int>(previous)) + "->" +
                           std::to_string(static_cast<int>(state_));
    logger_->write(log);
    return protocol::Result<SubmissionRecord>::success(record);
}

void SessionController::rebuild_progress() {
    progress_ = {};
    for (const auto& [day, accepted] : snapshot_.accepted_days) {
        (void)day;
        progress_.acquired_brands.insert(accepted.simulation.brands.begin(),
                                         accepted.simulation.brands.end());
        progress_.total_balls += accepted.simulation.total_balls;
        progress_.daily_distinct_brand_counts.push_back(
            static_cast<core::Quantity>(accepted.simulation.brands.size()));
    }
}

simulator::MatchProgress SessionController::progress() const { return progress_; }

protocol::Result<bool> SessionController::save(const std::filesystem::path& path) const {
    Json root{{"schemaVersion", SessionSnapshot::schema_version},
              {"matchId", snapshot_.match_id},
              {"startsAt", config_.starts_at}};
    if (snapshot_.last_observed_day) {
        root["lastObservedDay"] = *snapshot_.last_observed_day;
    } else {
        root["lastObservedDay"] = nullptr;
    }
    root["dailyPlanningDiagnostics"] = snapshot_.daily_planning_diagnostics;
    if (!snapshot_.production_policy_identity.is_null())
        root["productionPolicyIdentity"] = snapshot_.production_policy_identity;
    root["acceptedDays"] = Json::array();
    root["submittedAgentKinds"] = Json::array();
    root["agentKindsUnknown"] = snapshot_.agent_kinds_unknown;
    root["typeSelection"] = snapshot_.type_selection
        ? type_selection_json(*snapshot_.type_selection) : Json(nullptr);
    if (snapshot_.submitted_agent_kinds) {
        for (const auto kind : *snapshot_.submitted_agent_kinds) {
            root["submittedAgentKinds"].push_back(core::to_int(kind));
        }
    }
    for (const auto& [day, accepted] : snapshot_.accepted_days) {
        (void)day;
        Json value{{"day", accepted.day},
                   {"revision", accepted.revision},
                   {"actions", accepted.action_json},
                   {"simulation", summary_json(accepted.simulation)}};
        value["planner"] = accepted.planner ? planner_json(*accepted.planner) : Json(nullptr);
        root["acceptedDays"].push_back(std::move(value));
    }
    root["submissions"] = Json::array();
    for (const auto& record : snapshot_.submissions) {
        Json persisted_record{{"localId", record.local_id},
                              {"matchId", record.match_id},
                              {"day", record.day},
                              {"actions", record.action_json},
                              {"startedAtMs", record.started_at_ms},
                              {"finishedAtMs", record.finished_at_ms},
                              {"classification", static_cast<std::int32_t>(record.classification)},
                              {"simulation", summary_json(record.simulation)}};
        persisted_record["httpStatus"] = record.http_status ? Json(*record.http_status) : Json(nullptr);
        persisted_record["revision"] = record.revision ? Json(*record.revision) : Json(nullptr);
        persisted_record["submissionAttempted"] = record.submission_attempted
            ? Json(*record.submission_attempted) : Json(nullptr);
        persisted_record["planner"] = record.planner ? planner_json(*record.planner) : Json(nullptr);
        root["submissions"].push_back(std::move(persisted_record));
    }

    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Persistence, "cannot open temporary state file"});
    }
    output << root.dump(2) << '\n';
    output.flush();
    if (!output) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Persistence, "cannot write temporary state file"});
    }
    output.close();
    if (!protocol::secure_file(temporary)) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Persistence, "cannot secure temporary state file"});
    }
    auto synced_file = persistence_->sync_file(temporary);
    if (!synced_file) {
        return synced_file;
    }
    auto replaced = persistence_->replace(temporary, path);
    if (!replaced) {
        return replaced;
    }
    if (!protocol::secure_file(path)) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Persistence, "cannot secure state file"});
    }
    auto parent = path.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    auto synced_directory = persistence_->sync_directory(parent);
    if (!synced_directory) {
        return synced_directory;
    }
    return protocol::Result<bool>::success(true);
}

protocol::Result<bool> SessionController::restore(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Persistence, "cannot open state file"});
    }
    try {
        const auto root = Json::parse(input);
        if (!root.at("schemaVersion").is_number_integer()) {
            return protocol::Result<bool>::failure(
                {protocol::ErrorCode::VersionMismatch, "unsupported state schema version"});
        }
        const auto stored_schema = root.at("schemaVersion").get<std::int32_t>();
        if (stored_schema != 4 && stored_schema != SessionSnapshot::schema_version) {
            return protocol::Result<bool>::failure(
                {protocol::ErrorCode::VersionMismatch, "unsupported state schema version"});
        }
        // 盤と初期位置が同じでも、開始時刻（startsAt）が違えば別の試合。どちらかが 0（開始時刻が未定）なら比べない
        const auto stored_starts_at = root.value("startsAt", core::UnixTimestamp{0});
        if (root.at("matchId").get<std::string>() != match_id()
            || (stored_starts_at != 0 && config_.starts_at != 0 && stored_starts_at != config_.starts_at)) {
            state_ = SessionState::RecoveryRequired;
            return protocol::Result<bool>::failure(
                {protocol::ErrorCode::Conflict, "persisted state belongs to another match"});
        }

        SessionSnapshot loaded;
        loaded.daily_planning_diagnostics = root.value("dailyPlanningDiagnostics", std::vector<Json>{});
        loaded.production_policy_identity = root.value("productionPolicyIdentity", Json(nullptr));
        if (!loaded.production_policy_identity.is_null() && !loaded.production_policy_identity.is_object())
            return protocol::Result<bool>::failure({protocol::ErrorCode::Persistence, "invalid production policy identity"});
        loaded.match_id = root.at("matchId").get<std::string>();
        if (!root.at("lastObservedDay").is_null()) {
            loaded.last_observed_day = root.at("lastObservedDay").get<core::Quantity>();
        }
        if (root.contains("submittedAgentKinds") && !root.at("submittedAgentKinds").empty()) {
            std::vector<core::AgentKind> kinds;
            for (const auto& value : root.at("submittedAgentKinds")) {
                if (!value.is_number_integer()) {
                    return protocol::Result<bool>::failure(
                        {protocol::ErrorCode::Persistence, "invalid persisted agent kind"});
                }
                const auto kind = core::agent_kind_from_int(value.get<std::int32_t>());
                if (!kind) {
                    return protocol::Result<bool>::failure(
                        {protocol::ErrorCode::Persistence, "invalid persisted agent kind"});
                }
                kinds.push_back(*kind);
            }
            loaded.submitted_agent_kinds = std::move(kinds);
        }
        if (root.contains("agentKindsUnknown")) {
            if (!root.at("agentKindsUnknown").is_boolean()) {
                return protocol::Result<bool>::failure(
                    {protocol::ErrorCode::Persistence, "invalid agentKindsUnknown value"});
            }
            loaded.agent_kinds_unknown = root.at("agentKindsUnknown").get<bool>();
        }
        if (stored_schema >= 5 && root.contains("typeSelection")
            && !root.at("typeSelection").is_null()) {
            const auto& selection_json = root.at("typeSelection");
            TypeSelectionMetadata selection;
            selection.seed = selection_json.at("seed").get<std::uint64_t>();
            selection.initial_positions_hash = selection_json.at("initialPositionsHash").get<std::string>();
            selection.selected_kinds = selection_json.value("selectedTypes", std::vector<std::int32_t>{});
            selection.selected_official_score = selection_json.value("selectedOfficialScore", std::array<std::int64_t, 3>{});
            selection.selected_method = selection_json.value("selectedMethod", std::string{});
            selection.maximum_supply_agents = selection_json.value("maximumSupplyAgents", 1U);
            selection.minimum_supply_agents = selection_json.value("minimumSupplyAgents", 0U);
            selection.allowed_supply_counts = selection_json.value("allowedSupplyCounts", std::vector<std::size_t>{});
            if (selection.minimum_supply_agents > selection.maximum_supply_agents
                || (selection.allowed_supply_counts.empty() && selection.maximum_supply_agents > 2)
                || (!selection.allowed_supply_counts.empty() && selection.maximum_supply_agents > 3))
                return protocol::Result<bool>::failure({protocol::ErrorCode::Persistence, "invalid selector min/max supply"});
            selection.total_candidates = selection_json.value("totalCandidates", 0U);
            selection.total_by_supply_count = selection_json.value("totalBySupplyCount", std::vector<std::size_t>{});
            selection.evaluated_by_supply_count = selection_json.value("evaluatedBySupplyCount", std::vector<std::size_t>{});
            selection.budget_milliseconds = selection_json.at("budgetMilliseconds").get<std::int64_t>();
            selection.post_reserve_milliseconds = selection_json.value("postReserveMilliseconds", 0LL);
            selection.unevaluated_candidates = selection_json.at("unevaluatedCandidates").get<std::size_t>();
            selection.confidence_limited = selection_json.at("confidenceLimited").get<bool>();
            selection.termination = selection_json.at("termination").get<std::string>();
            selection.fallback_reason = selection_json.value("fallbackReason", std::string{});
            selection.optimizer_status = selection_json.value("optimizerStatus", std::string{"unknown-legacy"});
            selection.selection_warning = selection_json.value("selectionWarning", std::string{});
            selection.evaluated_candidates = selection_json.value("evaluatedCandidates", std::size_t{0});
            selection.candidate_set_hash = selection_json.value("candidateSetHash", std::string{});
            selection.configured_budget_milliseconds = selection_json.value("configuredBudgetMilliseconds", selection.budget_milliseconds);
            selection.effective_budget_milliseconds = selection_json.value("effectiveBudgetMilliseconds", selection.budget_milliseconds);
            selection.started_remaining_milliseconds = selection_json.value("startedRemainingMilliseconds", selection.effective_budget_milliseconds);
            selection.baseline_pass_remaining_milliseconds = selection_json.value("baselinePassRemainingMilliseconds", 0LL);
            selection.final_remaining_milliseconds = selection_json.value("finalRemainingMilliseconds", 0LL);
            selection.candidate_enumeration_microseconds = selection_json.value("candidateEnumerationMicroseconds", 0LL);
            selection.greedy_microseconds = selection_json.value("greedyMicroseconds", 0LL);
            selection.refuel_microseconds = selection_json.value("refuelMicroseconds", 0LL);
            selection.optimizer_microseconds = selection_json.value("optimizerMicroseconds", 0LL);
            selection.strict_simulator_timing_available = selection_json.value("strictSimulatorTimingAvailable", false);
            selection.second_pass_attempted = selection_json.value("secondPassAttempted", false);
            selection.second_pass_top_k = selection_json.value("secondPassTopK", std::size_t{0});
            selection.second_pass_reason = selection_json.value("secondPassReason", std::string{"legacy-unknown"});
            if (selection.fallback_reason == "optimizer phase skipped to preserve equal budgets across candidates") {
                selection.optimizer_status = "skipped-for-fairness";
                selection.fallback_reason.clear();
                selection.confidence_limited = selection.unevaluated_candidates > 0;
            }
            for (const auto& candidate_json : selection_json.at("candidates")) {
                TypeCandidateRecord candidate;
                candidate.kinds = candidate_json.at("types").get<std::vector<std::int32_t>>();
                candidate.official_score = candidate_json.at("officialScore").get<std::array<std::int64_t, 3>>();
                candidate.internal_tie_break = candidate_json.value("internalTieBreak", std::array<std::int64_t, 3>{});
                candidate.elapsed_microseconds = candidate_json.value("elapsedMicroseconds", 0LL);
                candidate.evaluation_method = candidate_json.at("method").get<std::string>();
                candidate.termination = candidate_json.value("termination", "legacy-metadata");
                candidate.explanation = candidate_json.at("explanation").get<std::string>();
                for (const auto kind : candidate.kinds) {
                    if (!core::agent_kind_from_int(kind)) {
                        return protocol::Result<bool>::failure(
                            {protocol::ErrorCode::Persistence, "invalid persisted selector type"});
                    }
                }
                selection.candidates.push_back(std::move(candidate));
            }
            loaded.type_selection = std::move(selection);
        }
        for (const auto& value : root.at("acceptedDays")) {
            auto summary = decode_summary(value.at("simulation"));
            if (!summary) {
                return protocol::Result<bool>::failure(summary.error());
            }
            std::optional<PlannerSubmissionMetadata> planner;
            if (value.contains("planner") && !value.at("planner").is_null()) {
                auto decoded = decode_planner(value.at("planner"));
                if (!decoded) return protocol::Result<bool>::failure(decoded.error());
                planner = decoded.take();
            }
            AcceptedDay accepted{value.at("day").get<core::Quantity>(),
                                 value.at("revision").get<std::int32_t>(),
                                 value.at("actions").get<std::string>(),
                                 summary.take(), std::move(planner)};
            loaded.accepted_days.emplace(accepted.day, std::move(accepted));
        }
        for (const auto& value : root.at("submissions")) {
            auto summary = decode_summary(value.at("simulation"));
            if (!summary) {
                return protocol::Result<bool>::failure(summary.error());
            }
            SubmissionRecord record;
            record.local_id = value.at("localId").get<std::uint64_t>();
            record.match_id = value.at("matchId").get<std::string>();
            record.day = value.at("day").get<core::Quantity>();
            record.action_json = value.at("actions").get<std::string>();
            record.started_at_ms = value.at("startedAtMs").get<std::int64_t>();
            record.finished_at_ms = value.at("finishedAtMs").get<std::int64_t>();
            if (!value.at("httpStatus").is_null()) {
                record.http_status = value.at("httpStatus").get<long>();
            }
            if (!value.at("revision").is_null()) {
                record.revision = value.at("revision").get<std::int32_t>();
            }
            if (value.contains("submissionAttempted") && !value.at("submissionAttempted").is_null())
                record.submission_attempted = value.at("submissionAttempted").get<bool>();
            const auto classification = value.at("classification").get<std::int32_t>();
            if (classification < 0 ||
                classification > static_cast<std::int32_t>(SubmissionClassification::DryRun)) {
                return protocol::Result<bool>::failure(
                    {protocol::ErrorCode::Persistence, "invalid submission classification"});
            }
            record.classification = static_cast<SubmissionClassification>(classification);
            record.simulation = summary.take();
            if (value.contains("planner") && !value.at("planner").is_null()) {
                auto decoded = decode_planner(value.at("planner"));
                if (!decoded) return protocol::Result<bool>::failure(decoded.error());
                record.planner = decoded.take();
            }
            next_submission_id_ = std::max(next_submission_id_, record.local_id + 1);
            loaded.submissions.push_back(std::move(record));
        }
        snapshot_ = std::move(loaded);
        rebuild_progress();
        const bool unknown_submission = snapshot_.agent_kinds_unknown
            || std::any_of(snapshot_.submissions.begin(), snapshot_.submissions.end(),
                           [](const auto& submission) {
                               return !submission.submission_attempted.has_value();
                           });
        state_ = unknown_submission ? SessionState::RecoveryRequired
                                    : SessionState::WaitingForDay;
        return protocol::Result<bool>::success(true);
    } catch (const Json::exception& error) {
        return protocol::Result<bool>::failure(
            {protocol::ErrorCode::Persistence, std::string{"invalid state file: "} + error.what()});
    }
}

Deadline SessionController::deadline(std::chrono::system_clock::time_point wall_now,
                                     std::chrono::steady_clock::time_point steady_now,
                                     std::chrono::seconds safety_margin) const {
    if (!current_day_) {
        return {steady_now};
    }
    const auto end = std::chrono::system_clock::time_point{
        std::chrono::seconds{current_day_->ends_at}};
    const auto until_end = end > wall_now ? end - wall_now : std::chrono::system_clock::duration::zero();
    const auto margin = std::chrono::duration_cast<std::chrono::system_clock::duration>(safety_margin);
    return {steady_now + (until_end > margin ? until_end - margin
                                             : std::chrono::system_clock::duration::zero())};
}

}  // namespace hexa_udon::session
