#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main() {
    std::string task;
    std::getline(std::cin, task);
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "late result" << std::endl;
}
