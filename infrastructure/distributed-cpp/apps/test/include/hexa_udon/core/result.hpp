#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hexa_udon::core {

enum class ValidationErrorCode {
    InvalidDimension,
    CellCountMismatch,
    OutOfRangeCell,
    NonPositiveValue,
    ValueOutOfRange,
    EmptySchedule,
    ScheduleSizeMismatch,
    InvalidEnumValue,
    InvalidTerrainCombination,
};

struct ValidationError {
    ValidationErrorCode code;
    std::string field;
    std::string message;

    [[nodiscard]] bool operator==(const ValidationError&) const = default;
};

using ValidationErrors = std::vector<ValidationError>;

template <typename T>
class Result {
public:
    [[nodiscard]] static Result success(T value)
    {
        return Result(std::move(value));
    }

    [[nodiscard]] static Result failure(ValidationErrors errors)
    {
        return Result(std::move(errors));
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return has_value();
    }

    [[nodiscard]] const T& value() const&
    {
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() &&
    {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const ValidationErrors& errors() const&
    {
        return std::get<ValidationErrors>(storage_);
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(ValidationErrors errors) : storage_(std::move(errors)) {}

    std::variant<T, ValidationErrors> storage_;
};

}  // namespace hexa_udon::core
