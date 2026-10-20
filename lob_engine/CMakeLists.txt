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
# LOB matching core: no external dependencies. Modules:
#   book.cpp            public facade
#   level_tree.cpp      AVL tree of price levels
#   state_pools.cpp     engine pools + level book-keeping
#   state_matching.cpp  matching + stop triggers
#   state_commands.cpp  validation + submit/cancel/modify
#   state_queries.cpp   queries + invariant audit
# (LTO in release builds keeps cross-module calls inlined.)
# ---------------------------------------------------------------------------
add_library(lob_core
    src/core/book.cpp
    src/core/level_tree.cpp
    src/core/state_pools.cpp
    src/core/state_matching.cpp
    src/core/state_commands.cpp
    src/core/state_queries.cpp)
set_target_properties(lob_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(lob_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(lob_core PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE)

add_executable(lob-core-tests
    tests/main.cpp
    tests/test_matching.cpp
    tests/test_placement.cpp
    tests/test_stops.cpp
    tests/test_stress.cpp)
target_link_libraries(lob-core-tests PRIVATE lob_core)

add_executable(lob-core-bench bench/bench_main.cpp)
target_link_libraries(lob-core-bench PRIVATE lob_core)
