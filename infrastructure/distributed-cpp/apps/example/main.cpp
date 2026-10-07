#include <cstdlib>
#include <atomic>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>

// Wiring demonstration, not a competition solver.
// stdout is reserved for the bridge protocol; diagnostics go to stderr.
int main() {
    const std::string role = std::getenv("ROLE") ? std::getenv("ROLE") : "worker";
    if (role == "main") {
        std::cout << "demo-task-1" << std::endl;  // dispatch to both remote PCs
        std::atomic<bool> finished{false};
        // Real application: computation publishes local candidates to the same
        // validation/selection queue as the stdin reader. Only main submits.
        std::jthread compute([&](std::stop_token stop) {
            while (!stop.stop_requested() && !finished)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
        std::string event;
        while (std::getline(std::cin, event)) {
            std::cerr << "remote event: " << event << '\n';
            // Demo only. Real applications must parse JSON, check jobId and
            // independently simulate candidates before comparing official scores.
            if (event.find("\"type\":\"done\"") != std::string::npos) {
                finished = true;
                break;
            }
        }
        if (!finished) return 1;
        std::cerr << "Demo finished. Real application must validate and select results.\n";
        return 0;
    }
    std::string task;
    if (!std::getline(std::cin, task)) return 1;
    std::mt19937_64 random(std::stoull(std::getenv("SEED")));
    auto best = random() % 1000;
    std::cout << best << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << best + 1 << std::endl; // a second best-so-far demo candidate
}
