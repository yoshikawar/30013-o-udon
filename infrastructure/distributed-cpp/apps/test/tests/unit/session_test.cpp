#include <cassert>
#include <optional>

int main() {
    std::optional<bool> before = false;
    std::optional<bool> success = true;
    std::optional<bool> unknown = std::nullopt;
    if (!(before.has_value() && !*before)) return 1;
    if (!(success.has_value() && *success)) return 1;
    if (unknown.has_value()) return 1;
    if (!(before.has_value() && success.has_value())) return 1;
    if (unknown.has_value()) return 1;
}
