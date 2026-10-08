#include "hexa_udon/protocol/api_client.hpp"
#include "hexa_udon/protocol/request_id_digest.hpp"

#include <cassert>
#include <deque>
#include <iostream>

namespace {

class FakeTransport final : public hexa_udon::protocol::HttpTransport {
public:
    std::deque<hexa_udon::protocol::Result<hexa_udon::protocol::HttpResponse>> results;

    hexa_udon::protocol::Result<hexa_udon::protocol::HttpResponse> execute(
        const hexa_udon::protocol::HttpRequest&) override {
        assert(!results.empty());
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }
};

hexa_udon::protocol::Error post_kind_error(
    hexa_udon::protocol::Result<hexa_udon::protocol::HttpResponse> transport_result) {
    using namespace hexa_udon;
    FakeTransport transport;
    transport.results.push_back(std::move(transport_result));
    protocol::ApiConfig config;
    config.base_url = "http://example.invalid";
    config.minimum_request_interval = std::chrono::milliseconds{0};
    protocol::ProconApiClient client(transport, std::move(config));
    auto result = client.post_agent_kinds({core::AgentKind::Patrol});
    assert(!result);
    return result.error();
}

}  // namespace

int main() {
    using hexa_udon::protocol::request_id_digest;
    using hexa_udon::protocol::request_id_digest_or_missing;
    const nlohmann::json id = "request-α";
    const auto escaped_id = nlohmann::json::parse(R"("request-\u03b1")");
    assert(request_id_digest(id) == request_id_digest(escaped_id));
    assert(request_id_digest_or_missing(id) != "missing");
    assert(request_id_digest_or_missing(nlohmann::json(nullptr)) == "missing");
    assert(request_id_digest_or_missing(nlohmann::json::object()) == "missing");
    assert(request_id_digest(nlohmann::json("")) != std::nullopt);

    using hexa_udon::protocol::ErrorCode;
    using hexa_udon::protocol::HttpResponse;
    using hexa_udon::protocol::Result;
    for (const auto code : {ErrorCode::DnsFailure, ErrorCode::ConnectionRefused,
                            ErrorCode::ConnectionTimeout}) {
        const auto error = post_kind_error(Result<HttpResponse>::failure({code, "not connected"}));
        assert(error.submission_attempted == false);
    }
    for (const auto code : {ErrorCode::TransferTimeout, ErrorCode::Disconnected}) {
        const auto error = post_kind_error(Result<HttpResponse>::failure({code, "after send"}));
        assert(!error.submission_attempted.has_value());
        assert(error.code == ErrorCode::UnknownResponse);
    }
    for (const long status : {400L, 403L, 429L}) {
        const auto rejected = post_kind_error(Result<HttpResponse>::success(
            {status, "", std::chrono::milliseconds{1}, {{"retry-after", "2"}}}));
        assert(rejected.submission_attempted == false);
        assert(rejected.retry_after_ms == 2000);
    }
    const auto server_error = post_kind_error(Result<HttpResponse>::success(
        {500, "", std::chrono::milliseconds{1}}));
    assert(!server_error.submission_attempted.has_value());
    std::cout << "protocol_request_id_digest_tests: PASS\n";
}
