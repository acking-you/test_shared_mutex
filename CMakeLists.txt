cmake_minimum_required(VERSION 3.29)
project(bench_shared_mutex)

set(CMAKE_CXX_STANDARD 20)

add_executable(bench_shared_mutex main.cpp
        SharedMutex.cpp
        SharedMutex.h)

target_link_libraries(bench_shared_mutex pthread)
