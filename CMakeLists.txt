cmake_minimum_required(VERSION 3.16)
project(starlifter_os LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# OTF-1 is the only wire format (docs/architecture.md, principle 2). The
# format and its C++ implementation, libotf1, live in the telemetry-protocol
# repository, which is a git submodule here.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/telemetry-protocol/CMakeLists.txt")
    message(FATAL_ERROR
        "The telemetry-protocol submodule is missing. Fetch it with:\n"
        "    git submodule update --init\n"
        "or clone with:  git clone --recurse-submodules <url>")
endif()
add_subdirectory(telemetry-protocol)

add_library(starlifter STATIC
    src/node.cpp
    src/coordinator.cpp
    src/sim.cpp
)

add_library(starlifter::starlifter ALIAS starlifter)

target_include_directories(starlifter PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# PUBLIC: the fleet layer's own headers are written in OTF-1 types.
target_link_libraries(starlifter PUBLIC otf1::otf1)

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
    tests/node_tests.cpp
    tests/coordinator_tests.cpp
    tests/fleet_tests.cpp
)
target_link_libraries(starlifter_tests PRIVATE starlifter)
if(MSVC)
    target_compile_options(starlifter_tests PRIVATE /utf-8)
    target_compile_options(starlifter_fleet_sim PRIVATE /utf-8)
endif()

add_test(NAME starlifter_tests COMMAND starlifter_tests)
