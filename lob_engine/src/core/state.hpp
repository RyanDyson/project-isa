// OrderBook::State - internal matching-engine state and operations.
//
// Data layout (all pool-backed; stable pointers, zero per-op allocation):
//   LevelTree bid / ask sides keyed by limit price
//   LevelTree dormant buy / sell stops keyed by trigger price
//   price -> level unordered maps for O(1) volume lookups
//   order id -> order map for O(1) cancel/modify
//
// The implementation is split across state_pools.cpp (pools + level
// book-keeping), state_matching.cpp (matching + stop triggers),
// state_commands.cpp (validation + submit/cancel/modify) and
// state_queries.cpp (queries + audit). Link/unlink and other tiny hot
// helpers are inline here.
//
// Internal module (not part of the public API).
#pragma once

#include <cassert>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/book.hpp"
#include "core/level_tree.hpp"
#include "core/nodes.hpp"

namespace lob {

struct OrderBook::State {
    explicit State(EventSink *sink);  // state_pools.cpp

    // ---------------------------------------------------------------- data

    EventSink *sink;

    LevelTree bids;        // resting buy limits: best = highest price
    LevelTree asks;        // resting sell limits: best = lowest price
    LevelTree stop_buys;   // dormant buy stops: trigger scan from lowest up
    LevelTree stop_sells;  // dormant sell stops: trigger scan from highest down

    // price -> level node, kept in lockstep with the trees: O(1) volume lookups
    std::unordered_map<Price, PriceLevel *> bid_index;
    std::unordered_map<Price, PriceLevel *> ask_index;
    std::unordered_map<Price, PriceLevel *> stop_buy_index;
    std::unordered_map<Price, PriceLevel *> stop_sell_index;

    std::deque<Order> order_pool;
    std::vector<Order *> free_orders;
    std::deque<PriceLevel> level_pool;
    std::vector<PriceLevel *> free_levels;
    std::unordered_map<std::uint64_t, Order *> by_id;
    std::uint64_t next_id = 0;

    Price last_price = 0.0;
    bool has_last = false;

    static EventSink *fallback_sink() {
        static EventSink no_op;
        return &no_op;
    }

    // ----------------------------------------------------------------- view

    struct SideRef {
        LevelTree &tree;
        std::unordered_map<Price, PriceLevel *> &index;
    };

    SideRef resting_side(Side side) {
        return side == Side::Buy ? SideRef{bids, bid_index} : SideRef{asks, ask_index};
    }
    SideRef dormant_side(Side side) {
        return side == Side::Buy ? SideRef{stop_buys, stop_buy_index}
                                 : SideRef{stop_sells, stop_sell_index};
    }

    // ----------------------------------------------------------------- pools

    Order &acquire_order();        // state_pools.cpp
    void recycle_order(Order *o);  // state_pools.cpp

    PriceLevel *acquire_level();            // state_pools.cpp
    void recycle_level(PriceLevel *lvl);     // state_pools.cpp

    // ------------------------------------------------------------ levels/FIFO

    PriceLevel &level_for(SideRef side, Price price);  // state_pools.cpp
    void drop_level_if_empty(SideRef side, PriceLevel *lvl);  // state_pools.cpp
    void insert_resting(Order &o);                     // state_pools.cpp

    static void link_order(PriceLevel &lvl, Order &o) {
        o.prev = lvl.tail;
        o.next = nullptr;
        if (lvl.tail != nullptr) lvl.tail->next = &o;
        else lvl.head = &o;
        lvl.tail = &o;
        o.level = &lvl;
        lvl.total += o.quantity;
        ++lvl.depth;
    }

    static void unlink_order(PriceLevel &lvl, Order &o) {
        if (o.prev != nullptr) o.prev->next = o.next;
        else lvl.head = o.next;
        if (o.next != nullptr) o.next->prev = o.prev;
        else lvl.tail = o.prev;
        o.prev = o.next = nullptr;
        o.level = nullptr;
        lvl.total -= o.quantity;
        --lvl.depth;
    }

    static void link_stop(PriceLevel &lvl, Order &o) {
        o.stop_prev = lvl.tail;
        o.stop_next = nullptr;
        if (lvl.tail != nullptr) lvl.tail->stop_next = &o;
        else lvl.head = &o;
        lvl.tail = &o;
        o.stop_level = &lvl;
        lvl.total += o.quantity;
        ++lvl.depth;
    }

    static void unlink_stop(PriceLevel &lvl, Order &o) {
        if (o.stop_prev != nullptr) o.stop_prev->stop_next = o.stop_next;
        else lvl.head = o.stop_next;
        if (o.stop_next != nullptr) o.stop_next->stop_prev = o.stop_prev;
        else lvl.tail = o.stop_prev;
        o.stop_prev = o.stop_next = nullptr;
        o.stop_level = nullptr;
        lvl.total -= o.quantity;
        --lvl.depth;
    }

    static bool crosses(bool buy, Price limit, Price maker_price) {
        return buy ? limit >= maker_price : limit <= maker_price;
    }

    // ----------------------------------------------------------------- events

    void emit(Event ev) const { sink->on_event(ev); }

    Event make(EventKind kind, std::uint64_t order_id) const {
        Event ev;
        ev.kind = kind;
        ev.order_id = order_id;
        return ev;
    }

    void s_last(Price price) {
        has_last = true;
        last_price = price;
    }

    // -------------------------------------------------- matching (state_matching.cpp)

    // Taker phase for a transient order; publishes the last traded price.
    // Returns true when at least one trade occurred.
    bool match_taker(Order &taker);

    // Taker lifecycle for Market / MarketableLimit (and converted stops).
    bool run_taker(Order &taker);

    // ------------------------------------------------------- stops (state_matching.cpp)

    Order &pop_front_stop(Side side, PriceLevel *lvl);

    // Activates dormant stops whose trigger is met, iteratively so arbitrarily
    // deep cascades cannot blow the stack.
    void trigger_pass();

    // ------------------------------------------------ validation (state_commands.cpp)

    const char *validate(const SubmitRequest &r) const;
    const char *validate_modify(const Order &o, const ModifyRequest &m, bool dormant) const;

    // ----------------------------------------------- commands (state_commands.cpp)

    std::uint64_t submit(const SubmitRequest &req);
    bool cancel(std::uint64_t order_id);
    bool modify(std::uint64_t order_id, const ModifyRequest &req);

    // external market-data tick: may trip dormant stops
    void update_last_price(Price price);

    // ----------------------------------------------- queries (state_queries.cpp)

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    Quantity volume_at(Side side, Price price) const;
    Quantity stop_volume_at(Side side, Price trigger) const;

    std::size_t order_count() const { return by_id.size(); }

    std::size_t level_size(Side side, Price price) const;
    std::vector<Price> resting_prices(Side side) const;

    // all invariants; empty string = healthy
    std::string audit() const;

private:
    static std::int64_t audit_heights(const PriceLevel *n, const PriceLevel *parent,
                                      std::string &error);
    static void audit_side(const LevelTree &tree,
                           const std::unordered_map<Price, PriceLevel *> &index, bool dormant,
                           const char *name, std::string &error);
};

}  // namespace lob
