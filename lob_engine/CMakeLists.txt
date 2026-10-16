cmake_minimum_required(VERSION 3.16)
project(lob_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

add_executable(lob-engine src/main.cpp)

target_link_libraries(lob-engine PRIVATE hiredis pthread nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# LOB matching core: no external dependencies (depending on hiredis/json is
# what requires none of these targets to run in plain CMake).
# ---------------------------------------------------------------------------
add_library(lob_core STATIC src/core/book.cpp)
set_target_properties(lob_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(lob_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(lob-core-tests
    tests/main.cpp
    tests/test_matching.cpp
    tests/test_placement.cpp
    tests/test_stops.cpp
    tests/test_stress.cpp)
target_link_libraries(lob-core-tests PRIVATE lob_core)

add_executable(lob-core-bench bench/bench_main.cpp)
target_link_libraries(lob-core-bench PRIVATE lob_core)
