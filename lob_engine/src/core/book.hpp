// Limit order book - public API.
//
// Matching core of the simulated exchange. One instance maintains the book
// for a single symbol. Uses price-time priority (FIFO within a price level)
// and a self-balancing AVL tree of price levels per side.
//
// Price levels live in an AVL tree -> O(log M) insert/remove of a new/dead
// level, O(1) best-bid/offer via cached tree extremes, O(1) volume-at-limit
// via a per-side price->level hash index, O(1) cancel via an id->order map
// (orders are pool-allocated, so cancellation only unlinks intrusive FIFO
// pointers).
//
// Stop orders (Stop / StopLimit) rest in a separate dormant book keyed by
// trigger price. Every trade publishes the last traded price; whenever the
// last price crosses a dormant order's trigger, it activates:
//   Stop       -> converts into a MarketOrder (unfilled remainder cancelled)
//   StopLimit  -> converts into a crossing limit order (remainder may rest)
// Activations cascade: a triggered stop can trade, which can move the last
// price and trigger further stops, until no more triggers fire.
//
// Concurrency: not thread-safe by design. The Redis service layer owns a
// single-threaded event loop (single source of truth, deterministic replay).
//
// Prices are doubles but every comparison is exact; use integer-valued
// prices (ticks) to keep arithmetic trivially exact.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lob {

enum class Side : std::uint8_t { Buy, Sell };
enum class Type : std::uint8_t { Market, Limit, MarketableLimit, Stop, StopLimit };

using Price = double;           // per-share price; positive, tick-valued recommended
using Quantity = std::int64_t;  // whole units; must be > 0

struct SubmitRequest {
    Type type = Type::Limit;
    Side side = Side::Buy;
    Quantity quantity = 0;
    Price limit_price = 0.0;    // required for Limit, MarketableLimit, StopLimit
    Price stop_price = 0.0;     // required for Stop, StopLimit
    std::uint64_t order_id = 0; // optional; 0 -> engine assigns a unique id
};

struct ModifyRequest {
    bool has_new_price = false;
    Price new_price = 0.0;
    bool has_new_quantity = false;
    Quantity new_quantity = 0;
    bool has_new_stop_price = false; // dormant Stop / StopLimit only
    Price new_stop_price = 0.0;
};

enum class EventKind : std::uint8_t {
    Accepted,   // validated and admitted (stops are accepted while dormant)
    Rejected,   // not admitted (bad params, duplicate id, crossing pure limit)
    Trade,      // maker(order_id) x taker(taker_order_id) at price/quantity
    Filled,     // one participant's share of a trade; remaining = qty left
    Cancelled,  // order removed (user cancel, or unfilled market remainder)
    Modified,   // resting/dormant order updated; may lose queue priority
    Triggered,  // dormant stop activated by the last traded price
};

struct Event {
    EventKind kind = EventKind::Accepted;
    std::uint64_t order_id = 0;        // subject of the event; maker id for Trade
    std::uint64_t taker_order_id = 0;  // Trade only
    Price price = 0.0;                 // Trade/Filled: execution price
    Quantity quantity = 0;             // Trade/Filled: traded qty; Cancelled: remaining qty; Modified: new qty
    Quantity remaining = 0;            // Filled: order quantity left after the fill
    const char *reason = nullptr;      // Rejected only
};

class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void on_event(const Event &) {}
};

class OrderBook {
public:
    // `sink` receives lifecycle/trade events synchronously inside submit,
    // cancel, and modify. Nullptr installs a no-op sink (benchmarks).
    explicit OrderBook(EventSink *sink = nullptr);
    ~OrderBook();
    OrderBook(const OrderBook &) = delete;
    OrderBook &operator=(const OrderBook &) = delete;

    // Submits an order. Returns the order id used (assigned ids are strictly
    // increasing and never reused, even after reject/fill/cancel).
    // Semantics:
    //   MarketOrder       - immediate-only; unfilled remainder cancelled.
    //   LimitOrder        - resting-only; rejected outright if it would cross.
    //   MarketableLimit   - taker phase first, remainder rests at limit price.
    //   StopOrder         - dormant until trigger, then acts as MarketOrder.
    //   StopLimit         - dormant until trigger, then acts as MarketableLimit.
    std::uint64_t submit(const SubmitRequest &request);

    // Cancels a resting limit or a dormant (pre-trigger) stop. Returns false
    // (and emits Rejected "unknown-order") when the id is not live.
    bool cancel(std::uint64_t order_id);

    // Modifies a resting limit or dormant stop.
    //  - quantity decrease keeps time priority: O(1)
    //  - quantity increase or price change re-books the order lose priority)
    // Stop-limit modification (price only) keeps queue position.
    bool modify(std::uint64_t order_id, const ModifyRequest &request);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Last traded price (undefined until the first trade).
    std::optional<Price> last_trade_price() const;

    // Total resting quantity at an exact price. O(1).
    Quantity volume_at(Side side, Price price) const;

    // Total dormant quantity at a stop trigger price. O(1).
    Quantity stop_volume_at(Side side, Price trigger_price) const;

    // Number of live orders (resting + dormant stops).
    std::size_t order_count() const;

    // Verifies all internal invariants (AVL balance/heights/parents/keys,
    // FIFO links, per-level totals, id/index maps). Empty string = healthy;
    // otherwise a description of the first violation found.
    std::string audit() const;

private:
    struct Order;
    struct PriceLevel;
    struct LevelTree;
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace lob
