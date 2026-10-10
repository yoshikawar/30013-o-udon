#include "hexa_udon/app/auto_client.hpp"
#include "hexa_udon/protocol/file_security.hpp"

#include "hexa_udon/solver.hpp"
#include "hexa_udon/simulator/simulator.hpp"
#include "hexa_udon/protocol/json_codec.hpp"
#include "hexa_udon/protocol/request_id_digest.hpp"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <charconv>
#include <iostream>
#include <iomanip>
#include <limits>
#include <future>
#include <sstream>
#include <set>
#include <thread>
#include <cstdlib>
#include <numeric>

namespace hexa_udon::app {

std::string classify_worker_termination(const std::string& termination) {
    if (termination == "completed" || termination == "iteration_limit"
        || termination == "deadline_exhausted_best_available") return "candidate";
    if (termination == "deadline_exhausted") return "deadline-exhausted";
    if (termination == "fallback") return "fallback";
    return "termination-invalid";
}

namespace {

bool retryable(protocol::ErrorCode code) {
    using protocol::ErrorCode;
    return code == ErrorCode::AccessTime || code == ErrorCode::Http429 || code == ErrorCode::Http5xx ||
           code == ErrorCode::DnsFailure || code == ErrorCode::ConnectionRefused ||
           code == ErrorCode::ConnectionTimeout || code == ErrorCode::TransferTimeout ||
           code == ErrorCode::Disconnected;
}

std::string safe_poll_classification(protocol::ErrorCode code) {
    using protocol::ErrorCode;
    if (code == ErrorCode::AccessTime) return "rate-limited-or-not-ready";
    if (code == ErrorCode::Http429) return "http-429-rate-limited";
    if (code == ErrorCode::Http5xx) return "server-error";
    if (code == ErrorCode::DeadlineExceeded) return "deadline-exceeded";
    if (code == ErrorCode::DnsFailure || code == ErrorCode::ConnectionRefused ||
        code == ErrorCode::ConnectionTimeout || code == ErrorCode::TransferTimeout ||
        code == ErrorCode::Disconnected) return "transport-error";
    return "poll-error";
}

std::chrono::milliseconds poll_wait_for(const protocol::Error& error,
                                        std::chrono::milliseconds backoff,
                                        std::chrono::milliseconds minimum_interval) {
    if (error.retry_after_ms && *error.retry_after_ms > 0) {
        return std::chrono::milliseconds{*error.retry_after_ms};
    }
    if (error.retry_after_ms && *error.retry_after_ms == 0) {
        return std::max(std::chrono::milliseconds{1}, minimum_interval);
    }
    return std::max(std::chrono::milliseconds{1}, backoff);
}

std::chrono::system_clock::time_point unix_time(core::UnixTimestamp value) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{value}};
}

std::string stable_hash(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string canonical_json_hash(const nlohmann::json& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value.dump()) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string type_identity(const std::vector<core::AgentKind>& kinds) {
    std::string encoded;
    for (const auto kind : kinds) encoded += std::to_string(core::to_int(kind));
    return stable_hash(encoded);
}

const char* agent_kind_name(const core::AgentKind kind) noexcept {
    switch (kind) {
        case core::AgentKind::Patrol: return "patrol";
        case core::AgentKind::Supply: return "supply";
    }
    return "unknown";
}

std::string daily_snapshot_identity(const core::MatchConfig& config,
                                    const core::DailyState& daily) {
    std::ostringstream encoded;
    encoded << daily.day << '|';
    for (std::int32_t cell = 0; cell < config.map.cell_count(); ++cell) {
        const auto terrain = config.map.terrain_at(core::CellIndex{cell});
        encoded << (terrain.has_value() ? static_cast<int>(*terrain) : -1) << ',';
    }
    for (const auto& traffic : daily.traffic)
        encoded << traffic.position.value << ':' << core::to_int(traffic.status) << ',';
    return stable_hash(encoded.str());
}

std::string agent_state_identity(const core::DailyState& daily) {
    std::ostringstream encoded;
    for (const auto& agent : daily.own_agents)
        encoded << core::to_int(agent.kind) << ':' << agent.position.value << ':' << agent.fuel << ',';
    return stable_hash(encoded.str());
}

nlohmann::json canonical_planner_input(const core::MatchConfig& config,
                                       const core::DailyState& daily,
                                       const simulator::MatchProgress& progress,
                                       const std::vector<core::AgentKind>& kinds,
                                       std::uint64_t seed, std::int64_t worker_budget_ms) {
    nlohmann::json map = {{"height", config.map.height()}, {"width", config.map.width()},
                          {"cells", nlohmann::json::array()}};
    for (std::int32_t cell = 0; cell < config.map.cell_count(); ++cell)
        map["cells"].push_back(core::to_int(*config.map.terrain_at({cell})));
    nlohmann::json spots = nlohmann::json::array();
    for (const auto& spot : config.spots)
        spots.push_back({{"brand", spot.brand}, {"position", spot.position.value}, {"stock", spot.max_stock}});
    nlohmann::json initial = nlohmann::json::array();
    for (const auto position : config.initial_agent_positions) initial.push_back(position.value);
    nlohmann::json agents = nlohmann::json::array();
    for (const auto& agent : daily.own_agents)
        agents.push_back({{"kind", core::to_int(agent.kind)}, {"position", agent.position.value}, {"fuel", agent.fuel}});
    nlohmann::json traffic = nlohmann::json::array();
    for (const auto& road : daily.traffic)
        traffic.push_back({{"position", road.position.value}, {"status", core::to_int(road.status)}});
    nlohmann::json type_values = nlohmann::json::array();
    for (const auto kind : kinds) type_values.push_back(core::to_int(kind));
    nlohmann::json acquired = nlohmann::json::array();
    for (const auto brand : progress.acquired_brands) acquired.push_back(brand);
    return {{"map", map}, {"startsAt", config.starts_at}, {"daySeconds", config.day_seconds},
            {"daySteps", config.day_steps}, {"spots", spots}, {"initialPositions", initial},
            {"fuelLimit", config.fuel_limit}, {"players", config.players},
            {"busyThreshold", config.busy_threshold}, {"jammedThreshold", config.jammed_threshold},
            {"daily", {{"endsAt", daily.ends_at}, {"day", daily.day}, {"agents", agents}, {"traffic", traffic}}},
            {"progress", {{"acquiredBrands", acquired}, {"totalBalls", progress.total_balls},
                           {"dailyDistinctBrandCounts", progress.daily_distinct_brand_counts}}},
            {"types", type_values}, {"plannerSeed", seed}, {"workerBudgetMs", worker_budget_ms}};
}

// 保存してある session が別の試合のものか。盤・初期位置・開始時刻（両方 0 でないとき）のどれかが違うか、
// 最終日まで受理済み（試合が終わっている）なら別の試合とみなす。読めないときは false（restore に任せる）
bool session_belongs_to_another_match(const std::filesystem::path& path, const std::string& match_id,
                                      const core::MatchConfig& config) {
    try {
        std::ifstream input(path, std::ios::binary);
        const auto root = nlohmann::json::parse(input);
        if (root.value("matchId", std::string{}) != match_id) return true;
        const auto stored_starts_at = root.value("startsAt", core::UnixTimestamp{0});
        if (stored_starts_at != 0 && config.starts_at != 0 && stored_starts_at != config.starts_at) return true;
        const auto last_day = static_cast<std::int64_t>(config.day_steps.size()) - 1;
        for (const auto& accepted : root.value("acceptedDays", nlohmann::json::array()))
            if (accepted.value("day", std::int64_t{-1}) == last_day) return true;
        return false;
    } catch (...) {
        return false;
    }
}

// 別の試合の session を同じ directory の session-previous-<時刻>.json に移す（directory はロックしているので動かさない）
std::filesystem::path archive_session_file(const std::filesystem::path& path) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    auto target = path.parent_path() / ("session-previous-" + std::string(stamp) + ".json");
    for (int suffix = 1; std::filesystem::exists(target); ++suffix)
        target = path.parent_path() / ("session-previous-" + std::string(stamp) + "-" + std::to_string(suffix) + ".json");
    std::filesystem::rename(path, target);
    return target;
}

}  // namespace

bool retryable_rejected_post(protocol::ErrorCode code) {
    using protocol::ErrorCode;
    return code == ErrorCode::DnsFailure || code == ErrorCode::ConnectionRefused ||
           code == ErrorCode::ConnectionTimeout || code == ErrorCode::AccessTime ||
           code == ErrorCode::Http429;
}

protocol::SteadyTime SystemAppClock::now() const { return std::chrono::steady_clock::now(); }
std::chrono::system_clock::time_point SystemAppClock::wall_now() const {
    return std::chrono::system_clock::now();
}
void SystemAppClock::wait_until(protocol::SteadyTime time) { std::this_thread::sleep_until(time); }

protocol::Result<std::vector<core::AgentKind>> parse_kind_list(const std::string& text) {
    std::vector<core::AgentKind> result;
    std::istringstream input(text);
    std::string part;
    while (std::getline(input, part, ',')) {
        std::int32_t value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) {
            return protocol::Result<std::vector<core::AgentKind>>::failure(
                {protocol::ErrorCode::InvalidSchema, "type list must contain only 0 or 1"});
        }
        const auto kind = core::agent_kind_from_int(value);
        if (!kind) {
            return protocol::Result<std::vector<core::AgentKind>>::failure(
                {protocol::ErrorCode::InvalidSchema, "type list must contain only 0 or 1"});
        }
        result.push_back(*kind);
    }
    if (result.empty()) {
        return protocol::Result<std::vector<core::AgentKind>>::failure(
            {protocol::ErrorCode::InvalidSchema, "type list must not be empty"});
    }
    return protocol::Result<std::vector<core::AgentKind>>::success(std::move(result));
}

AutoCompetitionClient::AutoCompetitionClient(
    protocol::ProconApiClient& api, AutoClientConfig config, AppClock& clock,
    std::function<bool()> stop_requested, std::ostream& output,
    protocol::OperationLogger* logger)
    : api_(api), config_(std::move(config)), clock_(clock),
      stop_requested_(std::move(stop_requested)), output_(output),
      logger_(logger == nullptr ? &null_logger_ : logger) {}

protocol::Result<core::MatchConfig> AutoCompetitionClient::fetch_setting(
    const std::string& phase, std::optional<protocol::SteadyTime> deadline) {
    const auto configured_interval = std::max(std::chrono::milliseconds{1}, config_.polling_interval);
    auto backoff = configured_interval;
    std::size_t failures = 0;
    std::size_t attempt = 0;
    while (!stop_requested_()) {
        ++attempt;
        auto result = api_.get_setting(deadline, phase, attempt);
        if (result) return result;
        if (!retryable(result.error().code)) return result;
        ++failures;
        const auto wait_duration = poll_wait_for(result.error(), backoff, configured_interval);
        const auto target = clock_.now() + wait_duration;
        if (deadline && target > *deadline) {
            protocol::OperationLogEntry entry;
            entry.level = protocol::OperationLogEntry::Level::Warning;
            entry.timestamp_utc = protocol::utc_timestamp();
            entry.operation = "setting";
            entry.method = "GET";
            entry.path = "/setting";
            entry.endpoint = "/setting";
            entry.phase = phase;
            entry.attempt = attempt;
            entry.result = "deadline-stop";
            entry.response_classification = "deadline-exceeded";
            entry.backoff_reason = "retry-would-exceed-deadline";
            entry.retry_after_ms = result.error().retry_after_ms;
            entry.retry_wait_ms = wait_duration.count();
            entry.deadline_remaining_ms = std::max<std::int64_t>(0,
                std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - clock_.now()).count());
            logger_->write(entry);
            return protocol::Result<core::MatchConfig>::failure(
                {protocol::ErrorCode::DeadlineExceeded, "setting retry exceeds deadline"});
        }
        if (failures >= config_.maximum_get_attempts) {
            protocol::OperationLogEntry entry;
            entry.level = protocol::OperationLogEntry::Level::Warning;
            entry.timestamp_utc = protocol::utc_timestamp();
            entry.operation = "setting";
            entry.method = "GET";
            entry.path = "/setting";
            entry.endpoint = "/setting";
            entry.phase = phase;
            entry.attempt = attempt;
            entry.result = "polling-limit";
            entry.response_classification = "polling-limit";
            entry.backoff_reason = "maximum-attempts-reached";
            entry.retry_after_ms = result.error().retry_after_ms;
            entry.retry_wait_ms = wait_duration.count();
            logger_->write(entry);
            return result;
        }
        protocol::OperationLogEntry entry;
        entry.timestamp_utc = protocol::utc_timestamp();
        entry.operation = "setting";
        entry.method = "GET";
        entry.path = "/setting";
        entry.endpoint = "/setting";
        entry.phase = phase;
        entry.attempt = attempt;
        entry.result = "retry-scheduled";
        entry.response_classification = safe_poll_classification(result.error().code);
        entry.backoff_reason = result.error().retry_after_ms ? "retry-after" : "bounded-exponential-backoff";
        entry.retry_after_ms = result.error().retry_after_ms;
        entry.retry_wait_ms = wait_duration.count();
        if (deadline) entry.deadline_remaining_ms = std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - clock_.now()).count());
        logger_->write(entry);
        clock_.wait_until(target);
        backoff = std::min(backoff * 2, std::chrono::milliseconds{4000});
    }
    return protocol::Result<core::MatchConfig>::failure(
        {protocol::ErrorCode::Conflict, "stop requested"});
}

protocol::Result<core::DailyState> AutoCompetitionClient::fetch_state(
    const core::MatchConfig& config, std::optional<core::Quantity> current_day,
    std::chrono::system_clock::time_point wall_deadline) {
    const auto configured_interval = std::max(std::chrono::milliseconds{1}, config_.polling_interval);
    auto backoff = configured_interval;
    std::size_t failures = 0;
    std::size_t attempt = 0;
    while (!stop_requested_() && clock_.wall_now() < wall_deadline) {
        ++attempt;
        const auto wall_remaining = wall_deadline - clock_.wall_now();
        const auto request_deadline = clock_.now() +
            std::chrono::duration_cast<protocol::SteadyTime::duration>(wall_remaining);
        auto result = api_.get_state(config, request_deadline, "daily-state", attempt);
        if (result) {
            failures = 0;
            backoff = configured_interval;
            if (!current_day || result.value().day > *current_day) return result;
            if (result.value().day < *current_day) {
                return protocol::Result<core::DailyState>::failure(
                    {protocol::ErrorCode::Conflict, "server day moved backwards"});
            }
            const auto target = clock_.now() + backoff;
            if (target > request_deadline || clock_.wall_now() + backoff > wall_deadline) {
                protocol::OperationLogEntry entry;
                entry.level = protocol::OperationLogEntry::Level::Warning;
                entry.timestamp_utc = protocol::utc_timestamp();
                entry.operation = "daily-state";
                entry.method = "GET";
                entry.path = "/";
                entry.endpoint = "/";
                entry.phase = "daily-state";
                entry.attempt = attempt;
                entry.result = "deadline-stop";
                entry.response_classification = "deadline-exceeded";
                entry.backoff_reason = "polling-next-state-would-exceed-deadline";
                entry.retry_wait_ms = backoff.count();
                logger_->write(entry);
                break;
            }
            protocol::OperationLogEntry entry;
            entry.timestamp_utc = protocol::utc_timestamp();
            entry.operation = "daily-state";
            entry.method = "GET";
            entry.path = "/";
            entry.endpoint = "/";
            entry.phase = "daily-state";
            entry.attempt = attempt;
            entry.result = "retry-scheduled";
            entry.response_classification = "state-not-ready";
            entry.backoff_reason = "bounded-exponential-backoff";
            entry.retry_wait_ms = backoff.count();
            logger_->write(entry);
            clock_.wait_until(target);
            backoff = std::min(backoff * 2, std::chrono::milliseconds{4000});
            continue;
        }
        if (result.error().code == protocol::ErrorCode::Auth || !retryable(result.error().code)) {
            return result;
        }
        ++failures;
        const auto wait_duration = poll_wait_for(result.error(), backoff, configured_interval);
        const auto target = clock_.now() + wait_duration;
        if (clock_.now() + wait_duration > request_deadline ||
            clock_.wall_now() + wait_duration > wall_deadline) {
            protocol::OperationLogEntry entry;
            entry.level = protocol::OperationLogEntry::Level::Warning;
            entry.timestamp_utc = protocol::utc_timestamp();
            entry.operation = "daily-state";
            entry.method = "GET";
            entry.path = "/";
            entry.endpoint = "/";
            entry.phase = "daily-state";
            entry.attempt = attempt;
            entry.result = "deadline-stop";
            entry.response_classification = "deadline-exceeded";
            entry.backoff_reason = "retry-would-exceed-deadline";
            entry.retry_after_ms = result.error().retry_after_ms;
            entry.retry_wait_ms = wait_duration.count();
            logger_->write(entry);
            break;
        }
        if (failures >= config_.maximum_get_attempts) {
            protocol::OperationLogEntry entry;
            entry.level = protocol::OperationLogEntry::Level::Warning;
            entry.timestamp_utc = protocol::utc_timestamp();
            entry.operation = "daily-state";
            entry.method = "GET";
            entry.path = "/";
            entry.endpoint = "/";
            entry.phase = "daily-state";
            entry.attempt = attempt;
            entry.result = "polling-limit";
            entry.response_classification = "polling-limit";
            entry.backoff_reason = "maximum-attempts-reached";
            entry.retry_after_ms = result.error().retry_after_ms;
            entry.retry_wait_ms = wait_duration.count();
            logger_->write(entry);
            return result;
        }
        protocol::OperationLogEntry entry;
        entry.timestamp_utc = protocol::utc_timestamp();
        entry.operation = "daily-state";
        entry.method = "GET";
        entry.path = "/";
        entry.endpoint = "/";
        entry.phase = "daily-state";
        entry.attempt = attempt;
        entry.result = "retry-scheduled";
        entry.response_classification = safe_poll_classification(result.error().code);
        entry.backoff_reason = result.error().retry_after_ms ? "retry-after" : "bounded-exponential-backoff";
        entry.retry_after_ms = result.error().retry_after_ms;
        entry.retry_wait_ms = wait_duration.count();
        logger_->write(entry);
        clock_.wait_until(target);
        backoff = std::min(backoff * 2, std::chrono::milliseconds{4000});
    }
    return protocol::Result<core::DailyState>::failure(
        {protocol::ErrorCode::DeadlineExceeded, "state polling deadline reached"});
}

bool AutoCompetitionClient::has_unknown_submission(const session::SessionSnapshot& snapshot) const {
    // 止めるのは種別の結果が分からないときだけ。日の提出は、あとに出したものが有効になるので
    // 結果の分からない提出（submissionAttempted が null）があっても、次の提出で上書きして続ける
    return snapshot.agent_kinds_unknown;
}

static std::size_t unknown_action_posts(const session::SessionSnapshot& snapshot) {
    return static_cast<std::size_t>(std::count_if(snapshot.submissions.begin(), snapshot.submissions.end(),
        [](const auto& item) { return !item.submission_attempted.has_value(); }));
}

void AutoCompetitionClient::print_kinds(const std::vector<core::AgentKind>& kinds) {
    output_ << "type-selection=complete agents=" << kinds.size() << '\n';
}

void AutoCompetitionClient::print_day_summary(
    const core::MatchConfig& config, const core::DailyState& daily,
    const session::SubmissionRecord& record, const simulator::MatchProgress& progress,
    const nlohmann::json& planning_record) {
    simulator::OfficialScore official_score;
    if (planning_record.is_object() && planning_record.contains("score")
        && planning_record.at("score").is_array()
        && planning_record.at("score").size() == 3) {
        official_score = {
            planning_record.at("score").at(0).get<std::int64_t>(),
            planning_record.at("score").at(1).get<std::int64_t>(),
            planning_record.at("score").at(2).get<std::int64_t>()};
    } else if (record.planner) {
        official_score = {
            record.planner->official_score[0], record.planner->official_score[1],
            record.planner->official_score[2]};
    } else {
        official_score = {
            static_cast<std::int64_t>(progress.acquired_brands.size()),
            std::accumulate(progress.daily_distinct_brand_counts.begin(),
                            progress.daily_distinct_brand_counts.end(), std::int64_t{0}),
            progress.total_balls};
    }
    const auto candidate_source = planning_record.is_object() ? planning_record.value(
        "candidateSource", record.planner ? record.planner->planner_kind : "baseline")
        : record.planner ? record.planner->planner_kind : "baseline";
    const auto adoption = planning_record.is_object() ? planning_record.value(
        "adoptionReason", record.planner ? record.planner->selection_reason : "baseline-retained")
        : record.planner ? record.planner->selection_reason : "baseline-retained";
    output_ << "daily-end day=" << daily.day
            << " score=[" << official_score.total_unique_brands << ','
            << official_score.cumulative_daily_unique_brands << ','
            << official_score.total_bowls << "]"
            << " candidateSource=" << candidate_source
            << " adoption=" << adoption
            << " post=" << (record.revision ? "success" : config_.mode == RunMode::DryRun ? "dry-run" : "none");
    if (planning_record.is_object() && planning_record.value("unknownPosts", std::size_t{0}) > 0)
        output_ << " unknownPosts=" << planning_record.value("unknownPosts", std::size_t{0});
    output_ << " agents=\"";
    for (std::size_t index = 0; index < record.simulation.end_agents.size(); ++index) {
        if (index != 0) output_ << ';';
        const auto& agent = record.simulation.end_agents[index];
        output_ << index << ':' << agent_kind_name(agent.kind)
                << '@' << agent.position.value << " fuel=" << agent.fuel;
    }
    output_ << "\"\n";
    static_cast<void>(config);
}

namespace {

// 種別の回答の締切（秒）。募集要項: 1 回戦 60 / 準決勝 90 / 決勝 120、練習場: 16×16 / 24×24 / 32×32 で 60 / 90 / 120
std::int64_t kind_deadline_seconds(core::GridSize width) {
    if (width <= 16) return 60;
    if (width <= 24) return 90;
    return 120;
}

// worker に渡した依頼と、返ってきた計画の検証結果
struct PendingWorker {
    nlohmann::json request;
    std::future<LanWorkerReply> reply;
    protocol::SteadyTime dispatched_at;
};

struct WorkerCandidate {
    simulator::DayActionPlan plan;
    simulator::DaySimulationResult simulation;
    simulator::OfficialScore score;
    std::string action_hash;
    std::size_t worker_index = 0;
};

}  // namespace

RunResult AutoCompetitionClient::run() {
    output_ << "mode=" << (config_.mode == RunMode::Execute ? "EXECUTE" : "DRY-RUN")
            << " transport=configured" << '\n';
    if (stop_requested_()) return {RunStatus::Stopped, "stop requested before startup"};
    std::error_code directory_error;
    std::filesystem::create_directories(config_.state_directory, directory_error);
    if (directory_error) return {RunStatus::Failed, "cannot create state directory"};
    if (!protocol::secure_directory(config_.state_directory)) {
        return {RunStatus::Failed, "cannot secure state directory"};
    }
    const auto existing_state = config_.state_directory / "session.json";
    if (std::filesystem::exists(existing_state)
        && !protocol::secure_file(existing_state)) {
        return {RunStatus::Failed, "cannot secure existing state file"};
    }

    std::optional<SessionDirectoryLock> lock;
    if (config_.mode == RunMode::Execute) {
        auto acquired = SessionDirectoryLock::acquire(config_.state_directory);
        if (!acquired) return {RunStatus::Failed, acquired.error().message};
        lock = acquired.take();
    }

    auto setting = fetch_setting("registration");
    if (!setting) return {stop_requested_() ? RunStatus::Stopped : RunStatus::Failed,
                          setting.error().message};
    const auto setting_received_at = clock_.wall_now();
    output_ << "startup=setting-received\n";
    solver::loadProblem(setting.value());

    const auto state_path = config_.state_directory / "session.json";
    session::SessionController initial(api_, setting.value(), nullptr, logger_);
    // 別の試合の session が残っていたら退避して、最初から始める。同じ試合なら読み込んで途中から入り直す
    if (std::filesystem::exists(state_path)
        && session_belongs_to_another_match(state_path, initial.match_id(), setting.value())) {
        const auto archived = archive_session_file(state_path);
        output_ << "session=archived reason=another-match file=" << archived.filename().string() << '\n';
    }
    if (std::filesystem::exists(state_path)) {
        output_ << "session=restored (same match)\n";
        auto restored = initial.restore(state_path);
        if (!restored) return {RunStatus::RecoveryRequired, restored.error().message};
        if (has_unknown_submission(initial.snapshot())) {
            protocol::OperationLogEntry recovery_log;
            recovery_log.level = protocol::OperationLogEntry::Level::Warning;
            recovery_log.timestamp_utc = protocol::utc_timestamp();
            recovery_log.operation = "recovery-required";
            recovery_log.phase = "recovery";
            recovery_log.result = "saved POST outcome is unknown";
            recovery_log.response_classification = "unknown-post-outcome";
            recovery_log.submission_attempted = std::nullopt;
            logger_->write(recovery_log);
            return {RunStatus::RecoveryRequired, "saved session contains an unknown POST outcome"};
        }
        if (const auto unknown = unknown_action_posts(initial.snapshot()); unknown > 0)
            output_ << "warning=restored-unknown-daily-posts count=" << unknown << " continuing\n";
    }
    // 種別の締切。設定を受け取った時刻から数え、startsAt（1 日目が始まる時刻）の方が早ければそこまで
    auto kind_deadline = setting_received_at + std::chrono::seconds{kind_deadline_seconds(setting.value().map.width())};
    if (setting.value().starts_at > 0)
        kind_deadline = std::min(kind_deadline, std::chrono::system_clock::time_point{
            std::chrono::seconds{setting.value().starts_at}});
    std::vector<core::AgentKind> selected_kinds;
    if (initial.snapshot().submitted_agent_kinds) {
        selected_kinds = *initial.snapshot().submitted_agent_kinds;
        if (config_.explicit_kinds && *config_.explicit_kinds != selected_kinds) {
            return {RunStatus::RecoveryRequired,
                    "requested types differ from the saved official-order types"};
        }
        output_ << "type-selection=restored\n";
    } else if (config_.explicit_kinds) {
        selected_kinds = *config_.explicit_kinds;
        output_ << "type-selection=explicit\n";
    } else {
        // 締切までの残りをミリ秒で数える
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            kind_deadline - clock_.wall_now()).count();
        // POST の分として、--safety-seconds（既定 3 秒）か残りの半分の短い方を残す
        const auto reserve = std::min<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(config_.safety_margin).count(),
            std::max<std::int64_t>(0, remaining) / 2);
        const auto budget = config_.kind_budget.count() > 0 ? config_.kind_budget.count()
            : std::max<std::int64_t>(0, remaining - reserve);
        std::vector<int> kinds;
        // 締切（startsAt）をもう過ぎていたら種別は受け付けられないので、選ばずに全員を巡回車として進む。
        // 持ち時間の半分で選ぶ（各候補に焼きなましを回す）。残りは 1 日目の先読みに使う。
        // 半分が 300ms に満たないときは焼きなましをせず、補給車 1 台の候補のうち中心に近いものをすぐ出す
        // （締切に間に合わず種別が届かないと、全員が巡回車になるため）
        if (remaining <= 0 && config_.kind_budget.count() == 0) {
            kinds.assign(setting.value().initial_agent_positions.size(), 0);
            output_ << "type-selection=skipped reason=deadline-passed remainingMs=" << remaining << '\n';
        } else if (budget / 2 < 300 && config_.kind_budget.count() == 0) {
            kinds = solver::kindCandidates().front();
            output_ << "type-selection=start mode=quick remainingMs=" << remaining << '\n';
        } else {
            output_ << "type-selection=start mode=solver budgetMs=" << budget << " remainingMs=" << remaining << '\n';
            kinds = solver::solveKind(static_cast<double>(budget) * 0.5);
        }
        solver::choosingKinds = false;
        for (const int kind : kinds) selected_kinds.push_back(static_cast<core::AgentKind>(kind));
        protocol::OperationLogEntry selector_log;
        selector_log.timestamp_utc = protocol::utc_timestamp();
        selector_log.operation = "solver-type-selection";
        selector_log.method = "LOCAL";
        selector_log.path = "Day0";
        selector_log.result = "types=";
        for (std::size_t i = 0; i < selected_kinds.size(); ++i) {
            if (i) selector_log.result.push_back(',');
            selector_log.result += std::to_string(core::to_int(selected_kinds[i]));
        }
        selector_log.result += ";budgetMs=" + std::to_string(budget);
        selector_log.state_transition = "types-selected-before-agent-post";
        logger_->write(selector_log);
    }
    if (selected_kinds.size() != setting.value().initial_agent_positions.size())
        return {RunStatus::Failed, "type count does not match agents"};
    print_kinds(selected_kinds);
    if (config_.mode == RunMode::Execute && !initial.snapshot().submitted_agent_kinds) {
        auto submitted = initial.submit_agent_kinds(selected_kinds,
            std::optional<protocol::SteadyTime>{clock_.now() + config_.type_submission_reserve});
        const auto saved = initial.save(state_path);
        if (!saved) return {RunStatus::Failed, saved.error().message};
        if (!submitted) {
            // 送ったかどうか分からないときだけ止める（受け付けられていれば種別が違ってしまうため）
            if (!submitted.error().submission_attempted.has_value())
                return {RunStatus::RecoveryRequired, submitted.error().message};
            // 送らなかった・断られた（受け付けられていないのが確か）。公式のルールでは種別が届かなければ
            // 全員が巡回車になるので、止まらずにそのつもりで毎日の計画へ進む
            output_ << "warning=agent-types-not-accepted reason=\"" << submitted.error().message
                    << "\" continuing-as=all-patrol\n";
            selected_kinds.assign(selected_kinds.size(), core::AgentKind::Patrol);
        } else {
            output_ << "post=agent-types success\n";
        }
    } else if (config_.mode == RunMode::DryRun) {
        output_ << "post=agent-types dry-run\n";
    }

    // 種別を出したら、1 日目が始まるまで（種別の締切まで）別のスレッドで 1 日目を計画しておく。
    // 途中から入り直したときは 1 日目が終わっているかもしれないので計画しない
    std::future<void> day0_planning;
    if (!initial.snapshot().last_observed_day) {
        std::vector<int> kinds;
        for (const auto kind : selected_kinds) kinds.push_back(core::to_int(kind));
        const auto day0_ms = std::chrono::duration_cast<std::chrono::milliseconds>(kind_deadline - clock_.wall_now()).count();
        if (day0_ms > 0) {
            output_ << "day0-planning=start budgetMs=" << day0_ms << '\n';
            day0_planning = std::async(std::launch::async,
                [kinds, day0_ms] { solver::planDay0(kinds, static_cast<double>(day0_ms)); });
        }
    }

    // Once type registration is complete, wait for the first daily state on GET /
    // instead of re-fetching the already accepted match setting. fetch_state()
    // owns the 403/Retry-After/backoff/deadline policy for this phase.
    output_ << "waiting-for-match\n";

    session::SessionController competition(api_, setting.value(), nullptr, logger_);
    if (std::filesystem::exists(state_path)) {
        auto restored = competition.restore(state_path);
        if (!restored) return {RunStatus::RecoveryRequired, restored.error().message};
    }
    std::optional<core::Quantity> observed_day = competition.snapshot().last_observed_day;
    bool reconcile_server_state = observed_day.has_value();
    std::set<core::Quantity> dry_processed;
    const auto total_days = static_cast<core::Quantity>(setting.value().day_steps.size());
    const auto log_daily_failure = [&](const core::Quantity day, const std::string& phase,
                                       const std::string& reason,
                                       std::optional<bool> submission_attempted = std::optional<bool>{false},
                                       const std::string& source_operation = "daily-replan") {
        std::string safe_reason = reason;
        for (auto& character : safe_reason) {
            if (character == '\n' || character == '\r') character = ' ';
        }
        if (safe_reason.size() > 240) safe_reason.resize(240);
        protocol::OperationLogEntry entry;
        entry.level = protocol::OperationLogEntry::Level::Warning;
        entry.timestamp_utc = protocol::utc_timestamp();
        entry.operation = "daily-replan";
        entry.method = "LOCAL";
        entry.path = "Day" + std::to_string(day);
        entry.day = day;
        entry.result = "termination=" + phase + ";failureReason=" + safe_reason;
        entry.state_transition = "safe-stop-no-plan-post";
        entry.endpoint = "/";
        entry.phase = "daily-submit";
        entry.stop_reason = phase + ":" + safe_reason;
        entry.response_classification = "safe-stop";
        entry.source_operation = source_operation;
        entry.submission_attempted = submission_attempted;
        logger_->write(entry);
    };

    while (!stop_requested_()) {
        const auto wait_deadline = clock_.wall_now() + std::chrono::minutes{10};
        auto daily = fetch_state(setting.value(), reconcile_server_state ? std::nullopt : observed_day,
                                 wait_deadline);
        if (!daily) {
            if (stop_requested_()) {
                if (config_.mode == RunMode::Execute) {
                    const auto saved_stop = competition.save(state_path);
                    if (!saved_stop) return {RunStatus::Failed, saved_stop.error().message};
                }
                protocol::OperationLogEntry log;
                log.timestamp_utc = protocol::utc_timestamp();
                log.operation = "automatic-client-exit";
                log.result = "signal-stop";
                logger_->write(log);
                return {RunStatus::Stopped, "stop requested"};
            }
            if (observed_day && *observed_day == total_days - 1 &&
                competition.current_day() && clock_.wall_now() >= unix_time(competition.current_day()->ends_at)) {
                if (config_.mode == RunMode::Execute) {
                    const auto saved_final = competition.save(state_path);
                    if (!saved_final) return {RunStatus::Failed, saved_final.error().message};
                }
                return {RunStatus::Completed, "final day deadline passed"};
            }
            return {RunStatus::Failed, daily.error().message};
        }
        reconcile_server_state = false;
        if (observed_day && daily.value().day > *observed_day + 1) {
            log_daily_failure(daily.value().day, "input_failure", "server skipped an unrecorded day");
            return {RunStatus::RecoveryRequired, "server skipped an unrecorded day"};
        }
        if (daily.value().day < 0 || daily.value().day >= total_days) {
            log_daily_failure(daily.value().day, "input_failure", "invalid day");
            return {RunStatus::RecoveryRequired, "server returned an invalid day"};
        }
        auto observed = competition.observe_day(daily.value());
        if (!observed) {
            log_daily_failure(daily.value().day, "input_failure", observed.error().message);
            return {RunStatus::RecoveryRequired, observed.error().message};
        }
        observed_day = daily.value().day;
        if (daily.value().day == 0 && std::any_of(daily.value().traffic.begin(),
                                                  daily.value().traffic.end(),
                                                  [](const auto& road) {
                                                      return road.status != core::RoadStatus::Smooth;
                                                  })) {
            log_daily_failure(daily.value().day, "input_failure", "Day0 road is not smooth");
            return {RunStatus::RecoveryRequired, "Day0 roads must be smooth"};
        }
        output_ << "daily-start day=" << daily.value().day << '\n';

        const bool already_done = config_.mode == RunMode::Execute
            ? competition.snapshot().accepted_days.contains(daily.value().day)
            : dry_processed.contains(daily.value().day);
        if (!already_done) {
            const auto deadline = competition.deadline(clock_.wall_now(), clock_.now(), config_.safety_margin);
            if (deadline.remaining(clock_.now()).count() <= 0) {
                log_daily_failure(daily.value().day, "deadline_exhausted", "safety deadline");
                return {RunStatus::RecoveryRequired, "day safety deadline already passed"};
            }
            const auto previous_progress = competition.progress();
            // 計画ができるまでの保険として、まず全員が待つ計画を出す（procon2026 の client と同じ）
            auto submitted = competition.submit_safe_wait(
                config_.mode == RunMode::DryRun, deadline.stop_at);
            auto submit_backoff = std::min(
                std::max(std::chrono::milliseconds{1}, config_.polling_interval),
                std::chrono::milliseconds{250});
            while (!submitted && submitted.error().submission_attempted == false &&
                   retryable_rejected_post(submitted.error().code) && !stop_requested_()) {
                const auto wait_duration = poll_wait_for(
                    submitted.error(), submit_backoff, std::chrono::milliseconds{1});
                const auto retry_at = clock_.now() + wait_duration;
                if (retry_at >= deadline.stop_at) break;

                protocol::OperationLogEntry retry_log;
                retry_log.timestamp_utc = protocol::utc_timestamp();
                retry_log.operation = "daily-submit";
                retry_log.method = "POST";
                retry_log.path = "/";
                retry_log.endpoint = "/";
                retry_log.phase = "daily-submit";
                retry_log.result = "retry-scheduled";
                retry_log.response_classification = safe_poll_classification(submitted.error().code);
                retry_log.backoff_reason = submitted.error().retry_after_ms
                    ? "retry-after" : "bounded-exponential-backoff";
                retry_log.retry_after_ms = submitted.error().retry_after_ms;
                retry_log.retry_wait_ms = wait_duration.count();
                retry_log.deadline_remaining_ms = std::max<std::int64_t>(0,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline.stop_at - clock_.now()).count());
                retry_log.submission_attempted = false;
                logger_->write(retry_log);

                clock_.wait_until(retry_at);
                if (clock_.now() >= deadline.stop_at || stop_requested_()) break;
                submitted = competition.submit_safe_wait(false, deadline.stop_at);
                submit_backoff = std::min(submit_backoff * 2, std::chrono::milliseconds{2000});
            }
            // 受け付けられた最後の提出。保険が通らなかったときは、何も受け付けられていない（全員がその場にいる）とみなす
            session::SubmissionRecord last_record;
            last_record.simulation.end_agents = daily.value().own_agents;
            std::size_t unknown_posts = 0;  // この日に送れたか分からなかった提出の数
            if (submitted) {
                last_record = submitted.value();
            } else {
                log_daily_failure(daily.value().day, "simulation_or_transport_failure",
                                  submitted.error().message, submitted.error().submission_attempted,
                                  "daily-submit");
                // 保険が通らなくても止めずに solver の計画を出しに行く。日の提出は、あとに出したものが有効になる
                const bool unknown = !submitted.error().submission_attempted.has_value();
                if (unknown) {
                    ++unknown_posts;
                    static_cast<void>(competition.continue_after_unknown_action_post());
                }
                output_ << "warning=safe-wait-" << (unknown ? "outcome-unknown" : "not-accepted")
                        << " day=" << daily.value().day << " reason=\"" << submitted.error().message
                        << "\" continuing\n";
            }
            if (config_.mode == RunMode::Execute) {
                auto saved = competition.save(state_path);
                if (!saved) return {RunStatus::Failed, saved.error().message};
            }

            const auto& day_state = daily.value();
            const auto steps = setting.value().day_steps.at(static_cast<std::size_t>(day_state.day));
            const simulator::DaySimulationInput sim_input{setting.value().map, setting.value().spots,
                setting.value().fuel_limit, steps, day_state.own_agents, day_state.traffic};
            std::vector<std::vector<int>> adopted = solver::allWait(steps);
            std::string adopted_source = "wait";
            // 計画が変わったときだけ出し直す
            const auto submit = [&](const std::vector<std::vector<int>>& plan, const std::string& source) {
                if (plan == adopted || clock_.now() >= deadline.stop_at) return;
                // 最後の計画は、16x16 と最終日では公式の点が良くなるときだけ出す。同じ点で出し直すと回答時間で負けるため
                // （練習場で上位が同点に並び、回答時間で順位を落とした）。24x24・32x32 の途中の日は、翌日向けの位置取りで
                // 玉が 1% ほど増えるので今までどおり出す
                const bool strict_final = source != "solver-interim" &&
                    (setting.value().map.width() <= 16 || day_state.day == total_days - 1);
                if (source == "solver-interim" || strict_final) {
                    const auto candidate = simulator::simulate_day(sim_input, solver::toActionPlan(plan));
                    if (candidate) {
                        const auto candidate_score = simulator::official_score(previous_progress, candidate.value());
                        const auto adopted_score = simulator::official_score(previous_progress,
                            simulator::DaySimulationResult{last_record.simulation.end_agents,
                                                           last_record.simulation.brands,
                                                           last_record.simulation.total_balls});
                        if (!simulator::better_official_score(candidate_score, adopted_score)) return;
                    }
                }
                auto sent = competition.submit_plan(solver::toActionPlan(plan), config_.mode == RunMode::DryRun,
                                                    deadline.stop_at, std::nullopt);
                if (!sent) {
                    log_daily_failure(day_state.day, "transport-or-deadline", sent.error().message,
                                      sent.error().submission_attempted, "daily-submit");
                    // 分かっている失敗なら前の提出が残る。送れたか分からないときも止めずに続け、次の提出で上書きする
                    // （adopted は前のままなので、同じ計画が最後にもう一度出される）
                    const bool unknown = !sent.error().submission_attempted.has_value();
                    if (unknown) {
                        ++unknown_posts;
                        static_cast<void>(competition.continue_after_unknown_action_post());
                        if (config_.mode == RunMode::Execute) static_cast<void>(competition.save(state_path));
                    }
                    output_ << "warning=daily-post-" << (unknown ? "outcome-unknown" : "not-accepted")
                            << " day=" << day_state.day << " source=" << source
                            << " reason=\"" << sent.error().message << "\" continuing\n";
                    return;
                }
                adopted = plan;
                adopted_source = source;
                last_record = sent.value();
                output_ << "post=" << source << ' ' << (sent.value().revision ? "success" : "dry-run") << '\n';
                if (config_.mode == RunMode::Execute) static_cast<void>(competition.save(state_path));
            };

            // LAN worker: 日の始めに依頼を出し、自分の計画が終わってから集める
            std::vector<PendingWorker> pending;
            const char* worker_secret = config_.lan_workers.empty() ? nullptr
                : std::getenv(config_.lan_worker_secret_environment.c_str());
            if (worker_secret != nullptr && *worker_secret != '\0') {
                auto worker_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline.stop_at - clock_.now()) - std::chrono::milliseconds{1000};
                worker_budget = std::min(worker_budget, std::chrono::milliseconds{59000});
                if (config_.lan_worker_timeout.count() > 0) worker_budget = std::min(worker_budget, config_.lan_worker_timeout);
                nlohmann::json request = {
                    {"requestId", daily_snapshot_identity(setting.value(), day_state)},
                    {"evaluatorVersion", worker_evaluator_identity()},
                    {"day", day_state.day}, {"size", setting.value().map.height()},
                    {"agentCount", day_state.own_agents.size()},
                    {"typeIdentity", type_identity(selected_kinds)},
                    {"snapshotIdentity", daily_snapshot_identity(setting.value(), day_state)},
                    {"mapIdentity", map_identity_digest(setting.value().map)},
                    {"stateIdentity", agent_state_identity(day_state)},
                    {"policyIdentity", nullptr},
                    {"workerBudgetMs", worker_budget.count()},
                    {"futureSnapshotRead", false}, {"lookahead", 0}};
                for (std::size_t index = 0; worker_budget.count() > 0 && index < config_.lan_workers.size(); ++index) {
                    auto worker_request = request;
                    worker_request["workerIndex"] = index;
                    worker_request["workerCount"] = config_.lan_workers.size();
                    worker_request["plannerInput"] = canonical_planner_input(setting.value(), day_state, previous_progress,
                        selected_kinds, config_.planner_seed + 1 + index, worker_budget.count());
                    worker_request["payloadHash"] = canonical_json_hash(worker_request["plannerInput"]);
                    const auto endpoint = config_.lan_workers[index];
                    const std::string secret{worker_secret};
                    const auto timeout = worker_budget + std::chrono::milliseconds{500};
                    pending.push_back({worker_request, std::async(std::launch::async,
                        [endpoint, secret, worker_request, timeout] {
                            return request_lan_worker(endpoint, secret, worker_request, timeout);
                        }), clock_.now()});
                }
            }

            // 1 日目の先読みが終わるまで待つ（同じ大域変数を使うため）
            if (day0_planning.valid()) day0_planning.get();
            solver::today = day_state.day;
            solver::interimSec = config_.interim_seconds;
            solver::interimSink = [&](const std::vector<std::vector<int>>& plan) { submit(plan, "solver-interim"); };
            const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline.stop_at - clock_.now()).count();
            // procon2026 の solver と同じく残り時間の 85% を計画に使う
            const auto plan = solver::planDay(solver::agentsOf(day_state.own_agents), solver::statusOf(day_state.traffic),
                                            std::vector<char>(solver::B, 0), steps, day_state.day == total_days - 1,
                                            static_cast<double>(std::max<std::int64_t>(0, remaining_ms)) * 0.85);
            solver::interimSink = nullptr;
            const auto local = simulator::simulate_day(sim_input, solver::toActionPlan(plan));
            std::optional<WorkerCandidate> best_worker;
            nlohmann::json worker_observations = nlohmann::json::array();
            for (std::size_t index = 0; index < pending.size(); ++index) {
                auto& worker = pending[index];
                nlohmann::json observation = {{"workerIndex", index}, {"adoption", "local-retained"}};
                const auto reply = worker.reply.get();
                if (!reply.success) {
                    observation["rejectionReason"] = reply.failure_classification.empty()
                        ? classify_worker_failure(reply.error, clock_.now() >= deadline.stop_at)
                        : reply.failure_classification;
                    worker_observations.push_back(observation);
                    output_ << "warning=lan-worker " << observation["rejectionReason"].get<std::string>() << '\n';
                    continue;
                }
                try {
                    simulator::RawDayActionPlan raw;
                    for (const auto& row : reply.payload.at("actions")) raw.push_back(row.get<std::vector<std::int32_t>>());
                    const auto parsed = simulator::parse_action_plan(raw);
                    if (!parsed) { observation["rejectionReason"] = "protocol-failure"; worker_observations.push_back(observation); continue; }
                    const auto simulation = simulator::simulate_day(sim_input, parsed.value());
                    if (!simulation) {
                        observation["rejectionReason"] = "strict-revalidation-failed";
                        worker_observations.push_back(observation);
                        continue;
                    }
                    // 依頼と違う条件で解いた計画は使わない
                    const auto actions = reply.payload.at("actions");
                    nlohmann::json mismatches = nlohmann::json::array();
                    const auto compare = [&](const std::string& field, const nlohmann::json& expected, const nlohmann::json& actual) {
                        if (expected != actual) mismatches.push_back(field);
                    };
                    compare("requestIdDigest", protocol::request_id_digest_or_missing(worker.request.at("requestId")),
                            reply.payload.value("requestIdDigest", "missing"));
                    compare("inputHash", worker.request.at("payloadHash"), reply.payload.value("inputHash", nlohmann::json(nullptr)));
                    compare("evaluatorVersion", worker.request.at("evaluatorVersion"), reply.payload.value("evaluatorVersion", nlohmann::json(nullptr)));
                    compare("mapIdentity", worker.request.at("mapIdentity"), reply.payload.value("mapIdentity", nlohmann::json(nullptr)));
                    compare("seed", worker.request.at("plannerInput").at("plannerSeed"), reply.payload.value("plannerSeed", nlohmann::json(nullptr)));
                    compare("workerIndex", worker.request.at("workerIndex"), reply.payload.value("workerIndex", nlohmann::json(nullptr)));
                    compare("workerCount", worker.request.at("workerCount"), reply.payload.value("workerCount", nlohmann::json(nullptr)));
                    compare("workerBuildFingerprint", worker_build_fingerprint(), reply.payload.value("workerBuildFingerprint", nlohmann::json(nullptr)));
                    compare("workerProtocolSchemaVersion", worker_protocol_schema_version(),
                            reply.payload.value("workerProtocolSchemaVersion", nlohmann::json(nullptr)));
                    compare("actionHash", canonical_json_hash(actions), reply.payload.value("actionHash", nlohmann::json(nullptr)));
                    const auto score = simulator::official_score(previous_progress, simulation.value());
                    compare("score", nlohmann::json::array({score.total_unique_brands, score.cumulative_daily_unique_brands, score.total_bowls}),
                            reply.payload.value("officialScore", nlohmann::json(nullptr)));
                    if (!mismatches.empty()) {
                        observation["rejectionReason"] = "claim-mismatch";
                        observation["claimMismatchFields"] = mismatches;
                        worker_observations.push_back(observation);
                        output_ << "warning=lan-worker claim-mismatch\n";
                        continue;
                    }
                    observation["score"] = nlohmann::json::array({score.total_unique_brands, score.cumulative_daily_unique_brands, score.total_bowls});
                    if (!best_worker || simulator::better_official_score(score, best_worker->score))
                        best_worker = WorkerCandidate{parsed.value(), simulation.value(), score, canonical_json_hash(actions), index};
                } catch (...) {
                    observation["rejectionReason"] = "candidate-response-schema-invalid";
                }
                worker_observations.push_back(observation);
            }
            // worker の計画は、自分の計画より公式の点が高いときだけ使う
            auto final_plan = plan;
            std::string final_source = "solver";
            if (best_worker && (!local || simulator::better_official_score(best_worker->score,
                    simulator::official_score(previous_progress, local.value())))) {
                final_plan = solver::fromActionPlan(best_worker->plan);
                final_source = "worker";
                worker_observations[best_worker->worker_index]["adoption"] = "worker";
            }
            if (local || final_source == "worker") submit(final_plan, final_source);
            const auto& adopted_simulation = last_record.simulation;
            nlohmann::json record = {{"day", day_state.day}, {"planner", "solver"},
                {"candidateSource", adopted_source}, {"adoptionReason", adopted_source},
                {"predictedBalls", adopted_simulation.total_balls},
                {"predictedBrands", adopted_simulation.brands},
                {"unknownPosts", unknown_posts},
                {"workerObservations", worker_observations}};
            const auto day_score = simulator::official_score(previous_progress,
                {adopted_simulation.end_agents, {adopted_simulation.brands.begin(), adopted_simulation.brands.end()},
                 adopted_simulation.total_balls});
            record["score"] = nlohmann::json::array({day_score.total_unique_brands,
                day_score.cumulative_daily_unique_brands, day_score.total_bowls});
            competition.record_daily_planning(record);
            if (config_.mode == RunMode::DryRun) dry_processed.insert(day_state.day);
            else {
                auto saved = competition.save(state_path);
                if (!saved) return {RunStatus::Failed, saved.error().message};
            }
            print_day_summary(setting.value(), day_state, last_record, competition.progress(), record);
        }

        if (daily.value().day == total_days - 1) {
            const auto end = unix_time(daily.value().ends_at);
            if (clock_.wall_now() < end) {
                const auto duration = end - clock_.wall_now();
                clock_.wait_until(clock_.now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
            }
            if (config_.mode == RunMode::Execute) {
                auto saved = competition.save(state_path);
                if (!saved) return {RunStatus::Failed, saved.error().message};
            }
            return stop_requested_() ? RunResult{RunStatus::Stopped, "stop requested"}
                                          : RunResult{RunStatus::Completed, "all days completed"};
        }
    }
    if (config_.mode == RunMode::Execute) {
        const auto saved_stop = competition.save(state_path);
        if (!saved_stop) return {RunStatus::Failed, saved_stop.error().message};
    }
    return {RunStatus::Stopped, "stop requested"};
}

}  // namespace hexa_udon::app
