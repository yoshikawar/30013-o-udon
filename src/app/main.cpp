#include "hexa_udon/app/auto_client.hpp"
#include "hexa_udon/app/lan_worker.hpp"
#include "hexa_udon/solver.hpp"
#include "hexa_udon/protocol/file_security.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace {

// 画面に出す内容（標準出力・標準エラー）を、そのまま log directory のファイルにも書く。
// solver のスレッドと同時に書くことがあるので、書き込みは 1 つの mutex で順番にする
class TeeBuffer : public std::streambuf {
public:
    TeeBuffer(std::streambuf* screen, std::streambuf* file, std::mutex& lock)
        : screen_(screen), file_(file), lock_(lock) {}

protected:
    int overflow(int c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        std::lock_guard guard(lock_);
        screen_->sputc(traits_type::to_char_type(c));
        file_->sputc(traits_type::to_char_type(c));
        return c;
    }
    std::streamsize xsputn(const char* text, std::streamsize count) override {
        std::lock_guard guard(lock_);
        screen_->sputn(text, count);
        file_->sputn(text, count);
        return count;
    }
    int sync() override {
        std::lock_guard guard(lock_);
        screen_->pubsync();
        file_->pubsync();
        return 0;
    }

private:
    std::streambuf* screen_;
    std::streambuf* file_;
    std::mutex& lock_;
};

// 終わるときに標準出力・標準エラーを元に戻す
struct StreamRestorer {
    std::streambuf* out;
    std::streambuf* err;
    ~StreamRestorer() {
        std::cout.rdbuf(out);
        std::cerr.rdbuf(err);
    }
};

std::string local_timestamp() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    return stamp;
}

const char* safe_error_classification(hexa_udon::protocol::ErrorCode code) noexcept {
    using ErrorCode = hexa_udon::protocol::ErrorCode;
    switch (code) {
    case ErrorCode::DnsFailure: return "dns-failure";
    case ErrorCode::ConnectionRefused: return "connection-refused";
    case ErrorCode::ConnectionTimeout: return "connection-timeout";
    case ErrorCode::TransferTimeout: return "transfer-timeout";
    case ErrorCode::Disconnected: return "disconnected";
    case ErrorCode::Auth: return "authentication-failure";
    case ErrorCode::AccessTime: return "rate-limited-or-not-ready";
    case ErrorCode::Http429: return "http-429-rate-limited";
    case ErrorCode::Http4xx: return "http-client-error";
    case ErrorCode::Http5xx: return "http-server-error";
    case ErrorCode::EmptyBody: return "empty-response";
    case ErrorCode::InvalidJson: return "invalid-json";
    case ErrorCode::InvalidSchema: return "invalid-schema";
    case ErrorCode::DeadlineExceeded: return "deadline-exceeded";
    default: return "check-failed";
    }
}

const char* run_status_label(hexa_udon::app::RunStatus status) noexcept {
    switch (status) {
    case hexa_udon::app::RunStatus::Completed: return "Success";
    case hexa_udon::app::RunStatus::Stopped: return "Stopped";
    case hexa_udon::app::RunStatus::RecoveryRequired: return "RecoveryRequired";
    case hexa_udon::app::RunStatus::Failed: return "Failed";
    }
    return "Failed";
}

namespace app = hexa_udon::app;
namespace protocol = hexa_udon::protocol;

volatile std::sig_atomic_t stop_requested = 0;
extern "C" void signal_handler(int) { stop_requested = 1; }

struct Options {
    std::string command;
    std::string base_url;
    std::string token_environment = "PROCON_TOKEN";
    std::filesystem::path session_directory = "run/session";
    std::filesystem::path log_directory = "run/log";
    std::optional<std::string> types;
    bool execute = false;
    std::int64_t poll_ms = 750;
    std::int64_t connect_timeout_ms = 2000;
    std::int64_t total_timeout_ms = 5000;
    std::int64_t safety_seconds = 3;
    std::size_t max_get_retries = 8;
    std::string log_level = "info";
    std::int64_t kind_ms = 0;
    std::int64_t interim_ms = 3000;
    std::uint64_t planner_seed = 30013;
    std::optional<std::string> worker_listen;
    std::string worker_token_environment;
    std::filesystem::path worker_log;
    std::string run_id;
    std::size_t worker_index = 0;
    std::size_t worker_count = 1;
    std::string worker_profile_identity = "solver";
    std::string worker_evaluator_identity = "solver-v1";
    std::vector<std::string> lan_worker_values;
    std::int64_t lan_worker_timeout_ms = 0;
    std::int64_t threads = 0;
};

void usage() {
    std::cerr << "Usage:\n"
              << "  hexa_udon check --base-url URL [options]\n"
              << "  hexa_udon auto --base-url URL [--execute] [--types 0,0,0,1] [options]\n"
              << "  hexa_udon recover --base-url URL [options] (dry-run by default)\n"
              << "  hexa_udon show-state --session-dir DIR\n"
              << "  hexa_udon worker --listen 127.0.0.1:PORT --worker-token-env NAME\n"
              << "  hexa_udon worker-preflight --listen HOST:PORT --worker-token-env NAME\n"
              << "Options: --token-env NAME --session-dir DIR --log-dir DIR --poll-ms N\n"
              << "         --connect-timeout-ms N --total-timeout-ms N --safety-seconds N\n"
              << "         --max-get-retries N --log-level info|warning|error\n";
    std::cerr << "         --kind-ms N (types; default: size deadline 60/90/120 s minus elapsed and safety)\n"
              << "         --interim-ms N (resubmit improved plans at this interval; 0 = final plan only) --seed N\n"
              << "         --threads N (solver threads per day; default: half of the logical processors)\n";
    std::cerr << "         --lan-worker HOST:PORT (repeatable) --lan-worker-timeout-ms N\n";
    std::cerr << "         --worker-index N --worker-count N --worker-log FILE --run-id ID\n";
}

template <typename T>
bool parse_integer(const char* text, T& output) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoll(text, &consumed);
        if (consumed != std::string{text}.size() || value < 0) return false;
        output = static_cast<T>(value);
        return true;
    } catch (...) { return false; }
}

std::optional<Options> parse_options(int argc, char** argv) {
    if (argc < 2) return std::nullopt;
    Options options;
    options.command = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string value = argv[index];
        auto next = [&]() -> const char* { return ++index < argc ? argv[index] : nullptr; };
        if (value == "--execute") options.execute = true;
        else if (value == "--base-url") { const auto* item = next(); if (!item) return {}; options.base_url = item; }
        else if (value == "--token-env") { const auto* item = next(); if (!item) return {}; options.token_environment = item; }
        else if (value == "--session-dir") { const auto* item = next(); if (!item) return {}; options.session_directory = item; }
        else if (value == "--log-dir") { const auto* item = next(); if (!item) return {}; options.log_directory = item; }
        else if (value == "--types") { const auto* item = next(); if (!item) return {}; options.types = item; }
        else if (value == "--log-level") { const auto* item = next(); if (!item) return {}; options.log_level = item; }
        else if (value == "--poll-ms") { const auto* item = next(); if (!item || !parse_integer(item, options.poll_ms)) return {}; }
        else if (value == "--connect-timeout-ms") { const auto* item = next(); if (!item || !parse_integer(item, options.connect_timeout_ms)) return {}; }
        else if (value == "--total-timeout-ms") { const auto* item = next(); if (!item || !parse_integer(item, options.total_timeout_ms)) return {}; }
        else if (value == "--safety-seconds") { const auto* item = next(); if (!item || !parse_integer(item, options.safety_seconds)) return {}; }
        else if (value == "--max-get-retries") { const auto* item = next(); if (!item || !parse_integer(item, options.max_get_retries)) return {}; }
        else if (value == "--kind-ms") { const auto* item = next(); if (!item || !parse_integer(item, options.kind_ms)) return {}; }
        else if (value == "--interim-ms") { const auto* item = next(); if (!item || !parse_integer(item, options.interim_ms)) return {}; }
        else if (value == "--seed") { const auto* item = next(); if (!item || !parse_integer(item, options.planner_seed)) return {}; }
        else if (value == "--listen") { const auto* item = next(); if (!item || options.worker_listen) return {}; options.worker_listen = item; }
        else if (value == "--worker-token-env") { const auto* item = next(); if (!item || !options.worker_token_environment.empty()) return {}; options.worker_token_environment = item; }
        else if (value == "--worker-log") { const auto* item = next(); if (!item) return {}; options.worker_log = item; }
        else if (value == "--run-id") { const auto* item = next(); if (!item) return {}; options.run_id = item; }
        else if (value == "--worker-index") { const auto* item = next(); if (!item || !parse_integer(item, options.worker_index)) return {}; }
        else if (value == "--worker-count") { const auto* item = next(); if (!item || !parse_integer(item, options.worker_count)) return {}; }
        else if (value == "--worker-profile-identity") { const auto* item = next(); if (!item) return {}; options.worker_profile_identity = item; }
        else if (value == "--worker-evaluator-identity") { const auto* item = next(); if (!item) return {}; options.worker_evaluator_identity = item; }
        else if (value == "--lan-worker") { const auto* item = next(); if (!item) return {}; options.lan_worker_values.emplace_back(item); }
        else if (value == "--threads") { const auto* item = next(); if (!item || !parse_integer(item, options.threads)) return {}; }
        else if (value == "--lan-worker-timeout-ms") { const auto* item = next(); if (!item || !parse_integer(item, options.lan_worker_timeout_ms)) return {}; }
        else return std::nullopt;
    }
    return options;
}

bool valid(const Options& options) {
    if (options.command == "worker" || options.command == "worker-preflight") {
        return options.worker_listen.has_value() && !options.worker_token_environment.empty()
            && !options.execute && options.base_url.empty() && options.lan_worker_values.empty()
            && options.token_environment == "PROCON_TOKEN"
            && app::parse_lan_worker_endpoint(*options.worker_listen).has_value()
            && options.worker_count > 0 && options.worker_index < options.worker_count;
    }
    if (!options.lan_worker_values.empty() && options.command != "auto") return false;
    if (options.lan_worker_timeout_ms < 0 || options.lan_worker_timeout_ms > 59000) return false;
    if (options.command == "show-state") return !options.execute;
    if (options.command != "check" && options.command != "auto" && options.command != "recover") return false;
    if (options.base_url.empty() || options.token_environment.empty()) return false;
    if (options.poll_ms < 500 || options.connect_timeout_ms <= 0 ||
        options.total_timeout_ms < options.connect_timeout_ms || options.safety_seconds < 1 ||
        options.max_get_retries == 0) return false;
    if (options.log_level != "info" && options.log_level != "warning" && options.log_level != "error") return false;
    if (options.command == "check" && options.execute) return false;
    return true;
}

int show_state(const std::filesystem::path& directory) {
    std::ifstream input(directory / "session.json", std::ios::binary);
    if (!input) { std::cerr << "No saved session found\n"; return 1; }
    try {
        const auto json = nlohmann::json::parse(input);
        std::cout << "matchId=" << json.at("matchId").get<std::string>() << '\n'
                  << "lastDay=" << (json.at("lastObservedDay").is_null()
                                         ? std::string{"none"}
                                         : std::to_string(json.at("lastObservedDay").get<int>())) << '\n'
                  << "typesSubmitted=" << (!json.at("submittedAgentKinds").empty() ? "yes" : "no") << '\n';
        std::set<std::int32_t> match_brands;
        std::size_t daily_brand_sum = 0;
        std::int64_t total_balls = 0;
        bool unknown = json.value("agentKindsUnknown", false);
        for (const auto& day : json.at("acceptedDays")) {
            std::cout << "day=" << day.at("day") << " revision=" << day.at("revision")
                      << " brands=" << day.at("simulation").at("brands").size()
                      << " balls=" << day.at("simulation").at("totalBalls") << '\n';
            daily_brand_sum += day.at("simulation").at("brands").size();
            for (const auto& brand : day.at("simulation").at("brands"))
                match_brands.insert(brand.get<std::int32_t>());
            total_balls += day.at("simulation").at("totalBalls").get<std::int64_t>();
        }
        for (const auto& submission : json.at("submissions"))
            unknown = unknown || submission.at("classification").get<int>() == 5;
        std::cout << "matchBrands=" << match_brands.size()
                  << " dailyBrandCountSum=" << daily_brand_sum << " totalBalls=" << total_balls
                  << " unknownResponse=" << (unknown ? "yes" : "no") << '\n';
        if (!json.at("acceptedDays").empty()) {
            const auto& agents = json.at("acceptedDays").back().at("simulation").at("endAgents");
            for (std::size_t index = 0; index < agents.size(); ++index)
                std::cout << "agent=" << index << " endPos=" << agents[index].at("position")
                          << " endFuel=" << agents[index].at("fuel") << '\n';
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Invalid saved session: " << exception.what() << '\n';
        return 1;
    }
}
}  // namespace

int main(int argc, char** argv) {
    // 標準出力を書くたびに出す。パイプ（practice.sh の tee など）につなぐと既定ではためてから出すので、
    // 提出や日ごとの結果の行が何十秒も遅れてログに出ていた
    std::cout << std::unitbuf;
    if (argc == 1 || (argc > 1 && (std::string{argv[1]} == "--help" || std::string{argv[1]} == "help"))) {
        usage();
        return 0;
    }
    const auto options = parse_options(argc, argv);
    if (!options || !valid(*options)) { usage(); return 2; }
    // 全部のスレッドを使うと通信や OS の処理が遅れるので、指定がなければ論理スレッド数の半分にする
    hexa_udon::solver::threads = options->threads > 0 ? static_cast<int>(options->threads)
        : std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2);
    if (options->command == "worker") {
        const auto endpoint = app::parse_lan_worker_endpoint(*options->worker_listen);
        if (!endpoint) { std::cerr << "worker requires a private or loopback listen address\n"; return 2; }
        std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
        app::LanWorkerConfig worker_config{*endpoint, options->worker_token_environment, 262144,
            options->worker_index, options->worker_count, options->worker_log, options->run_id,
            options->worker_profile_identity, options->worker_evaluator_identity};
        return app::run_lan_worker(worker_config,
                                   [] { return stop_requested != 0; }, std::cout);
    }
    if (options->command == "worker-preflight") {
        const auto endpoint = app::parse_lan_worker_endpoint(*options->worker_listen);
        const char* secret = std::getenv(options->worker_token_environment.c_str());
        if (!endpoint || secret == nullptr || *secret == '\0') {
            std::cerr << "worker-preflight=failed reason=secret-missing-or-endpoint-invalid\n";
            return 2;
        }
        const auto reply = app::request_lan_worker_preflight(
            *endpoint, secret, options->worker_index, options->worker_count,
            std::chrono::milliseconds{options->lan_worker_timeout_ms > 0
                ? options->lan_worker_timeout_ms : 1000});
        if (!reply.success) {
            std::cout << "worker-preflight=failed classification="
                      << (reply.failure_classification.empty() ? "unknown" : reply.failure_classification)
                      << '\n';
            return 1;
        }
        std::cout << "worker-preflight=ok protocolSchemaVersion="
                  << reply.payload.value("workerProtocolSchemaVersion", 0)
                  << " buildFingerprint=" << reply.payload.value("workerBuildFingerprint", "unknown")
                  << " workerIndex=" << reply.payload.value("logicalWorkerIndex", -1)
                  << " workerCount=" << reply.payload.value("logicalWorkerCount", 0)
                  << " evaluatorIdentity=" << reply.payload.value("workerEvaluatorIdentity", "unknown")
                  << " profileIdentity=" << reply.payload.value("workerProfileIdentity", "unknown")
                  << " secretConfigured=true\n";
        return 0;
    }
    if (options->command == "show-state") return show_state(options->session_directory);
    const char* token = std::getenv(options->token_environment.c_str());
    if (token == nullptr || *token == '\0') {
        std::cerr << "Token environment variable is missing or empty\n";
        return 2;
    }
    std::error_code error;
    std::filesystem::create_directories(options->log_directory, error);
    if (error) { std::cerr << "Cannot create log directory\n"; return 1; }
    if (!protocol::secure_directory(options->log_directory)) {
        std::cerr << "Cannot secure log directory\n"; return 1;
    }
    const auto log_level = options->log_level == "error"
        ? protocol::OperationLogEntry::Level::Error
        : options->log_level == "warning" ? protocol::OperationLogEntry::Level::Warning
                                            : protocol::OperationLogEntry::Level::Info;
    protocol::FileOperationLogger logger(options->log_directory / "operations.jsonl", log_level);
    // 画面の出力を log directory の client-output-<時刻>.log にも残す（tee を付けなくても残る）
    const auto client_log_path = options->log_directory / ("client-output-" + local_timestamp() + ".log");
    std::ofstream client_log(client_log_path);
    std::filesystem::permissions(client_log_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    std::mutex client_log_lock;
    TeeBuffer out_tee(std::cout.rdbuf(), client_log.rdbuf(), client_log_lock);
    TeeBuffer err_tee(std::cerr.rdbuf(), client_log.rdbuf(), client_log_lock);
    StreamRestorer restore_streams{std::cout.rdbuf(), std::cerr.rdbuf()};
    if (client_log) {
        std::cout.rdbuf(&out_tee);
        std::cerr.rdbuf(&err_tee);
        std::cout << "client-log=" << client_log_path.string() << '\n';
    } else {
        std::cerr << "warning=client-log cannot open " << client_log_path.string() << '\n';
    }
    protocol::CurlHttpTransport transport;
    protocol::ApiConfig api_config{options->base_url, token};
    api_config.connect_timeout = std::chrono::milliseconds{options->connect_timeout_ms};
    api_config.total_timeout = std::chrono::milliseconds{options->total_timeout_ms};
    protocol::ProconApiClient api(transport, api_config, nullptr, &logger);
    if (options->command == "check") {
        std::cout << "mode=CHECK baseUrlConfigured=true\n";
        const auto setting = api.get_setting();
        if (!setting) {
            // Never expose transport messages: curl/DNS errors can contain the configured host.
            std::cerr << "connection-check=failed code="
                      << safe_error_classification(setting.error().code) << '\n';
            return 1;
        }
        std::cout << "connection-check=ok startsAt=" << setting.value().starts_at
                  << " agents=" << setting.value().initial_agent_positions.size()
                  << " days=" << setting.value().day_steps.size() << '\n';
        return 0;
    }
    if (options->command == "recover" && !std::filesystem::exists(options->session_directory / "session.json")) {
        std::cerr << "Recovery requested but no saved session exists\n";
        return 2;
    }
    std::optional<std::vector<hexa_udon::core::AgentKind>> kinds;
    if (options->types) {
        auto parsed = app::parse_kind_list(*options->types);
        if (!parsed) { std::cerr << parsed.error().message << '\n'; return 2; }
        kinds = parsed.take();
    }
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    app::SystemAppClock clock;
    app::AutoClientConfig config;
    config.base_url = options->base_url;
    config.mode = options->execute ? app::RunMode::Execute : app::RunMode::DryRun;
    config.state_directory = options->session_directory;
    config.explicit_kinds = kinds;
    config.type_submission_reserve = std::chrono::milliseconds{
        std::max<std::int64_t>(750, options->total_timeout_ms + 250)};
    config.polling_interval = std::chrono::milliseconds{options->poll_ms};
    config.safety_margin = std::chrono::seconds{options->safety_seconds};
    config.maximum_get_attempts = options->max_get_retries;
    config.kind_budget = std::chrono::milliseconds{options->kind_ms};
    config.interim_seconds = static_cast<double>(options->interim_ms) / 1000.0;
    config.planner_seed = options->planner_seed;
    for (const auto& value : options->lan_worker_values) {
        const auto endpoint = app::parse_lan_worker_endpoint(value);
        if (!endpoint) { std::cerr << "Invalid private LAN worker endpoint\n"; return 2; }
        config.lan_workers.push_back(*endpoint);
    }
    config.lan_worker_timeout = std::chrono::milliseconds{options->lan_worker_timeout_ms};
    if (!config.lan_workers.empty()) {
        config.lan_worker_secret_environment = "HEXA_LAN_WORKER_SECRET";
        if (std::getenv(config.lan_worker_secret_environment.c_str()) == nullptr) {
            std::cerr << "HEXA_LAN_WORKER_SECRET is required for --lan-worker\n"; return 2;
        }
    }
    app::AutoCompetitionClient client(api, std::move(config), clock,
        [] { return stop_requested != 0; }, std::cout, &logger);
    const auto result = client.run();
    std::cout << "result=" << run_status_label(result.status) << '\n';
    return result.status == app::RunStatus::Completed ? 0
         : result.status == app::RunStatus::Stopped ? 130 : 1;
}
