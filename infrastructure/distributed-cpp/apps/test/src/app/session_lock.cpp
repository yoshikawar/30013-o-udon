#include "hexa_udon/app/auto_client.hpp"
#include "hexa_udon/protocol/file_security.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace hexa_udon::app {

SessionDirectoryLock::SessionDirectoryLock(int descriptor) : descriptor_(descriptor) {}
SessionDirectoryLock::~SessionDirectoryLock() {
    if (descriptor_ >= 0) {
        ::flock(descriptor_, LOCK_UN);
        ::close(descriptor_);
    }
}
SessionDirectoryLock::SessionDirectoryLock(SessionDirectoryLock&& other) noexcept
    : descriptor_(other.descriptor_) { other.descriptor_ = -1; }
SessionDirectoryLock& SessionDirectoryLock::operator=(SessionDirectoryLock&& other) noexcept {
    if (this != &other) {
        if (descriptor_ >= 0) { ::flock(descriptor_, LOCK_UN); ::close(descriptor_); }
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
    }
    return *this;
}

protocol::Result<SessionDirectoryLock> SessionDirectoryLock::acquire(
    const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return protocol::Result<SessionDirectoryLock>::failure(
            {protocol::ErrorCode::Persistence, "cannot create session directory: " + error.message()});
    }
    if (!protocol::secure_directory(directory)) {
        return protocol::Result<SessionDirectoryLock>::failure(
            {protocol::ErrorCode::Persistence, "cannot secure session directory"});
    }
    const auto path = directory / ".execute.lock";
    const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return protocol::Result<SessionDirectoryLock>::failure(
            {protocol::ErrorCode::Persistence, std::string{"cannot open session lock: "} + std::strerror(errno)});
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor);
        return protocol::Result<SessionDirectoryLock>::failure(
            {protocol::ErrorCode::Conflict, "another execute process owns this session directory"});
    }
    return protocol::Result<SessionDirectoryLock>::success(SessionDirectoryLock{descriptor});
}

bool SessionDirectoryLock::owns_lock() const noexcept { return descriptor_ >= 0; }

}  // namespace hexa_udon::app
