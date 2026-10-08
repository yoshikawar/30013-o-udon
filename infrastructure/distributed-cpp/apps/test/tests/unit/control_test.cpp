#include "hexa_udon/protocol/request_control.hpp"

#include <cassert>

int main() {
    using namespace hexa_udon::protocol;
    SteadyTime now{};
    RequestRateLimiter limiter(std::chrono::milliseconds{10}, [&] { return now; },
        [&](SteadyTime target) { now = target; });
    assert(limiter.acquire(now + std::chrono::milliseconds{1}));
    const auto equal = limiter.acquire(now + std::chrono::milliseconds{10});
    assert(!equal && equal.error().code == ErrorCode::DeadlineExceeded);
    now += std::chrono::milliseconds{20};
    const auto late = limiter.check_deadline(now);
    assert(!late && late.error().code == ErrorCode::DeadlineExceeded);
}
