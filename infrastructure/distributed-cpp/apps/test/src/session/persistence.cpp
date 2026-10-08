#include "hexa_udon/session/persistence.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hexa_udon::session {
namespace {

protocol::Result<bool> failure(const char* operation) {
    return protocol::Result<bool>::failure(
        {protocol::ErrorCode::Persistence,
         std::string{operation} + ": " + std::strerror(errno)});
}

protocol::Result<bool> sync_descriptor(int descriptor, const char* operation) {
    if (::fsync(descriptor) != 0) {
        const auto result = failure(operation);
        ::close(descriptor);
        return result;
    }
    if (::close(descriptor) != 0) {
        return failure("close");
    }
    return protocol::Result<bool>::success(true);
}

}  // namespace

protocol::Result<bool> PosixPersistenceOperations::sync_file(
    const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return failure("open temporary state file for fsync");
    }
    return sync_descriptor(descriptor, "fsync temporary state file");
}

protocol::Result<bool> PosixPersistenceOperations::replace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        return failure("rename state file");
    }
    return protocol::Result<bool>::success(true);
}

protocol::Result<bool> PosixPersistenceOperations::sync_directory(
    const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return failure("open state directory for fsync");
    }
    return sync_descriptor(descriptor, "fsync state directory");
}

}  // namespace hexa_udon::session
