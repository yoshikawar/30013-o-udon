#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace hexa_udon::protocol {

// Request IDs are JSON string values on the local worker protocol.  Hash only
// the decoded UTF-8 value so JSON quoting is not part of the identity.
inline std::optional<std::string> request_id_digest(const nlohmann::json& value) {
    if (!value.is_string()) return std::nullopt;
    const auto text = value.get<std::string>();
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

inline std::string request_id_digest_or_missing(const nlohmann::json& value) {
    const auto digest = request_id_digest(value);
    return digest ? *digest : "missing";
}

}  // namespace hexa_udon::protocol
