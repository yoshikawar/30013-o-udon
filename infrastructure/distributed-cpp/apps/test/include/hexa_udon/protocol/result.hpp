#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
namespace hexa_udon::protocol {
enum class ErrorCode { DnsFailure, ConnectionRefused, ConnectionTimeout, TransferTimeout, Disconnected, ResponseTooLarge, Transport, UnknownResponse, Http4xx, Http429, Http5xx, Auth, AccessTime, EmptyBody, InvalidJson, InvalidSchema, CoreValidation, RejectedRevision, MissingRevision, Persistence, VersionMismatch, Conflict, DeadlineExceeded };
struct Error {
    ErrorCode code;
    std::string message;
    std::optional<std::int64_t> retry_after_ms;
    // false: POST was not started, true: a POST was started and received a response,
    // nullopt: the outcome after starting a POST is unknown.
    std::optional<bool> submission_attempted;
    Error(ErrorCode error_code, std::string error_message,
          std::optional<std::int64_t> retry_after = std::nullopt,
          std::optional<bool> attempted = std::nullopt)
        : code(error_code), message(std::move(error_message)), retry_after_ms(retry_after),
          submission_attempted(attempted) {}
};
template <typename T> class Result {
public:
    [[nodiscard]] static Result success(T value) { return Result(std::move(value)); }
    [[nodiscard]] static Result failure(Error error) { Result result; result.error_ = std::move(error); return result; }
    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
    [[nodiscard]] const T& value() const& { return *value_; }
    [[nodiscard]] T&& take() { return std::move(*value_); }
    [[nodiscard]] const Error& error() const& { return *error_; }
private:
    Result() = default;
    explicit Result(T value) : value_(std::move(value)) {}
    std::optional<T> value_; std::optional<Error> error_;
};
}  // namespace hexa_udon::protocol
