cmake_minimum_required(VERSION 3.20)
project(cluster_bridge LANGUAGES CXX)
find_package(Threads REQUIRED)
foreach(role IN ITEMS main worker)
  add_executable(cluster_${role} main.cpp)
  target_compile_features(cluster_${role} PRIVATE cxx_std_20)
  target_compile_options(cluster_${role} PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(cluster_${role} PRIVATE CLUSTER_FIXED_ROLE_${role}=1)
  target_link_libraries(cluster_${role} PRIVATE Threads::Threads)
endforeach()

# Native loopback tests use one role-selectable binary. Runtime images copy only
# cluster_main or cluster_worker, so a worker image cannot be started as a server.
add_executable(cluster_bridge main.cpp)
target_compile_features(cluster_bridge PRIVATE cxx_std_20)
target_compile_options(cluster_bridge PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(cluster_bridge PRIVATE Threads::Threads)
