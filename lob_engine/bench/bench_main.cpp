// Standalone throughput and trade-rate benchmark for the LOB core.
//
// Reports two headline numbers per scenario:
//   ops/s     - book transactions per second (submits, cancels, modifies)
//   trades/s  - executed trades per second
//
// Single-threaded, in-memory core. Deterministic seeds: numbers are stable.
#include "core/book.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

using lob::OrderBook;
using lob::Price;
using lob::Quantity;
using lob::Side;
using lob::SubmitRequest;
using lob::Type;

struct CountingSink final : lob::EventSink {
    std::uint64_t trades = 0;
    std::uint64_t events = 0;
    void on_event(const lob::Event &e) override {
        ++events;
        if (e.kind == lob::EventKind::Trade) ++trades;
    }
};

struct Result {
    const char *name;
    std::uint64_t ops;
    std::uint64_t trades;
    double seconds;
};

template <typename Fn>
Result run(const char *name, std::uint64_t ops, Fn &&body) {
    CountingSink sink;
    OrderBook book(&sink);
    const auto start = std::chrono::steady_clock::now();
    body(book, sink);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const std::string error = book.audit();
    if (!error.empty()) {
        std::fprintf(stderr, "AUDIT FAILED in %s: %s\n", name, error.c_str());
        std::exit(1);
    }
    return Result{name, ops, sink.trades, seconds};
}

[[maybe_unused]] double tick_price_away(Price price) {
    return std::floor(price * 100.0) / 100.0;
}

std::vector<Price> tick_grid(Price lo, Price hi) {
    std::vector<Price> prices;
    for (Price p = lo; p <= hi + 1e-9; p += 0.01) prices.push_back(p);
    return prices;
}

// 1. Resting liquidity build-up: the new-price-level path (AVL insert).
Result bench_build_depth() {
    const std::uint64_t ops = 1'000'000;
    const auto grid = tick_grid(99.00, 101.00);
    return run("build-depth", ops, [&](OrderBook &book, CountingSink &) {
        std::mt19937_64 rng(42);
        for (std::uint64_t i = 0; i < ops; ++i) {
            SubmitRequest req;
            req.type = Type::Limit;
            req.side = (i & 1) != 0 ? Side::Sell : Side::Buy;
            req.quantity = 100;
            req.limit_price = grid[rng() % grid.size()];
            book.submit(req);
        }
    });
}

// 2. Steady-state matching: deep two-sided book, every aggressive order
//    crosses one level with size 10 → one trade per submission.
Result bench_steady_matching() {
    const std::uint64_t ops = 1'000'000;
    const auto grid = tick_grid(99.00, 101.00);
    return run("steady-matching", ops, [&](OrderBook &book, CountingSink &) {
        std::mt19937_64 rng(42);
        // seed ~26k levels with 1,000 units each on both sides
        for (Price p : grid) {
            SubmitRequest bid;
            bid.type = Type::Limit;
            bid.side = Side::Buy;
            bid.quantity = 1000;
            bid.limit_price = p;
            book.submit(bid);
            SubmitRequest ask = bid;
            ask.side = Side::Sell;
            book.submit(ask);
        }
        const std::size_t level_shares = 10;  // consume 10 units per order
        for (std::uint64_t i = 0; i < ops; ++i) {
            SubmitRequest req;
            req.type = Type::MarketableLimit;
            req.side = ((i & 3) == 0) ? Side::Sell : Side::Buy;
            req.quantity = level_shares;
            req.limit_price = req.side == Side::Buy ? 999.0 : 0.01;  // cross top level
            book.submit(req);
        }
    });
}

// 3. Cancel/modify churn: the O(1) paths.
Result bench_cancel_churn() {
    const std::uint64_t per_round = 200'000;
    const auto grid = tick_grid(99.00, 101.00);
    return run("cancel-churn", per_round * 2, [&](OrderBook &book, CountingSink &) {
        std::mt19937_64 rng(42);
        for (std::uint64_t round = 0; round < 5; ++round) {
            for (std::uint64_t i = 0; i < per_round; ++i) {
                SubmitRequest req;
                req.type = Type::Limit;
                req.side = Side::Buy;
                req.quantity = 100;
                req.limit_price = grid[rng() % grid.size()];
                book.submit(req);
            }
            for (std::uint64_t id = round * per_round + 1; id <= (round + 1) * per_round; ++id) {
                book.cancel(id);
            }
        }
    });
}

// 4. Mixed random workload: limits, cancels, aggressive orders, stops.
Result bench_mixed_workload() {
    const std::uint64_t ops = 1'000'000;
    const auto grid = tick_grid(99.50, 100.50);
    return run("mixed-workload", ops, [&](OrderBook &book, CountingSink &) {
        std::mt19937_64 rng(42);
        std::uint64_t ids[1000];
        std::size_t live = 0;
        for (std::uint64_t i = 0; i < ops; ++i) {
            const std::uint64_t roll = rng() % 100;
            if (roll < 55) {  // resting limit
                SubmitRequest req;
                req.type = Type::Limit;
                req.side = (rng() & 1) != 0 ? Side::Sell : Side::Buy;
                req.quantity = static_cast<Quantity>(1 + rng() % 500);
                req.limit_price = grid[rng() % grid.size()];
                const std::uint64_t id = book.submit(req);
                if (live < 1000) ids[live++] = id;
            } else if (roll < 75 && live > 0) {  // cancel a random live order
                const std::size_t pick = static_cast<std::size_t>(rng() % live);
                const std::uint64_t id = ids[pick];
                ids[pick] = ids[--live];
                book.cancel(id);
            } else if (roll < 92) {  // aggressive crossing order
                const bool buy = (rng() & 1) != 0;
                SubmitRequest req;
                req.type = Type::MarketableLimit;
                req.side = buy ? Side::Buy : Side::Sell;
                req.quantity = static_cast<Quantity>(1 + rng() % 100);
                req.limit_price = buy ? 101.0 : 99.0;
                book.submit(req);
            } else {  // dormant stop near the mid
                const bool buy = (rng() & 1) != 0;
                SubmitRequest req;
                req.type = Type::Stop;
                req.side = buy ? Side::Buy : Side::Sell;
                req.quantity = static_cast<Quantity>(1 + rng() % 50);
                req.stop_price = buy ? 100.10 : 99.90;
                book.submit(req);
            }
        }
    });
}

}  // namespace

int main() {
    std::fprintf(stderr, "[bench] starting\n");
    const Result r1 = bench_build_depth();
    std::fprintf(stderr, "[bench] build-depth done\n");
    const Result r2 = bench_steady_matching();
    std::fprintf(stderr, "[bench] steady-matching done\n");
    const Result r3 = bench_cancel_churn();
    std::fprintf(stderr, "[bench] cancel-churn done\n");
    const Result r4 = bench_mixed_workload();
    std::fprintf(stderr, "[bench] mixed-workload done\n");

    std::printf("%-16s %4s %12s %12s %10s %12s\n", "scenario", "", "ops", "ops/s", "trades",
                "trades/s");
    const std::vector<Result> results = {r1, r2, r3, r4};
    std::printf("%s\n", std::string(88, '-').c_str());
    for (const Result &r : results) {
        std::printf("%-16s %4s %12llu %12.0f %10llu %12.0f\n", r.name, "", 
                    static_cast<unsigned long long>(r.ops), r.ops / r.seconds,
                    static_cast<unsigned long long>(r.trades),
                    r.trades / r.seconds);
    }
    std::printf("\nsingle-threaded in-memory core (no serialization, no Redis)\n");
    return 0;
}
