#include "hexa_udon/protocol/request_id_digest.hpp"

#include <cassert>
#include <iostream>

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
    std::cout << "protocol_request_id_digest_tests: PASS\n";
}
