// Randomized stress: hammer the book with a deterministic PRNG mix and
// verify full invariants after every operation. Catches corner cases in
// tree/FIFO bookkeeping that hand-written scenarios can't reach.
//
// Live-id tracking is approximate: resting orders may be filled by later
// aggressive orders, so an id list entry can become stale. Cancelling a
// stale id simply yields "unknown-order", which is accepted here.
#include "harness.hpp"

#include <random>
#include <vector>

using lob::EventKind;
using lob::Side;

namespace {
constexpr std::size_t kPrices = 200;  // distinct tick levels spanning the range
inline double tick_price(std::size_t i) { return 90.0 + static_cast<double>(i) * 0.5; }
}  // namespace

LOB_TEST(stress_audit_after_every_operation) {
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        lobtest::Recorder sink;
        lob::OrderBook book(&sink);
        std::mt19937_64 rng(seed);
        std::vector<std::uint64_t> live_ids;
        std::vector<std::string> ops;
        int failing_step = -1;

        for (int step = 0; step < 4000; ++step) {
            const std::uint64_t roll = rng() % 100;
            if (roll < 45 || live_ids.empty()) {  // submit a resting limit
                lob::SubmitRequest req;
                req.type = lob::Type::Limit;
                req.side = (rng() & 1) != 0 ? Side::Sell : Side::Buy;
                req.quantity = static_cast<std::int64_t>(1 + rng() % 100);
                req.limit_price = tick_price(rng() % kPrices);
                const std::uint64_t id = book.submit(req);
                if (sink.events.back().kind == EventKind::Accepted) live_ids.push_back(id);
                ops.push_back("limit " + std::to_string(req.quantity) + " @ " +
                              std::to_string(req.limit_price) + " side " +
                              std::to_string((int)req.side) + " id " + std::to_string(id));
            } else if (roll < 70) {  // cancel a random id (may be stale -> rejected)
                const std::size_t pick = static_cast<std::size_t>(rng() % live_ids.size());
                const std::uint64_t id = live_ids[pick];
                live_ids[pick] = live_ids.back();
                live_ids.pop_back();
                const bool ok = book.cancel(id);
                ops.push_back(std::string("cancel id ") + std::to_string(id) +
                              (ok ? " ok" : " unknown"));
            } else if (roll < 92) {  // aggressive order sweeping the whole side
                lob::SubmitRequest req;
                req.type = lob::Type::MarketableLimit;
                const bool buy = (rng() & 1) != 0;
                req.side = buy ? Side::Buy : Side::Sell;
                req.quantity = static_cast<std::int64_t>(1 + rng() % 300);
                req.limit_price = buy ? 200.0 : 89.0;
                const std::uint64_t id = book.submit(req);
                ops.push_back("aggressive " + std::to_string(req.quantity) + " side " +
                              std::to_string((int)req.side) + " id " + std::to_string(id));
            } else {  // dormant stops that cascades can trip
                const bool buy = (rng() & 1) != 0;
                const std::int64_t qty = static_cast<std::int64_t>(1 + rng() % 50);
                const std::uint64_t id =
                    book.submit(lobtest::stop(buy ? Side::Buy : Side::Sell, buy ? 145.0 : 135.0,
                                              qty));
                ops.push_back("stop " + std::to_string(qty) + " side " +
                              std::to_string((int)(buy ? 0 : 1)) + " trigger " +
                              std::to_string(buy ? 145.0 : 135.0) + " id " + std::to_string(id));
            }

            const std::string error = book.audit();
            if (!error.empty()) {
                std::fprintf(stderr, "seed %u step %d: %s\n", seed, step, error.c_str());
                const int first = step > 300 ? step - 300 : 0;
                for (int i = first; i <= step; ++i) {
                    std::fprintf(stderr, "  step %4d: %s\n", i, ops[static_cast<std::size_t>(i)].c_str());
                }
                (void)failing_step;
                lobtest::fail("audited");
            }
        }
        EXPECT(book.cancel(static_cast<std::uint64_t>(-1)) == false);
    }
}
