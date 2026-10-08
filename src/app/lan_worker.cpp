#include "hexa_udon/app/lan_worker.hpp"
#include "hexa_udon/solver.hpp"
#include "hexa_udon/simulator/simulator.hpp"
#include "hexa_udon/protocol/json_codec.hpp"
#include "hexa_udon/protocol/request_id_digest.hpp"
#include "hexa_udon/protocol/file_security.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <string_view>

namespace hexa_udon::app {
namespace {

constexpr int protocol_version = 1;
constexpr auto request_frame_timeout = std::chrono::seconds{1};
constexpr auto default_reply_grace = std::chrono::milliseconds{100};

#ifndef HEXA_UDON_BUILD_FINGERPRINT
#define HEXA_UDON_BUILD_FINGERPRINT "unknown"
#endif
constexpr std::string_view evaluator_identity = "solver-v1";

std::string hex_digest(std::string_view value) {
    // This is an identity/MAC token for the local protocol, not a password
    // transport. The shared secret is never placed in a JSON frame or log.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

bool wait_fd(int fd, bool writable, std::chrono::milliseconds timeout);

enum class FrameReadStatus { Success, Timeout, Eof, Oversized };

struct FrameReadResult {
    FrameReadStatus status = FrameReadStatus::Eof;
    std::string line;
};

bool write_all(int fd, std::string_view data,
               const std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto timeout = remaining_worker_timeout(deadline, std::chrono::steady_clock::now());
        if (timeout.count() <= 0 || !wait_fd(fd, true, timeout)
            || std::chrono::steady_clock::now() >= deadline) return false;
        const auto written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

FrameReadResult read_line(
    int fd, std::size_t maximum, const std::chrono::steady_clock::time_point deadline) {
    std::string result;
    result.reserve(std::min<std::size_t>(maximum, 4096));
    std::array<char, 1024> buffer{};
    while (result.size() <= maximum) {
        const auto timeout = remaining_worker_timeout(deadline, std::chrono::steady_clock::now());
        if (timeout.count() <= 0 || !wait_fd(fd, false, timeout)
            || std::chrono::steady_clock::now() >= deadline) {
            return {FrameReadStatus::Timeout, {}};
        }
        const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return {FrameReadStatus::Eof, {}};
        result.append(buffer.data(), static_cast<std::size_t>(count));
        const auto end = result.find('\n');
        if (end != std::string::npos) {
            if (end > maximum) return {FrameReadStatus::Oversized, {}};
            result.resize(end);
            return {FrameReadStatus::Success, std::move(result)};
        }
    }
    return {FrameReadStatus::Oversized, {}};
}

bool wait_fd(int fd, bool writable, std::chrono::milliseconds timeout) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
    const auto result = ::select(fd + 1, writable ? nullptr : &set,
                                 writable ? &set : nullptr, nullptr, &tv);
    return result > 0 && FD_ISSET(fd, &set);
}

nlohmann::json response_failure(const std::string& reason,
                                const std::string& termination = "worker-failure") {
    return { {"protocolVersion", protocol_version}, {"success", false},
             {"termination", termination}, {"failureReason", reason},
             {"futureSnapshotRead", false}, {"lookahead", 0},
             {"networkRequests", 0}, {"postCount", 0} };
}

class WorkerDiagnosticLog {
public:
    explicit WorkerDiagnosticLog(const LanWorkerConfig& config) :
        run_id_(config.run_id.empty() ? "unspecified" : config.run_id),
        worker_index_(config.worker_index), worker_count_(config.worker_count) {
        if (config.diagnostic_log.empty()) return;
        std::error_code error;
        const auto parent = config.diagnostic_log.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, error);
        if (error || (!parent.empty() && !protocol::secure_directory(parent))) return;
        file_.open(config.diagnostic_log, std::ios::app);
        if (file_) static_cast<void>(protocol::secure_file(config.diagnostic_log));
    }

    [[nodiscard]] bool available() const noexcept { return file_.is_open() && file_.good(); }

    void phase(const std::string& name, const std::string& status,
               const std::string& classification, std::int64_t elapsed_ms) noexcept {
        if (!file_) return;
        try {
            const nlohmann::json row = {
                {"runId", run_id_}, {"workerIndex", worker_index_},
                {"workerCount", worker_count_}, {"phase", name}, {"status", status},
                {"failureClassification", classification}, {"elapsedMs", elapsed_ms}};
            file_ << row.dump() << '\n';
            file_.flush();
        } catch (...) {
            // Diagnostics must never change planner or fallback behavior.
        }
    }

private:
    std::ofstream file_;
    std::string run_id_;
    std::size_t worker_index_;
    std::size_t worker_count_;
};

struct DecodedInput {
    core::MatchConfig match;
    core::DailyState daily;
    simulator::MatchProgress progress;
    std::vector<core::AgentKind> types;
    std::uint64_t seed = 30013;
    std::chrono::milliseconds budget{0};
};

std::optional<DecodedInput> decode_input(const nlohmann::json& request, std::string& error) {
    try {
        const auto& input = request.at("plannerInput");
        static const std::set<std::string> allowed_input{
            "map", "startsAt", "daySeconds", "daySteps", "spots", "initialPositions",
            "fuelLimit", "players", "busyThreshold", "jammedThreshold", "daily", "progress",
            "types", "plannerSeed", "workerBudgetMs"};
        for (const auto& item : input.items())
            if (!allowed_input.contains(item.key())) { error = "unknown planner input field"; return std::nullopt; }
        const auto& map = input.at("map");
        std::vector<core::Terrain> cells;
        for (const auto& value : map.at("cells")) {
            const auto terrain = core::terrain_from_int(value.get<std::int32_t>());
            if (!terrain) { error = "invalid map terrain"; return std::nullopt; }
            cells.push_back(*terrain);
        }
        auto map_result = core::MapDefinition::create(map.at("height"), map.at("width"), std::move(cells));
        if (!map_result) { error = "invalid map"; return std::nullopt; }
        const auto starts_at = input.value("startsAt", 0LL);
        const auto day_seconds = input.at("daySeconds").get<std::vector<core::Quantity>>();
        const auto day_steps = input.at("daySteps").get<std::vector<core::Quantity>>();
        std::vector<core::Spot> spots;
        for (const auto& spot : input.at("spots")) spots.push_back({spot.at("brand"), {spot.at("position")}, spot.at("stock")});
        std::vector<core::CellIndex> initial_positions;
        for (const auto& position : input.at("initialPositions")) initial_positions.push_back({position.get<std::int32_t>()});
        const core::MatchConfig match{starts_at, day_seconds, day_steps, std::move(map_result).value(),
            std::move(spots), std::move(initial_positions), input.at("fuelLimit"), input.value("players", 1),
            input.value("busyThreshold", 1), input.value("jammedThreshold", 2)};
        core::DailyState daily{}; daily.ends_at = input.at("daily").at("endsAt"); daily.day = input.at("daily").at("day");
        for (const auto& agent : input.at("daily").at("agents")) {
            const auto kind = core::agent_kind_from_int(agent.at("kind"));
            if (!kind) { error = "invalid agent kind"; return std::nullopt; }
            daily.own_agents.push_back({*kind, {agent.at("position")}, agent.at("fuel")});
        }
        for (const auto& road : input.at("daily").at("traffic")) {
            const auto status = core::road_status_from_int(road.at("status"));
            if (!status) { error = "invalid road status"; return std::nullopt; }
            daily.traffic.push_back({{road.at("position")}, *status});
        }
        simulator::MatchProgress progress{};
        for (const auto& brand : input.at("progress").at("acquiredBrands")) progress.acquired_brands.insert(brand.get<core::Quantity>());
        progress.total_balls = input.at("progress").at("totalBalls");
        progress.daily_distinct_brand_counts = input.at("progress").at("dailyDistinctBrandCounts").get<std::vector<core::Quantity>>();
        std::vector<core::AgentKind> types;
        for (const auto& value : input.at("types")) {
            const auto kind = core::agent_kind_from_int(value.get<std::int32_t>());
            if (!kind) { error = "invalid type"; return std::nullopt; }
            types.push_back(*kind);
        }
        if (types.size() != daily.own_agents.size() || daily.own_agents.size() != match.initial_agent_positions.size()) {
            error = "agent count mismatch"; return std::nullopt;
        }
        DecodedInput decoded{std::move(match), std::move(daily), std::move(progress), std::move(types),
                              input.at("plannerSeed"), std::chrono::milliseconds{input.at("workerBudgetMs").get<std::int64_t>()}};
        if (decoded.budget.count() <= 0 || decoded.budget > std::chrono::minutes{1}) { error = "invalid worker budget"; return std::nullopt; }
        if (!core::validate(decoded.match).empty() || !core::validate(decoded.daily, decoded.match).empty()) {
            error = "model validation failed"; return std::nullopt;
        }
        return decoded;
    } catch (...) { error = "planner input schema invalid"; return std::nullopt; }
}

nlohmann::json score_json(const simulator::OfficialScore& score) {
    return {score.total_unique_brands, score.cumulative_daily_unique_brands, score.total_bowls};
}

std::string value_hash(const nlohmann::json& value) { return hex_digest(value.dump()); }

nlohmann::json agents_json(const std::vector<core::AgentState>& agents) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& agent : agents) {
        result.push_back({{"kind", core::to_int(agent.kind)},
                          {"position", agent.position.value}, {"fuel", agent.fuel}});
    }
    return result;
}

}  // namespace

std::chrono::milliseconds remaining_worker_timeout(
    const std::chrono::steady_clock::time_point deadline,
    const std::chrono::steady_clock::time_point now) noexcept {
    if (now >= deadline) return std::chrono::milliseconds{0};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return std::max(std::chrono::milliseconds{1}, remaining);
}

std::chrono::steady_clock::time_point worker_reply_deadline(
    const std::chrono::steady_clock::time_point planning_started,
    const std::chrono::milliseconds worker_budget,
    const std::chrono::milliseconds reply_grace) noexcept {
    return planning_started + worker_budget + reply_grace;
}

std::string classify_worker_failure(const std::string& error, const bool deadline_reached) {
    if (error.find("connection") != std::string::npos
        || error.find("address resolution") != std::string::npos) return "connect-failure";
    if (error.find("write timeout") != std::string::npos) return "write-timeout";
    if (error.find("read timeout") != std::string::npos) return "read-timeout";
    if (error.find("reply eof") != std::string::npos) return "reply-eof";
    if (error.find("authentication") != std::string::npos
        || error.find("protocol mismatch") != std::string::npos
        || error.find("identity-mismatch") != std::string::npos
        || error.find("identity mismatch") != std::string::npos
        || error.find("future-snapshot") != std::string::npos) return "auth-protocol-mismatch";
    if (error.find("malformed") != std::string::npos
        || error.find("oversized") != std::string::npos
        || error.find("decode") != std::string::npos) return "frame-decode-failure";
    if (error.find("planner") != std::string::npos
        || error.find("optimizer") != std::string::npos
        || error.find("action-encoding") != std::string::npos) return "planner-failure";
    if (deadline_reached) return "deadline-exhausted";
    return "transport-failure";
}

std::optional<LanWorkerEndpoint> parse_lan_worker_endpoint(const std::string& value) {
    const auto separator = value.rfind(':');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) return std::nullopt;
    try {
        const auto port = std::stoul(value.substr(separator + 1));
        if (port == 0 || port > 65535) return std::nullopt;
        LanWorkerEndpoint endpoint{value.substr(0, separator), static_cast<unsigned short>(port)};
        if (!is_allowed_worker_address(endpoint.host)) return std::nullopt;
        return endpoint;
    } catch (...) {
        return std::nullopt;
    }
}

bool is_allowed_worker_address(const std::string& host) {
    if (host == "localhost" || host == "127.0.0.1" || host == "::1") return true;
    in_addr address{};
    if (::inet_pton(AF_INET, host.c_str(), &address) != 1) return false;
    const auto value = ntohl(address.s_addr);
    return (value >> 24U) == 10U || (value >> 20U) == 0xAC1U
        || (value >> 16U) == 0xC0A8U;
}

std::string worker_auth_digest(const std::string& secret, const nlohmann::json& request) {
    nlohmann::json canonical = request;
    canonical.erase("auth");
    return hex_digest(secret + "\n" + canonical.dump());
}

std::string map_identity_digest(const core::MapDefinition& map) {
    std::ostringstream canonical;
    canonical << map.height() << 'x' << map.width() << ':';
    for (const auto terrain : map.cells()) canonical << core::to_int(terrain) << ',';
    return hex_digest(canonical.str());
}

std::string worker_build_fingerprint() { return HEXA_UDON_BUILD_FINGERPRINT; }
std::string worker_evaluator_identity() { return std::string{evaluator_identity}; }
int worker_protocol_schema_version() noexcept { return protocol_version; }

LanWorkerReply request_lan_worker(const LanWorkerEndpoint& endpoint,
                                  const std::string& secret,
                                  const nlohmann::json& request,
                                  std::chrono::milliseconds timeout,
                                  std::size_t maximum_frame_bytes) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (secret.empty()) return {false, {}, "worker secret is empty", "auth-protocol-mismatch"};
    if (request.dump().size() > maximum_frame_bytes) return {false, {}, "request is oversized", "frame-decode-failure"};
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    const auto service = std::to_string(endpoint.port);
    addrinfo* resolved = nullptr;
    if (::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &resolved) != 0) {
        return {false, {}, "worker address resolution failed", "connect-failure"};
    }
    int fd = -1;
    for (auto* item = resolved; item != nullptr; item = item->ai_next) {
        fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) continue;
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const auto connect_timeout = remaining_worker_timeout(deadline, std::chrono::steady_clock::now());
        const auto connected = connect_timeout.count() > 0
            && (::connect(fd, item->ai_addr, item->ai_addrlen) == 0
                || (errno == EINPROGRESS && wait_fd(fd, true, connect_timeout)));
        if (connected) {
            break;
        }
        ::close(fd); fd = -1;
    }
    ::freeaddrinfo(resolved);
    if (fd < 0) return {false, {}, "worker connection failed", "connect-failure"};
    nlohmann::json frame = request;
    frame["protocolVersion"] = protocol_version;
    frame["auth"] = worker_auth_digest(secret, frame);
    const auto encoded = frame.dump() + "\n";
    if (!write_all(fd, encoded, deadline)) {
        ::close(fd); return {false, {}, "worker write timeout", "write-timeout"};
    }
    const auto line = read_line(fd, maximum_frame_bytes, deadline);
    ::close(fd);
    if (line.status == FrameReadStatus::Timeout)
        return {false, {}, "worker read timeout", "read-timeout"};
    if (line.status == FrameReadStatus::Eof)
        return {false, {}, "worker reply eof", "reply-eof"};
    if (line.status == FrameReadStatus::Oversized)
        return {false, {}, "worker reply oversized", "frame-decode-failure"};
    try {
        const auto payload = nlohmann::json::parse(line.line);
        if (payload.value("protocolVersion", 0) != protocol_version)
            return {false, payload, "worker protocol mismatch", "auth-protocol-mismatch"};
        const auto error = payload.value("failureReason", "");
        if (!payload.value("success", false)) {
            return {false, payload, error,
                    classify_worker_failure(error, false)};
        }
        return {true, payload, {}, {}};
    } catch (...) { return {false, {}, "worker reply malformed", "frame-decode-failure"}; }
}

LanWorkerReply request_lan_worker_preflight(const LanWorkerEndpoint& endpoint,
                                            const std::string& secret,
                                            const std::size_t expected_worker_index,
                                            const std::size_t expected_worker_count,
                                            const std::chrono::milliseconds timeout) {
    const nlohmann::json request = {
        {"preflight", true}, {"requestId", "worker-preflight"},
        {"workerIndex", expected_worker_index}, {"workerCount", expected_worker_count}};
    auto reply = request_lan_worker(endpoint, secret, request, timeout);
    if (!reply.success) return reply;
    if (!reply.payload.value("preflight", false)
        || reply.payload.value("workerProtocolSchemaVersion", 0) != protocol_version
        || reply.payload.value("logicalWorkerIndex", expected_worker_index + 1) != expected_worker_index
        || reply.payload.value("logicalWorkerCount", 0U) != expected_worker_count
        || reply.payload.value("workerBuildFingerprint", "").empty()
        || reply.payload.value("workerEvaluatorIdentity", "").empty()) {
        return {false, reply.payload, "worker preflight identity mismatch", "auth-protocol-mismatch"};
    }
    return reply;
}

int run_lan_worker(const LanWorkerConfig& config, const std::function<bool()>& stop_requested,
                   std::ostream& output) {
    if (!is_allowed_worker_address(config.listen.host) || config.listen.port == 0) {
        output << "worker-error=address-not-private\n"; return 2;
    }
    const char* secret = std::getenv(config.secret_environment.c_str());
    if (secret == nullptr || *secret == '\0') { output << "worker-error=secret-missing\n"; return 2; }
    WorkerDiagnosticLog diagnostics(config);
    if (!config.diagnostic_log.empty() && !diagnostics.available()) {
        output << "worker-error=diagnostic-log\n";
        return 1;
    }
    const int server = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) { output << "worker-error=socket\n"; return 1; }
    int reuse = 1; ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(config.listen.port);
    const auto bind_result = config.listen.host == "localhost"
        ? (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1)
        : (::inet_pton(AF_INET, config.listen.host.c_str(), &address.sin_addr) == 1);
    if (!bind_result
        || ::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
        || ::listen(server, 4) != 0) { ::close(server); output << "worker-error=listen\n"; return 1; }
    output << "worker=ready\n";
    while (!stop_requested()) {
        if (!wait_fd(server, false, std::chrono::milliseconds{100})) continue;
        const int client = ::accept(server, nullptr, nullptr);
        if (client < 0) continue;
        const auto request_started = std::chrono::steady_clock::now();
        diagnostics.phase("request-received", "started", "", 0);
        const auto request_deadline = std::chrono::steady_clock::now() + request_frame_timeout;
        auto reply_deadline = request_deadline;
        const auto flags = ::fcntl(client, F_GETFL, 0);
        if (flags >= 0) static_cast<void>(::fcntl(client, F_SETFL, flags | O_NONBLOCK));
        const auto line = read_line(client, config.maximum_frame_bytes, request_deadline);
        nlohmann::json reply;
        nlohmann::json request = nlohmann::json::object();
        if (line.status != FrameReadStatus::Success) {
            const auto classification = line.status == FrameReadStatus::Timeout
                ? "read-timeout" : "frame-decode-failure";
            diagnostics.phase("auth-protocol", "rejected", classification, 0);
            reply = response_failure(
                line.status == FrameReadStatus::Timeout ? "request-read-timeout" : "malformed-or-oversized-request");
        }
        else try {
            request = nlohmann::json::parse(line.line);
            static const std::set<std::string> allowed_request{
                "protocolVersion", "requestId", "auth", "evaluatorVersion", "day", "size",
                "agentCount", "typeIdentity", "snapshotIdentity", "mapIdentity", "stateIdentity",
                "policyIdentity", "workerBudgetMs", "futureSnapshotRead", "lookahead", "startAgents",
                "traffic", "plannerInput", "payloadHash", "workerIndex", "workerCount", "preflight"};
            bool unknown = false;
            for (const auto& item : request.items()) unknown = unknown || !allowed_request.contains(item.key());
            const auto supplied = request.value("auth", "");
            if (request.value("protocolVersion", 0) != protocol_version
                || supplied != worker_auth_digest(secret, request) || unknown) {
                diagnostics.phase("auth-protocol", "rejected", "auth-protocol-mismatch", 0);
                reply = response_failure("authentication-or-protocol-mismatch");
            } else if (request.value("futureSnapshotRead", false)
                       || request.value("lookahead", 0) != 0) {
                diagnostics.phase("auth-protocol", "accepted", "", 0);
                reply = response_failure("future-snapshot-forbidden");
            } else if (request.value("preflight", false)) {
                diagnostics.phase("auth-protocol", "accepted", "", 0);
                const auto requested_index = request.value("workerIndex", config.worker_index);
                const auto requested_count = request.value("workerCount", config.worker_count);
                const bool identity_ok = requested_index == config.worker_index
                    && requested_count == config.worker_count;
                reply = { {"protocolVersion", protocol_version}, {"success", identity_ok},
                          {"preflight", true}, {"failureReason", identity_ok ? "" : "worker identity mismatch"},
                          {"workerBuildFingerprint", worker_build_fingerprint()},
                          {"workerProtocolSchemaVersion", worker_protocol_schema_version()},
                          {"workerEvaluatorIdentity", config.evaluator_identity.empty()
                              ? worker_evaluator_identity() : config.evaluator_identity},
                          {"workerProfileIdentity", config.profile_identity},
                          {"logicalWorkerIndex", config.worker_index},
                          {"logicalWorkerCount", config.worker_count},
                          {"futureSnapshotRead", false}, {"lookahead", 0},
                          {"networkRequests", 0}, {"postCount", 0} };
            } else {
                diagnostics.phase("auth-protocol", "accepted", "", 0);
                std::string decode_error;
                const auto decoded = decode_input(request, decode_error);
                if (!decoded) {
                    diagnostics.phase("planner", "failed", "frame-decode-failure", 0);
                    reply = response_failure(decode_error);
                } else {
                    const auto planning_started = std::chrono::steady_clock::now();
                    const auto planning_budget = std::max(
                        std::chrono::milliseconds{0}, decoded->budget - default_reply_grace);
                    const auto deadline = planning_started + planning_budget;
                    reply_deadline = worker_reply_deadline(planning_started, planning_budget,
                                                           default_reply_grace);
                    diagnostics.phase("planner", "started", "", 0);
                    if (request.value("day", -1) != decoded->daily.day
                        || request.value("size", 0) != decoded->match.map.height()
                        || request.value("agentCount", 0) != static_cast<int>(decoded->daily.own_agents.size())
                        || request.value("payloadHash", "") != value_hash(request.at("plannerInput"))
                        || request.value("futureSnapshotRead", true)
                        || request.value("lookahead", 1) != 0) {
                        reply = response_failure("planner-input-identity-mismatch");
                        diagnostics.phase("planner", "failed", "claim-mismatch", 0);
                        reply["requestIdDigest"] = protocol::request_id_digest_or_missing(
                            request.value("requestId", nlohmann::json(nullptr)));
                        const auto encoded = reply.dump() + "\n";
                        static_cast<void>(write_all(client, encoded, reply_deadline));
                        ::close(client);
                        continue;
                    }
                    // solver で計画する。worker ごとに乱数の種を変えて、main PC と違う解を探す
                    namespace m = solver;
                    m::loadProblem(decoded->match);
                    m::rng.x = 88172645463325252ULL ^ (decoded->seed * 0x9E3779B97F4A7C15ULL);
                    m::today = decoded->daily.day;
                    m::interimSec = 0;
                    const auto steps = decoded->match.day_steps.at(static_cast<std::size_t>(decoded->daily.day));
                    const auto budget_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                    const auto planned = m::planDay(m::agentsOf(decoded->daily.own_agents), m::statusOf(decoded->daily.traffic),
                                                    std::vector<char>(m::B, 0), steps,
                                                    decoded->daily.day + 1 == static_cast<core::Quantity>(decoded->match.day_steps.size()),
                                                    static_cast<double>(std::max<std::int64_t>(0, budget_ms)) * 0.85);
                    const auto plan = m::toActionPlan(planned);
                    const simulator::DaySimulationInput sim_input{decoded->match.map, decoded->match.spots, decoded->match.fuel_limit,
                        steps, decoded->daily.own_agents, decoded->daily.traffic};
                    const auto simulation = simulator::simulate_day(sim_input, plan);
                    const auto encoded = protocol::encode_actions(plan);
                    if (!simulation || !encoded) {
                        diagnostics.phase("planner", "failed", "planner-failure", 0);
                        reply = response_failure("solver-plan-invalid", "planner_failure");
                    } else {
                        const auto actions = nlohmann::json::parse(encoded.value());
                        const nlohmann::json end_state = agents_json(simulation.value().end_agents);
                        const nlohmann::json start_state = agents_json(decoded->daily.own_agents);
                        const auto action_hash = value_hash(actions);
                        const auto plan_hash = value_hash({{"actions", actions}, {"types", decoded->types}});
                        const std::string termination = "completed";
                        reply = {{"protocolVersion", protocol_version}, {"success", true},
                                 {"requestId", request.value("requestId", "")},
                                 {"requestIdDigest", protocol::request_id_digest_or_missing(request.value("requestId", nlohmann::json(nullptr)))},
                                 {"evaluatorVersion", request.value("evaluatorVersion", "")},
                                 {"policyIdentity", request.value("policyIdentity", nlohmann::json(nullptr))},
                                 {"mapIdentity", map_identity_digest(decoded->match.map)},
                                 {"plannerSeed", decoded->seed},
                                 {"workerIndex", request.value("workerIndex", -1)},
                                 {"workerCount", request.value("workerCount", 0)},
                                 {"workerBuildFingerprint", worker_build_fingerprint()},
                                 {"workerProtocolSchemaVersion", worker_protocol_schema_version()},
                                 {"workerEvaluatorIdentity", worker_evaluator_identity()},
                                 {"workerProfileIdentity", value_hash(request.value("policyIdentity", nlohmann::json(nullptr)))},
                                 {"logicalWorkerIndex", config.worker_index},
                                 {"logicalWorkerCount", config.worker_count},
                                 {"payloadHash", value_hash(request.at("plannerInput"))},
                                 {"inputHash", value_hash(request.at("plannerInput"))},
                                 {"startStateHash", value_hash(start_state)},
                                 {"candidateValidated", true}, {"actions", actions},
                                 {"actionHash", action_hash}, {"planHash", plan_hash},
                                 {"endState", end_state}, {"endStateHash", value_hash(end_state)},
                                 {"officialScore", score_json(simulator::official_score(decoded->progress, simulation.value()))},
                                 {"workerTermination", termination},
                                 {"termination", termination},
                                 {"failureReason", ""},
                                 {"futureSnapshotRead", false}, {"lookahead", 0},
                                 {"networkRequests", 0}, {"postCount", 0}};
                        diagnostics.phase("planner", "completed", "", 0);
                    }
                }
            }
        } catch (...) { reply = response_failure("malformed-json"); }
        if (request.contains("requestId")) {
            reply["requestIdDigest"] = protocol::request_id_digest_or_missing(request.at("requestId"));
        } else {
            reply["requestIdDigest"] = "missing";
        }
        const auto encoded = reply.dump() + "\n";
        const auto reply_classification = reply.value("success", false) ? std::string{}
            : classify_worker_failure(reply.value("failureReason", ""), false);
        diagnostics.phase("reply-generated", "completed", reply_classification,
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - request_started).count());
        diagnostics.phase("reply-send", "started", "", 0);
        const bool sent = std::chrono::steady_clock::now() < reply_deadline
            && write_all(client, encoded, reply_deadline);
        diagnostics.phase("reply-send", sent ? "completed" : "failed",
                          sent ? "" : "deadline-exhausted",
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - request_started).count());
        ::close(client);
    }
    ::close(server);
    output << "worker=stopped\n";
    return 0;
}

}  // namespace hexa_udon::app
