#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace std::chrono_literals;
int main() {
    const std::string mode = std::getenv("TEST_MODE");
    if (std::string(std::getenv("ROLE")) == "main") {
        for (int job = 1; job <= 2; ++job) {
            const auto start = std::chrono::steady_clock::now();
            std::cout << "task-" << job << std::endl;
            if (mode == "backpressure") std::this_thread::sleep_for(3s);
            std::string event;
            bool done = false;
            while (std::getline(std::cin, event)) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                std::cerr << "EVENT " << ms << " " << event << '\n';
                if (event.find("\"type\":\"done\"") != std::string::npos) {
                    done = true;
                    break;
                }
            }
            if (!done) return 2;
        }
        return 0;
    }
    std::string task;
    std::getline(std::cin, task);
    if (mode == "backpressure") {
        for (int i = 0; i < 4; ++i) std::cout << std::string(8192, '\x01') << std::endl;
        return 0;
    }
    const bool slow = std::string(std::getenv("NODE_ID")) == "worker-1";
    if (slow) std::this_thread::sleep_for(1500ms);
    std::cout << task << "-first" << std::endl;
    if (mode == "failure") return 1;
    std::this_thread::sleep_for(1200ms);
    std::cout << task << "-better" << std::endl;
}
