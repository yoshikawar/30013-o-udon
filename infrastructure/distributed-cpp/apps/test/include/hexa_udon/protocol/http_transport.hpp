#pragma once
#include "hexa_udon/protocol/result.hpp"
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
namespace hexa_udon::protocol {
enum class HttpMethod { Get, Post };
struct HttpRequest { HttpMethod method; std::string url; std::vector<std::string> headers; std::string body; std::chrono::milliseconds connect_timeout{2000}; std::chrono::milliseconds total_timeout{5000}; std::size_t max_response_bytes=1024*1024; std::string user_agent="hexa-udon/0.1"; };
struct HttpResponse {
    long status;
    std::string body;
    std::chrono::milliseconds elapsed;
    std::vector<std::pair<std::string, std::string>> headers;
    HttpResponse(long response_status, std::string response_body,
                 std::chrono::milliseconds response_elapsed,
                 std::vector<std::pair<std::string, std::string>> response_headers = {})
        : status(response_status), body(std::move(response_body)), elapsed(response_elapsed),
          headers(std::move(response_headers)) {}
};
class HttpTransport { public: virtual ~HttpTransport()=default; [[nodiscard]] virtual Result<HttpResponse> execute(const HttpRequest& request)=0; };
class CurlHttpTransport final : public HttpTransport {
public:
    CurlHttpTransport();
    ~CurlHttpTransport() override;
    CurlHttpTransport(const CurlHttpTransport&) = delete;
    CurlHttpTransport& operator=(const CurlHttpTransport&) = delete;
    [[nodiscard]] Result<HttpResponse> execute(const HttpRequest& request) override;

private:
    bool initialized_ = false;
};
}
