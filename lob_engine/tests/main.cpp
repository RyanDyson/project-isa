#include "harness.hpp"

#include <cstdio>

using lob::EventKind;
using lob::Side;

int main() {
    int failed = 0;
    for (const auto &test : lobtest::registry()) {
        const int before = lobtest::failure_count();
        try {
            test.fn();
        } catch (const lobtest::CheckFailed &) {
            // failure already counted; keep running the remaining tests
        }
        if (lobtest::failure_count() != before) {
            std::printf("FAIL  %s\n", test.name);
            ++failed;
        } else {
            std::printf("ok    %s\n", test.name);
        }
    }
    std::printf("\n%zu test(s), %d failed\n", lobtest::registry().size(), failed);
    return failed == 0 ? 0 : 1;
}
