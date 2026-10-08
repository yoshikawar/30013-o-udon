#pragma once

#include "hexa_udon/protocol/result.hpp"

#include <filesystem>

namespace hexa_udon::session {

class PersistenceOperations {
public:
    virtual ~PersistenceOperations() = default;
    [[nodiscard]] virtual protocol::Result<bool> sync_file(
        const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual protocol::Result<bool> replace(
        const std::filesystem::path& temporary,
        const std::filesystem::path& destination) = 0;
    [[nodiscard]] virtual protocol::Result<bool> sync_directory(
        const std::filesystem::path& path) = 0;
};

class PosixPersistenceOperations final : public PersistenceOperations {
public:
    [[nodiscard]] protocol::Result<bool> sync_file(
        const std::filesystem::path& path) override;
    [[nodiscard]] protocol::Result<bool> replace(
        const std::filesystem::path& temporary,
        const std::filesystem::path& destination) override;
    [[nodiscard]] protocol::Result<bool> sync_directory(
        const std::filesystem::path& path) override;
};

}  // namespace hexa_udon::session
