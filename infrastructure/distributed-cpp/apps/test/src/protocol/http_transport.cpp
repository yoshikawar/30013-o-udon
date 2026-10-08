#include "hexa_udon/protocol/http_transport.hpp"

#include <curl/curl.h>

#include <array>
#include <cctype>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace hexa_udon::protocol {
namespace {

std::mutex global_mutex;
std::size_t global_users = 0;

struct WriteState {
    std::string body;
    std::size_t limit;
    bool exceeded = false;
};

struct HeaderState {
    std::vector<std::pair<std::string, std::string>> headers;
};

std::size_t header_callback(char* data, std::size_t size, std::size_t count,
                            void* pointer) noexcept {
    auto* state = static_cast<HeaderState*>(pointer);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto bytes = size * count;
    std::string line(data, bytes);
    const auto colon = line.find(':');
    if (colon == std::string::npos) return bytes;
    auto name = line.substr(0, colon);
    auto value = line.substr(colon + 1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) value.pop_back();
    for (auto& character : name) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    state->headers.emplace_back(std::move(name), std::move(value));
    return bytes;
}

std::size_t write_callback(
    char* data, std::size_t size, std::size_t count, void* pointer) noexcept {
    auto* state = static_cast<WriteState*>(pointer);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        state->exceeded = true;
        return 0;
    }
    const auto bytes = size * count;
    if (state->body.size() > state->limit || bytes > state->limit - state->body.size()) {
        state->exceeded = true;
        return 0;
    }
    try {
        state->body.append(data, bytes);
        return bytes;
    } catch (...) {
        return 0;
    }
}

ErrorCode curl_code(CURLcode code, curl_off_t connect_time_microseconds) {
    switch (code) {
        case CURLE_COULDNT_RESOLVE_HOST:
            return ErrorCode::DnsFailure;
        case CURLE_COULDNT_CONNECT:
            return ErrorCode::ConnectionRefused;
        case CURLE_OPERATION_TIMEDOUT:
            return connect_time_microseconds == 0 ? ErrorCode::ConnectionTimeout
                                                  : ErrorCode::TransferTimeout;
        case CURLE_RECV_ERROR:
        case CURLE_PARTIAL_FILE:
            return ErrorCode::Disconnected;
        default:
            return ErrorCode::Transport;
    }
}

long timeout_value(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0) {
        return 1;
    }
    if (timeout.count() > std::numeric_limits<long>::max()) {
        return std::numeric_limits<long>::max();
    }
    return static_cast<long>(timeout.count());
}

}  // namespace

CurlHttpTransport::CurlHttpTransport() {
    std::lock_guard lock(global_mutex);
    if (global_users == 0 && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return;
    }
    ++global_users;
    initialized_ = true;
}

CurlHttpTransport::~CurlHttpTransport() {
    std::lock_guard lock(global_mutex);
    if (initialized_ && --global_users == 0) {
        curl_global_cleanup();
    }
}

Result<HttpResponse> CurlHttpTransport::execute(const HttpRequest& request) {
    if (!initialized_) {
        return Result<HttpResponse>::failure(
            {ErrorCode::Transport, "curl global initialization failed"});
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return Result<HttpResponse>::failure(
            {ErrorCode::Transport, "curl request initialization failed"});
    }

    WriteState write{{}, request.max_response_bytes};
    HeaderState response_headers;
    curl_slist* headers = nullptr;
    for (const auto& header : request.headers) {
        curl_slist* appended = curl_slist_append(headers, header.c_str());
        if (appended == nullptr) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return Result<HttpResponse>::failure(
                {ErrorCode::Transport, "curl header allocation failed"});
        }
        headers = appended;
    }

    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, request.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_value(request.connect_timeout));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_value(request.total_timeout));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.data());
    if (request.method == HttpMethod::Post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(request.body.size()));
    }

    const auto start = std::chrono::steady_clock::now();
    const auto code = curl_easy_perform(curl);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    long status = 0;
    curl_off_t connect_time_microseconds = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect_time_microseconds);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (write.exceeded) {
        return Result<HttpResponse>::failure(
            {ErrorCode::ResponseTooLarge, "response exceeded configured limit"});
    }
    if (code != CURLE_OK) {
        std::string message = error_buffer[0] == '\0' ? curl_easy_strerror(code)
                                                       : error_buffer.data();
        return Result<HttpResponse>::failure(
            {curl_code(code, connect_time_microseconds), message.substr(0, 256)});
    }
    return Result<HttpResponse>::success({status, std::move(write.body), elapsed,
                                          std::move(response_headers.headers)});
}

}  // namespace hexa_udon::protocol
