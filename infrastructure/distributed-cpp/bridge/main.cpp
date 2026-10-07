#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
volatile sig_atomic_t stopping = 0;
constexpr std::size_t max_line = 1024 * 1024;
void stop(int) { stopping = 1; }
Deadline after(int ms) { return Clock::now() + std::chrono::milliseconds(ms); }
std::string env(const char* name, const std::string& fallback = "") {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}
int number(const std::string& text, int low, int high) {
    std::size_t used = 0;
    const auto value = std::stoll(text, &used);
    if (used != text.size() || value < low || value > high)
        throw std::runtime_error("invalid numeric setting");
    return static_cast<int>(value);
}
bool valid_id(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
void configure(int fd) {
    if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("cannot configure descriptor");
}
struct Fd {
    int value = -1;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
void ready(int fd, short events, Deadline deadline) {
    while (!stopping && Clock::now() < deadline) {
        pollfd p{fd, events, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int rc = poll(&p, 1, static_cast<int>(std::clamp<long long>(remaining, 1, 100)));
        if (rc > 0 && ((p.revents & events) || (events == POLLIN && (p.revents & POLLHUP)))) return;
        if (rc > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
            throw std::runtime_error("connection closed");
        if (rc < 0 && errno != EINTR) throw std::runtime_error("poll failed");
    }
    throw std::runtime_error(stopping ? "stopping" : "timeout");
}
bool line(int fd, std::string& result, Deadline deadline) {
    result.clear();
    while (true) {
        ready(fd, POLLIN, deadline);
        char c;
        const auto size = read(fd, &c, 1);
        if (size == 0) {
            if (!result.empty()) throw std::runtime_error("unterminated frame");
            return false;
        }
        if (size < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            throw std::runtime_error("read failed");
        }
        if (c == '\n') return true;
        if (result.size() >= max_line) throw std::runtime_error("frame too large");
        result += c;
    }
}
std::string required_line(int fd, Deadline deadline) {
    std::string value;
    if (!line(fd, value, deadline)) throw std::runtime_error("connection closed");
    return value;
}
void send_line(int fd, const std::string& value, Deadline deadline) {
    if (value.size() > max_line || value.find('\n') != std::string::npos)
        throw std::runtime_error("invalid frame");
    const auto message = value + '\n';
    std::size_t offset = 0;
    while (offset < message.size()) {
        ready(fd, POLLOUT, deadline);
        const auto size = write(fd, message.data() + offset, message.size() - offset);
        if (size < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            throw std::runtime_error("write failed");
        }
        offset += static_cast<std::size_t>(size);
    }
}
std::string quote(const std::string& value) {
    const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32 || c >= 127) {
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out + '"';
}
// Child applications use newline-delimited ASCII (JSON with escaped Unicode is fine).
struct Child {
    pid_t pid = -1;
    Fd input, output;
    Child() {
        const auto executable = env("APP_EXEC", "/opt/application/bin/application");
        int in[2], out[2];
        if (pipe(in) < 0) throw std::runtime_error("pipe failed");
        Fd in_read(in[0]); input.value = in[1];
        if (pipe(out) < 0) throw std::runtime_error("pipe failed");
        Fd out_write(out[1]); output.value = out[0];
        // All four descriptors must be close-on-exec, including the child ends.
        fcntl(in_read.value, F_SETFD, FD_CLOEXEC);
        fcntl(out_write.value, F_SETFD, FD_CLOEXEC);
        configure(input.value); configure(output.value);
        pid = fork();
        if (pid < 0) throw std::runtime_error("fork failed");
        if (pid == 0) {
            setpgid(0, 0);
            dup2(in_read.value, STDIN_FILENO);
            dup2(out_write.value, STDOUT_FILENO);
            execl(executable.c_str(), "application", static_cast<char*>(nullptr));
            _exit(127);
        }
        setpgid(pid, pid);
    }
    bool successful(Deadline deadline) {
        while (!stopping && Clock::now() < deadline) {
            int status = 0;
            const auto result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                // A solver must not leave background descendants running.
                kill(-pid, SIGKILL);
                pid = -1;
                return WIFEXITED(status) && WEXITSTATUS(status) == 0;
            }
            if (result < 0 && errno != EINTR) throw std::runtime_error("wait failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    ~Child() {
        if (pid > 0) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }
};
struct Peer {
    Fd socket;
    std::string id, seed;
    std::atomic<bool> alive{true};
    explicit Peer(int fd) : socket(fd) {}
};
std::string result(const Peer& peer, const std::string& status, const std::string& payload) {
    return "{\"nodeId\":" + quote(peer.id) + ",\"seed\":" + quote(peer.seed) +
        ",\"status\":" + quote(status) + ",\"payload\":" + quote(payload) + "}";
}
[[maybe_unused]] int main_node(const std::string& secret, int port, int budget) {
    Fd listener(socket(AF_INET, SOCK_STREAM, 0)); configure(listener.value);
    int yes = 1;
    setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(port);
    if (bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener.value, 8) < 0) throw std::runtime_error("cannot listen");
    std::mutex mutex;
    std::vector<std::shared_ptr<Peer>> peers;
    std::jthread acceptor([&](std::stop_token token) {
        while (!token.stop_requested() && !stopping) {
            pollfd p{listener.value, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0 || !(p.revents & POLLIN)) continue;
            const int fd = accept(listener.value, nullptr, nullptr);
            if (fd < 0) continue;
            auto peer = std::make_shared<Peer>(fd);
            try {
                configure(fd);
                const auto deadline = after(2000);
                if (required_line(fd, deadline) != "cpp-cluster-v2" ||
                    required_line(fd, deadline) != secret) throw std::runtime_error("authentication failed");
                peer->id = required_line(fd, deadline);
                peer->seed = required_line(fd, deadline);
                if (!valid_id(peer->id) || peer->id == env("NODE_ID") || peer->seed.size() > 20 ||
                    peer->seed.empty() || peer->seed.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("invalid identity");
                std::lock_guard lock(mutex);
                for (const auto& item : peers) {
                    char byte;
                    const auto size = recv(item->socket.value, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                    if (size == 0 || (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                        item->alive = false;
                }
                std::erase_if(peers, [](const auto& item) { return !item->alive; });
                if (peers.size() >= 16 || std::any_of(peers.begin(), peers.end(), [&](const auto& item) {
                        return item->id == peer->id;
                    })) throw std::runtime_error("duplicate identity or cluster full");
                send_line(fd, "ready", deadline);
                peers.push_back(peer);
                std::cerr << "worker connected: " << peer->id << '\n';
            } catch (const std::exception&) {
                std::cerr << "worker registration rejected\n";
            }
        }
    });
    const int expected = number(env("EXPECTED_WORKERS", "2"), 0, 16);
    const auto startup = after(number(env("STARTUP_WAIT_MS", "60000"), 0, 600000));
    std::cerr << "main listening; waiting for workers\n";
    while (!stopping && Clock::now() < startup) {
        { std::lock_guard lock(mutex); if (peers.size() >= static_cast<std::size_t>(expected)) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stopping) return 0;
    { std::lock_guard lock(mutex); std::cerr << "starting application; workers=" << peers.size() << '\n'; }
    Child application;
    std::uint64_t job = 0;
    const int read_timeout = number(env("APP_READ_TIMEOUT_MS", "30000"), 1000, 600000);
    while (!stopping) {
        std::string task;
        // Main application owns its lifecycle and may wait for external input.
        if (!line(application.output.value, task, Clock::time_point::max())) break;
        const auto deadline = after(budget);
        const auto job_id = std::to_string(++job);
        std::vector<std::shared_ptr<Peer>> snapshot;
        { std::lock_guard lock(mutex); for (const auto& peer : peers) if (peer->alive) snapshot.push_back(peer); }
        std::sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) { return a->id < b->id; });
        struct Event { const Peer* peer; bool candidate; std::string json; };
        std::mutex event_mutex;
        std::condition_variable changed;
        std::deque<Event> events;
        // Bound memory without replacing an unvalidated candidate with another.
        // Reserve space for one terminal event per worker even on backpressure.
        auto publish = [&](const Peer& peer, bool candidate, const std::string& status,
                           const std::string& payload) {
            auto json = "{\"type\":" + quote(candidate ? "candidate" : "worker_done") +
                ",\"jobId\":" + quote(job_id) + ",\"result\":" + result(peer, status, payload) + "}";
            std::unique_lock lock(event_mutex);
            if (candidate) {
                while (events.size() >= 64 && !stopping && Clock::now() < deadline)
                    changed.wait_for(lock, std::chrono::milliseconds(50));
                if (stopping || Clock::now() >= deadline)
                    throw std::runtime_error("candidate consumer too slow");
            }
            events.push_back({&peer, candidate, std::move(json)});
            changed.notify_one();
        };
        std::vector<std::future<void>> futures;
        for (const auto& peer : snapshot) {
            futures.push_back(std::async(std::launch::async, [&, peer] {
                try {
                    send_line(peer->socket.value, job_id, deadline);
                    send_line(peer->socket.value, std::to_string(budget), deadline);
                    send_line(peer->socket.value, task, deadline);
                    while (true) {
                        if (required_line(peer->socket.value, deadline) != job_id)
                            throw std::runtime_error("stale job reply");
                        const auto type = required_line(peer->socket.value, deadline);
                        const auto payload = required_line(peer->socket.value, deadline);
                        if (type == "candidate") {
                            if (payload.size() > 8192) throw std::runtime_error("reply too large");
                            publish(*peer, true, "unvalidated", payload);
                        } else if (type == "done" && (payload == "ok" || payload == "error")) {
                            publish(*peer, false, payload, "");
                            break;
                        } else throw std::runtime_error("invalid reply");
                    }
                } catch (const std::exception&) {
                    peer->alive = false;
                    shutdown(peer->socket.value, SHUT_RDWR);
                    publish(*peer, false, "unavailable", "");
                }
            }));
        }
        std::size_t completed = 0;
        while (completed < snapshot.size()) {
            std::unique_lock lock(event_mutex);
            changed.wait(lock, [&] { return !events.empty(); });
            auto event = std::move(events.front()); events.pop_front();
            lock.unlock();
            changed.notify_all();
            if (!event.candidate) ++completed;
            send_line(application.input.value, event.json, after(read_timeout));
        }
        for (auto& future : futures) future.get();
        send_line(application.input.value, "{\"type\":\"done\",\"jobId\":" + quote(job_id) + "}", after(read_timeout));
    }
    return application.successful(after(5000)) ? 0 : 1;
}
[[maybe_unused]] int worker_node(const std::string& secret, int port, int budget) {
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    if (inet_pton(AF_INET, env("MAIN_HOST").c_str(), &address.sin_addr) != 1)
        throw std::runtime_error("MAIN_HOST must be the main PC wired IPv4 address");
    while (!stopping) {
        try {
            Fd connection(socket(AF_INET, SOCK_STREAM, 0)); configure(connection.value);
            const auto deadline = after(3000);
            if (connect(connection.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
                if (errno != EINPROGRESS) throw std::runtime_error("connect failed");
                ready(connection.value, POLLOUT, deadline);
                int error = 0; socklen_t length = sizeof(error);
                if (getsockopt(connection.value, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
                    throw std::runtime_error("connect failed");
            }
            send_line(connection.value, "cpp-cluster-v2", deadline);
            send_line(connection.value, secret, deadline);
            send_line(connection.value, env("NODE_ID"), deadline);
            send_line(connection.value, env("SEED"), deadline);
            if (required_line(connection.value, deadline) != "ready") throw std::runtime_error("registration failed");
            std::cerr << "connected to main\n";
            while (!stopping) {
                const auto job_id = required_line(connection.value, Clock::time_point::max());
                const int requested = number(required_line(connection.value, Clock::time_point::max()), 100, 600000);
                const auto finish = after(std::min(budget, requested) - 50);
                const auto task = required_line(connection.value, finish);
                std::string payload, status = "ok";
                try {
                    Child solver;
                    send_line(solver.input.value, task, finish);
                    close(solver.input.value); solver.input.value = -1;
                    while (line(solver.output.value, payload, finish)) {
                        if (payload.size() > 8192) throw std::runtime_error("reply too large");
                        send_line(connection.value, job_id, finish);
                        send_line(connection.value, "candidate", finish);
                        send_line(connection.value, payload, finish);
                    }
                    if (!solver.successful(finish)) throw std::runtime_error("solver failed");
                } catch (const std::exception&) { status = "error"; payload.clear(); }
                // If a candidate frame was interrupted, reconnect instead of appending
                // a terminal frame to a potentially truncated stream.
                if (Clock::now() >= finish) throw std::runtime_error("job timeout");
                send_line(connection.value, job_id, after(50));
                send_line(connection.value, "done", after(50));
                send_line(connection.value, status, after(50));
            }
        } catch (const std::exception&) {
            if (!stopping) std::cerr << "main unavailable; reconnecting\n";
        }
        for (int i = 0; i < 20 && !stopping; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
} // namespace

int main() {
    signal(SIGPIPE, SIG_IGN); signal(SIGINT, stop); signal(SIGTERM, stop);
    try {
        const auto secret = env("CLUSTER_SECRET");
        if (secret.size() < 16 || secret.size() > 256 || secret.find_first_of("\r\n") != std::string::npos ||
            secret.starts_with("replace-")) throw std::runtime_error("set a shared CLUSTER_SECRET (16..256 characters)");
        if (!valid_id(env("NODE_ID"))) throw std::runtime_error("invalid NODE_ID");
        const auto seed = env("SEED");
        if (seed.empty() || seed.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("SEED must be an unsigned 64-bit integer");
        (void)std::stoull(seed);
        const int port = number(env("PORT", "39001"), 1024, 65535);
        const int budget = number(env("JOB_TIMEOUT_MS", "10000"), 100, 600000);
#if defined(CLUSTER_FIXED_ROLE_main)
        if (setenv("ROLE", "main", 1) != 0) throw std::runtime_error("cannot set fixed role");
        return main_node(secret, port, budget);
#elif defined(CLUSTER_FIXED_ROLE_worker)
        if (setenv("ROLE", "worker", 1) != 0) throw std::runtime_error("cannot set fixed role");
        return worker_node(secret, port, budget);
#else
        if (env("ROLE") == "main") return main_node(secret, port, budget);
        if (env("ROLE") == "worker") return worker_node(secret, port, budget);
        throw std::runtime_error("ROLE must be main or worker");
#endif
    } catch (const std::exception& error) {
        if (stopping) return 0;
        std::cerr << "cluster: " << error.what() << '\n';
        return 1;
    }
}
