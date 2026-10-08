#include "hexa_udon/protocol/request_control.hpp"
#include "hexa_udon/protocol/file_security.hpp"

#include <nlohmann/json.hpp>

#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace hexa_udon::protocol {
namespace {

nlohmann::json entry_json(const OperationLogEntry& entry) {
    nlohmann::json json{{"time", entry.timestamp_utc},
                        {"level", static_cast<int>(entry.level)},
                        {"operation", entry.operation},
                        {"method", entry.method},
                        {"path", entry.path},
                        {"elapsedMs", entry.elapsed.count()},
                        {"result", entry.result},
                        {"stateTransition", entry.state_transition}};
    json["endpoint"] = entry.endpoint;
    json["phase"] = entry.phase;
    json["attempt"] = entry.attempt;
    json["requestStartedAt"] = entry.request_started_utc.empty()
                                     ? nlohmann::json(nullptr)
                                     : nlohmann::json(entry.request_started_utc);
    json["nextRetryAt"] = entry.next_retry_utc.empty()
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(entry.next_retry_utc);
    json["backoffReason"] = entry.backoff_reason;
    json["retryAfterMs"] = entry.retry_after_ms ? nlohmann::json(*entry.retry_after_ms) : nlohmann::json(nullptr);
    json["retryWaitMs"] = entry.retry_wait_ms ? nlohmann::json(*entry.retry_wait_ms) : nlohmann::json(nullptr);
    json["deadlineRemainingMs"] = entry.deadline_remaining_ms
        ? nlohmann::json(*entry.deadline_remaining_ms) : nlohmann::json(nullptr);
    json["stopReason"] = entry.stop_reason;
    json["responseClassification"] = entry.response_classification;
    json["sourceOperation"] = entry.source_operation;
    json["submissionAttempted"] = entry.submission_attempted
        ? nlohmann::json(*entry.submission_attempted) : nlohmann::json(nullptr);
    json["day"] = entry.day ? nlohmann::json(*entry.day) : nlohmann::json(nullptr);
    json["localSubmissionId"] = entry.local_submission_id
        ? nlohmann::json(*entry.local_submission_id) : nlohmann::json(nullptr);
    json["httpStatus"] = entry.http_status ? nlohmann::json(*entry.http_status) : nlohmann::json(nullptr);
    json["revision"] = entry.revision ? nlohmann::json(*entry.revision) : nlohmann::json(nullptr);
    return json;
}

bool same_logical_operation(const nlohmann::json& json, const OperationLogEntry& entry) {
    return json.value("operation", std::string{}) == entry.operation
        && json.value("localSubmissionId", nlohmann::json(nullptr))
            == (entry.local_submission_id ? nlohmann::json(*entry.local_submission_id)
                                           : nlohmann::json(nullptr));
}

}  // namespace

RequestRateLimiter::RequestRateLimiter(
    std::chrono::milliseconds minimum_interval, Now now, SleepUntil sleep_until)
    : minimum_interval_(minimum_interval), now_(std::move(now)), sleep_until_(std::move(sleep_until)) {}

Result<bool> RequestRateLimiter::acquire(std::optional<SteadyTime> deadline) {
    std::lock_guard lock(mutex_);
    auto current = now_();
    const auto send_at = next_allowed_ && *next_allowed_ > current ? *next_allowed_ : current;
    if (deadline && send_at >= *deadline) {
        return Result<bool>::failure({ErrorCode::DeadlineExceeded, "rate limit wait exceeds deadline"});
    }
    if (send_at > current) {
        sleep_until_(send_at);
        current = now_();
        if (deadline && current >= *deadline) {
            return Result<bool>::failure({ErrorCode::DeadlineExceeded, "deadline passed while rate limited"});
        }
    }
    next_allowed_ = current + minimum_interval_;
    return Result<bool>::success(true);
}

Result<bool> RequestRateLimiter::check_deadline(std::optional<SteadyTime> deadline) {
    std::lock_guard lock(mutex_);
    if (deadline && now_() >= *deadline) {
        return Result<bool>::failure({ErrorCode::DeadlineExceeded,
                                      "deadline reached before HTTP send"});
    }
    return Result<bool>::success(true);
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

FileOperationLogger::FileOperationLogger(
    std::filesystem::path path, OperationLogEntry::Level minimum_level)
    : path_(std::move(path)), minimum_level_(minimum_level) {
    std::error_code error;
    if (!path_.parent_path().empty())
        std::filesystem::create_directories(path_.parent_path(), error);
    if (!error && !std::filesystem::exists(path_)) {
        std::ofstream create(path_, std::ios::app);
        create.close();
    }
    if (!error) static_cast<void>(secure_file(path_));
}

void FileOperationLogger::write(const OperationLogEntry& entry) noexcept {
    try {
        if (entry.level < minimum_level_) return;
        std::lock_guard lock(mutex_);
        static_cast<void>(secure_file(path_));
        std::ofstream output(path_, std::ios::app);
        if (output) {
            output << entry_json(entry).dump() << '\n';
            output.flush();
        }
    } catch (...) {
        // Logging must never stop competition processing.
    }
}

void FileOperationLogger::update(const OperationLogEntry& entry) noexcept {
    try {
        if (entry.level < minimum_level_) return;
        if (!entry.local_submission_id) {
            write(entry);
            return;
        }
        std::lock_guard lock(mutex_);
        std::ifstream input(path_);
        std::vector<nlohmann::json> rows;
        std::string line;
        bool replaced = false;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            auto row = nlohmann::json::parse(line);
            if (!replaced && same_logical_operation(row, entry)) {
                row = entry_json(entry);
                replaced = true;
            }
            rows.push_back(std::move(row));
        }
        if (!replaced) {
            rows.push_back(entry_json(entry));
        }
        const auto temporary = path_.string() + ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        for (const auto& row : rows) output << row.dump() << '\n';
        output.flush();
        output.close();
        static_cast<void>(secure_file(temporary));
        std::error_code error;
        std::filesystem::rename(temporary, path_, error);
        if (!error) static_cast<void>(secure_file(path_));
        if (error) std::filesystem::remove(temporary, error);
    } catch (...) {
        // Logging must never stop competition processing.
    }
}

}  // namespace hexa_udon::protocol
