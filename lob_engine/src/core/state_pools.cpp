// State constructor, object pools, and price-level book-keeping.
#include "core/state.hpp"

#include <algorithm>
#include <cassert>

namespace lob {

OrderBook::State::State(EventSink *sink_)
    : sink(sink_ != nullptr ? sink_ : fallback_sink()),
      bids(false),      // best bid = highest price
      asks(true),       // best ask = lowest price
      stop_buys(true),  // buy stops: trigger scan from lowest trigger up
      stop_sells(false) {
    bid_index.reserve(1024);
    ask_index.reserve(1024);
    stop_buy_index.reserve(256);
    stop_sell_index.reserve(256);
    by_id.reserve(4096);
}

// ------------------------------------------------------------------- pools

OrderBook::Order &OrderBook::State::acquire_order() {
    if (!free_orders.empty()) {
        Order &o = *free_orders.back();
        free_orders.pop_back();
        o.stop_prev = o.stop_next = nullptr;
        o.stop_level = nullptr;
        return o;
    }
    order_pool.emplace_back();
    return order_pool.back();
}

void OrderBook::State::recycle_order(Order *o) {
    by_id.erase(o->id);
    o->mark_dead();
    free_orders.push_back(o);
}

OrderBook::PriceLevel *OrderBook::State::acquire_level() {
    if (!free_levels.empty()) {
        PriceLevel *lvl = free_levels.back();
        free_levels.pop_back();
        lvl->total = 0;
        lvl->head = lvl->tail = nullptr;
        lvl->depth = 0;
        lvl->parent = lvl->left = lvl->right = nullptr;
        lvl->height = 1;
        return lvl;
    }
    level_pool.emplace_back();
    return &level_pool.back();
}

void OrderBook::State::recycle_level(PriceLevel *lvl) {
    assert(lvl->depth == 0 && lvl->total == 0);
    free_levels.push_back(lvl);
}

// ------------------------------------------------------ level book-keeping

OrderBook::PriceLevel &OrderBook::State::level_for(SideRef side, Price price) {
    auto it = side.index.find(price);
    if (it != side.index.end()) {
        assert(side.tree.find(price) == it->second);  // index entry is the tree node
        return *it->second;
    }
    PriceLevel *lvl = acquire_level();
    lvl->price = price;
    lvl->total = 0;
    lvl->head = lvl->tail = nullptr;
    lvl->depth = 0;
    const bool inserted = side.tree.insert(lvl);
    assert(inserted);
    (void)inserted;  // silence unused-var in release builds
    side.index.emplace(price, lvl);
    assert(side.tree.find(price) == lvl);
    return *lvl;
}

void OrderBook::State::drop_level_if_empty(SideRef side, PriceLevel *lvl) {
    if (lvl->depth != 0) return;
    assert(side.tree.find(lvl->price) == lvl);  // tree/index agree at entry
    side.tree.remove(lvl);
    assert(side.tree.find(lvl->price) == nullptr);
    side.index.erase(lvl->price);
    recycle_level(lvl);
}

void OrderBook::State::insert_resting(Order &o) {
    link_order(level_for(resting_side(o.side), o.limit_price), o);
}

}  // namespace lob
