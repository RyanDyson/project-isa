// Minimal self-registering test harness for the LOB core (no external deps).
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "core/book.hpp"

namespace lobtest {

struct TestCase {
    const char *name;
    void (*fn)();
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int &failure_count() {
    static int count = 0;
    return count;
}

struct CheckFailed {
    std::string what;
};

[[noreturn]] inline void fail(const std::string &what) {
    std::fprintf(stderr, "    check failed: %s\n", what.c_str());
    ++failure_count();
    throw CheckFailed{what};
}

inline void check(bool ok, const std::string &what) {
    if (!ok) fail(what);
}

#define LOB_TEST(name)                                                            \
    static void test_body_##name();                                               \
    static const bool registered_##name = [] {                                    \
        lobtest::registry().push_back(lobtest::TestCase{#name, test_body_##name}); \
        return true;                                                              \
    }();                                                                          \
    static void test_body_##name()

#define EXPECT(cond) lobtest::check((cond), #cond)
#define EXPECT_EQ(a, b) lobtest::check((a) == (b), std::string(#a) + " == " + #b)

// ------------------------------------------------------------------ helpers

struct Recorder : lob::EventSink {
    std::vector<lob::Event> events;

    void on_event(const lob::Event &e) override { events.push_back(e); }

    std::size_t count(lob::EventKind kind) const {
        std::size_t n = 0;
        for (const auto &e : events) n += e.kind == kind ? 1 : 0;
        return n;
    }

    std::vector<lob::Event> of_kind(lob::EventKind kind) const {
        std::vector<lob::Event> out;
        for (const auto &e : events) {
            if (e.kind == kind) out.push_back(e);
        }
        return out;
    }
};

inline lob::SubmitRequest mk(lob::Type type, lob::Side side, std::int64_t qty, double limit = 0.0,
                             double stop = 0.0, std::uint64_t id = 0) {
    lob::SubmitRequest req;
    req.type = type;
    req.side = side;
    req.quantity = qty;
    req.limit_price = limit;
    req.stop_price = stop;
    req.order_id = id;
    return req;
}

inline lob::SubmitRequest limit(lob::Side side, double price, std::int64_t qty,
                                std::uint64_t id = 0) {
    return mk(lob::Type::Limit, side, qty, price, 0.0, id);
}
inline lob::SubmitRequest market(lob::Side side, std::int64_t qty, std::uint64_t id = 0) {
    return mk(lob::Type::Market, side, qty, 0.0, 0.0, id);
}
inline lob::SubmitRequest marketable(lob::Side side, double price, std::int64_t qty,
                                     std::uint64_t id = 0) {
    return mk(lob::Type::MarketableLimit, side, qty, price, 0.0, id);
}
inline lob::SubmitRequest stop(lob::Side side, double trigger, std::int64_t qty,
                               std::uint64_t id = 0) {
    return mk(lob::Type::Stop, side, qty, 0.0, trigger, id);
}
inline lob::SubmitRequest stop_limit(lob::Side side, double trigger, double limit_price,
                                     std::int64_t qty, std::uint64_t id = 0) {
    return mk(lob::Type::StopLimit, side, qty, limit_price, trigger, id);
}

inline void expect_healthy(const lob::OrderBook &book) {
    const std::string error = book.audit();
    if (!error.empty()) fail("audit: " + error);
}

}  // namespace lobtest
