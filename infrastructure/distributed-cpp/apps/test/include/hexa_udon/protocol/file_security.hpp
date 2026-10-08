#pragma once

#include <filesystem>

namespace hexa_udon::protocol {

[[nodiscard]] inline bool secure_directory(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
            | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, error);
    return !error;
}

[[nodiscard]] inline bool secure_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::permissions(
        path,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    return !error;
}

}  // namespace hexa_udon::protocol
