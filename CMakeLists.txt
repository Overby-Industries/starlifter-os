cmake_minimum_required(VERSION 3.16)
project(starlifter_os LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(starlifter STATIC
    src/json.cpp
    src/time.cpp
    src/otf1.cpp
    src/node.cpp
    src/coordinator.cpp
    src/sim.cpp
)

add_library(starlifter::starlifter ALIAS starlifter)

target_include_directories(starlifter PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(starlifter PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(starlifter PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# The fleet demo: one coordinator, five nodes, delays, an outage and two
# faults. Prints a summary and can write every packet sent as JSON Lines.
add_executable(starlifter_fleet_sim sim/main.cpp)
target_link_libraries(starlifter_fleet_sim PRIVATE starlifter)

enable_testing()

add_executable(starlifter_tests
    tests/main.cpp
    tests/json_tests.cpp
    tests/time_tests.cpp
    tests/otf1_tests.cpp
    tests/node_tests.cpp
    tests/coordinator_tests.cpp
    tests/fleet_tests.cpp
)
target_link_libraries(starlifter_tests PRIVATE starlifter)
# The decoder is tested against the same example and must-fail packets the
# JSON Schema is, so the tests need to know where protocol/ is.
target_compile_definitions(starlifter_tests PRIVATE
    STARLIFTER_PROTOCOL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/protocol")
if(MSVC)
    target_compile_options(starlifter_tests PRIVATE /utf-8)
    target_compile_options(starlifter_fleet_sim PRIVATE /utf-8)
endif()

add_test(NAME starlifter_tests COMMAND starlifter_tests)
