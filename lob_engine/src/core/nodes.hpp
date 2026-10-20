// Internal node types of the matching engine.
//
// PriceLevel is an AVL tree node and simultaneously the head/tail of an
// intrusive doubly-linked FIFO list of orders. Orders are pool allocated;
// resting orders occupy a book level, dormant stops occupy a stop level.
//
// Internal module (not part of the public API).
#pragma once

#include <cstdint>

#include "core/book.hpp"

namespace lob {

struct OrderBook::Order {
    std::uint64_t id = 0;
    Side side = Side::Buy;
    Type type = Type::Limit;
    Price limit_price = 0.0;
    Price stop_price = 0.0;
    Quantity quantity = 0;  // remaining

    // FIFO links inside the resting book (levels keyed by limit price)
    Order *prev = nullptr;
    Order *next = nullptr;
    PriceLevel *level = nullptr;

    // FIFO links inside the dormant stop book (levels keyed by trigger price)
    Order *stop_prev = nullptr;
    Order *stop_next = nullptr;
    PriceLevel *stop_level = nullptr;

    void set(Type t, Side s, Quantity q, Price limit, Price stop, std::uint64_t i) {
        id = i;
        side = s;
        type = t;
        limit_price = limit;
        stop_price = stop;
        quantity = q;
    }
    void mark_dead() {
        prev = next = nullptr;
        level = nullptr;
        stop_prev = stop_next = nullptr;
        stop_level = nullptr;
    }
};

struct OrderBook::PriceLevel {
    Price price = 0.0;
    Quantity total = 0;      // invariant: == sum of member order quantities
    Order *head = nullptr;   // FIFO front (oldest)
    Order *tail = nullptr;   // FIFO back (newest)
    std::size_t depth = 0;   // number of orders linked here
    PriceLevel *parent = nullptr;
    PriceLevel *left = nullptr;
    PriceLevel *right = nullptr;
    std::uint32_t height = 1;
};

}  // namespace lob
