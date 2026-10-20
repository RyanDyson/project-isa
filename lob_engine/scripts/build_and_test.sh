#!/usr/bin/env bash
# Builds and runs the LOB core tests (and optionally the benchmark).
# Usable on any host with a C++17 toolchain; designed for the gcc:13 image:
#   docker run --rm -v "$(pwd)":/work -w /work gcc:13 bash scripts/build_and_test.sh --bench
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
CXX=${CXX:-g++}
# -flto: the engine is split across modules; LTO keeps cross-module calls
# inlined so throughput matches the pre-split single-TU build.
flags=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -flto=auto -I"$root/src")
core=("$root/src/core/book.cpp"
      "$root/src/core/level_tree.cpp"
      "$root/src/core/state_pools.cpp"
      "$root/src/core/state_matching.cpp"
      "$root/src/core/state_commands.cpp"
      "$root/src/core/state_queries.cpp")

echo "== build tests =="
"$CXX" "${flags[@]}" "${core[@]}" "$root"/tests/*.cpp -o /tmp/lob-tests
echo "== run tests =="
/tmp/lob-tests
if [[ "${1:-}" == "--bench" ]]; then
    echo "== build bench =="
    "$CXX" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -flto=auto -I"$root/src" \
        "${core[@]}" "$root/bench/bench_main.cpp" -o /tmp/lob-bench
    echo "== run bench =="
    /tmp/lob-bench
fi
