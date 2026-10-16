#!/usr/bin/env bash
# Builds and runs the LOB core tests (and optionally the benchmark).
# Usable on any host with a C++17 toolchain; designed for the gcc:13 image:
#   docker run --rm -v "$(pwd)":/work -w /work gcc:13 bash scripts/build_and_test.sh --bench
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
CXX=${CXX:-g++}
flags=(-std=c++17 -O2 -Wall -Wextra -Wpedantic -I"$root/src")

echo "== build tests =="
"$CXX" "${flags[@]}" "$root/src/core/book.cpp" "$root"/tests/*.cpp -o /tmp/lob-tests
echo "== run tests =="
/tmp/lob-tests
if [[ "${1:-}" == "--bench" ]]; then
    echo "== build bench =="
    "$CXX" -std=c++17 -O3 -DNDEBUG -Wall -Wextra -I"$root/src" \
        "$root/src/core/book.cpp" "$root/bench/bench_main.cpp" -o /tmp/lob-bench
    echo "== run bench =="
    /tmp/lob-bench
fi
