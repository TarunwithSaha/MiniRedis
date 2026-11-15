cmake_minimum_required(VERSION 3.10)
project(MiniRedis CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimize by default. We deliberately drop CMake's -DNDEBUG: it would silently
# disable every assert() in the test programs, so the tests would "pass" while checking nothing.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(Threads REQUIRED)

add_executable(server main.cpp)
target_link_libraries(server PRIVATE Threads::Threads)

add_executable(bench bench.cpp)
target_link_libraries(bench PRIVATE Threads::Threads)

enable_testing()
foreach(t lru_test lru_expiry_test lru_order_test)
  add_executable(${t} ${t}.cpp)
  add_test(NAME ${t} COMMAND ${t})
endforeach()