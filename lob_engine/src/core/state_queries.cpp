// Queries (best prices, volumes) and the full invariant audit.
#include "core/state.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lob {

std::optional<Price> OrderBook::State::best_bid() const {
    const PriceLevel *lvl = bids.get_best();
    return lvl != nullptr ? std::optional<Price>(lvl->price) : std::nullopt;
}

std::optional<Price> OrderBook::State::best_ask() const {
    const PriceLevel *lvl = asks.get_best();
    return lvl != nullptr ? std::optional<Price>(lvl->price) : std::nullopt;
}

Quantity OrderBook::State::volume_at(Side side, Price price) const {
    const auto &index = side == Side::Buy ? bid_index : ask_index;
    auto it = index.find(price);
    return it != index.end() ? it->second->total : 0;
}

Quantity OrderBook::State::stop_volume_at(Side side, Price trigger) const {
    const auto &index = side == Side::Buy ? stop_buy_index : stop_sell_index;
    auto it = index.find(trigger);
    return it != index.end() ? it->second->total : 0;
}

// ------------------------------------------------------------------- audit

std::string OrderBook::State::audit() const {
    std::string error;
    audit_side(bids, bid_index, false, "bids", error);
    audit_side(asks, ask_index, false, "asks", error);
    audit_side(stop_buys, stop_buy_index, true, "stop-buys", error);
    audit_side(stop_sells, stop_sell_index, true, "stop-sells", error);
    if (!error.empty()) return error;

    // by_id must be a bijection with live orders across all levels
    std::size_t live = 0;
    const struct {
        const LevelTree *tree;
        bool dormant;
    } trees[] = {{&bids, false}, {&asks, false}, {&stop_buys, true}, {&stop_sells, true}};
    for (const auto &side : trees) {
        // start the successor walk at the leftmost (minimum) node
        PriceLevel *start = side.tree->root;
        if (start != nullptr) {
            while (start->left != nullptr) start = start->left;
        }
        for (PriceLevel *lvl = start; lvl != nullptr;) {
            // in-order iteration via successor walk
            PriceLevel *next = lvl->right;
            if (next == nullptr) {
                PriceLevel *p = lvl;
                while (p->parent != nullptr && p == p->parent->right) p = p->parent;
                next = p->parent;
            } else {
                while (next->left != nullptr) next = next->left;
            }
            for (const Order *o = lvl->head; o != nullptr;
                 o = side.dormant ? o->stop_next : o->next) {
                ++live;
            }
            lvl = next;
        }
    }
    if (live != by_id.size()) {
        error = "by_id size (" + std::to_string(by_id.size()) +
                ") inconsistent with linked orders (" + std::to_string(live) + ")";
        return error;
    }
    for (const auto &entry : by_id) {
        const Order *o = entry.second;
        if (o->id != entry.first) {
            error = "by_id key mismatch for order " + std::to_string(entry.first);
            return error;
        }
        if (o->quantity <= 0) {
            error = "live order " + std::to_string(o->id) + " has non-positive quantity";
            return error;
        }
        if (o->level == nullptr && o->stop_level == nullptr) {
            error = "order " + std::to_string(o->id) + " not linked to any level";
            return error;
        }
    }
    return error;
}

std::int64_t OrderBook::State::audit_heights(const PriceLevel *n, const PriceLevel *parent,
                                             std::string &error) {
    if (n == nullptr) return 0;
    if (n->parent != parent) {
        if (error.empty()) error = "broken AVL parent pointer at price " + std::to_string(n->price);
    }
    const std::int64_t hl = audit_heights(n->left, n, error);
    const std::int64_t hr = audit_heights(n->right, n, error);
    if (n->height != static_cast<std::uint32_t>(1 + std::max(hl, hr))) {
        if (error.empty()) error = "stale AVL height at price " + std::to_string(n->price);
    }
    if (std::abs(hl - hr) > 1) {
        if (error.empty()) error = "unbalanced AVL subtree at price " + std::to_string(n->price);
    }
    return n->height;
}

void OrderBook::State::audit_side(const LevelTree &tree,
                                  const std::unordered_map<Price, PriceLevel *> &index,
                                  bool dormant, const char *name, std::string &error) {
    if (error.empty()) audit_heights(tree.root, nullptr, error);
    if (!error.empty()) return;

    // in-order traversal must be strictly ascending and match the index
    std::vector<const PriceLevel *> stack;
    Price prev_price = 0.0;
    bool first = true;
    std::size_t seen = 0;
    const PriceLevel *n = tree.root;
    while (n != nullptr || !stack.empty()) {
        while (n != nullptr) {
            stack.push_back(n);
            n = n->left;
        }
        n = stack.back();
        stack.pop_back();
        if (!first && !(prev_price < n->price)) {
            if (error.empty())
                error = std::string(name) + ": non-strict price ordering at " +
                        std::to_string(n->price);
            return;
        }
        prev_price = n->price;
        first = false;
        ++seen;
        auto it = index.find(n->price);
        if (it == index.end() || it->second != n) {
            if (error.empty())
                error = std::string(name) + ": index map missing level " +
                        std::to_string(n->price);
            return;
        }
        // FIFO list of the level
        const Order *prev = nullptr;
        Quantity sum = 0;
        std::size_t depth = 0;
        for (const Order *o = n->head; o != nullptr; o = dormant ? o->stop_next : o->next) {
            if ((dormant ? o->stop_prev : o->prev) != prev) {
                if (error.empty())
                    error = std::string(name) + ": broken FIFO prev link in level " +
                            std::to_string(n->price);
                return;
            }
            const PriceLevel *attached = dormant ? o->stop_level : o->level;
            if (attached != n) {
                if (error.empty())
                    error = std::string(name) + ": order " + std::to_string(o->id) +
                            " points to wrong level";
                return;
            }
            if (o->quantity <= 0) {
                if (error.empty())
                    error = std::string(name) + ": order " + std::to_string(o->id) +
                            " has non-positive quantity";
                return;
            }
            if ((depth == 0) != (n->head == o)) {
                if (error.empty())
                    error = std::string(name) + ": FIFO head/tail inconsistency in level " +
                            std::to_string(n->price);
                return;
            }
            prev = o;
            sum += o->quantity;
            ++depth;
        }
        if (depth != n->depth || sum != n->total) {
            if (error.empty()) {
                std::string members;
                for (const Order *o = n->head; o != nullptr;
                     o = dormant ? o->stop_next : o->next) {
                    members += " #" + std::to_string(o->id) + "x" + std::to_string(o->quantity);
                }
                error = std::string(name) + ": level " + std::to_string(n->price) +
                        " totals inconsistent (total=" + std::to_string(n->total) +
                        " sum=" + std::to_string(sum) + " depth=" + std::to_string(n->depth) +
                        " seen=" + std::to_string(depth) + " members:" + members + ")";
            }
            return;
        }
        if (depth > 0 && n->tail != prev) {
            if (error.empty())
                error = std::string(name) + ": stale tail in level " + std::to_string(n->price);
            return;
        }
        if (n == tree.best && depth == 0) {
            if (error.empty()) error = std::string(name) + ": cached best level is empty";
            return;
        }
        n = n->right;
    }
    if (seen != index.size()) {
        if (error.empty()) {
            std::string extra;
            for (const auto &e : index) {
                if (tree.find(e.first) != e.second) extra += " " + std::to_string(e.first);
            }
            error = std::string(name) + ": index map size " + std::to_string(index.size()) +
                    " differs from tree levels " + std::to_string(seen) +
                    " (index-only/fast-wrong:" + extra + ")";
        }
        return;
    }
    // cached best must equal the extreme
    const PriceLevel *expect = tree.root;
    if (expect != nullptr) {
        while ((tree.best_is_min ? expect->left : expect->right) != nullptr) {
            expect = tree.best_is_min ? expect->left : expect->right;
        }
    }
    if (expect != tree.best) {
        if (error.empty()) error = std::string(name) + ": cached best pointer stale";
    }
}

}  // namespace lob
