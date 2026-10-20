// AVL tree of price levels - the ordered index for one side of the book.
//
// Tree order is always ascending by price; the `best_is_min` flag picks
// which extreme is the best price:
//   resting asks, buy-stop triggers -> min first
//   resting bids, sell-stop triggers -> max first
//
// Internal module (not part of the public API).
#pragma once

#include <algorithm>
#include <cstdint>

#include "core/book.hpp"
#include "core/nodes.hpp"

namespace lob {

struct OrderBook::LevelTree {
    bool best_is_min = true;
    PriceLevel *root = nullptr;
    PriceLevel *best = nullptr;  // cached extreme; O(1) reads

    explicit LevelTree(bool best_is_min_);  // level_tree.cpp

    bool empty() const { return root == nullptr; }
    PriceLevel *get_best() const { return best; }

    static std::uint32_t height_of(const PriceLevel *n) { return n ? n->height : 0; }
    static void update_height(PriceLevel *n) {
        n->height = 1 + std::max(height_of(n->left), height_of(n->right));
    }

    static PriceLevel *rotate_right(PriceLevel *a) {
        PriceLevel *l = a->left;
        a->left = l->right;
        if (a->left != nullptr) a->left->parent = a;
        l->right = a;
        a->parent = l;
        update_height(a);
        update_height(l);
        return l;
    }

    static PriceLevel *rotate_left(PriceLevel *a) {
        PriceLevel *r = a->right;
        a->right = r->left;
        if (a->right != nullptr) a->right->parent = a;
        r->left = a;
        a->parent = r;
        update_height(a);
        update_height(r);
        return r;
    }

    // Rebalances the subtree rooted at `*ref` (the parent's child pointer or
    // the root itself) and assigns the new subtree root back into `*ref`.
    // Hot path: kept inline for the book's insert/remove fix-up walks.
    void rebalance_here(PriceLevel *&ref) {
        PriceLevel *v = ref;
        update_height(v);
        const auto balance =
            static_cast<int>(height_of(v->left)) - static_cast<int>(height_of(v->right));
        if (balance > 1) {  // left heavy
            if (height_of(v->left->left) < height_of(v->left->right)) {
                v->left = rotate_left(v->left);
                v->left->parent = v;
            }
            PriceLevel *p = v->parent;
            ref = rotate_right(v);
            ref->parent = p;
        } else if (balance < -1) {  // right heavy
            if (height_of(v->right->left) > height_of(v->right->right)) {
                v->right = rotate_right(v->right);
                v->right->parent = v;
            }
            PriceLevel *p = v->parent;
            ref = rotate_left(v);
            ref->parent = p;
        }
    }

    // O(log M) insert of a prepared level node; false iff the price already
    // exists in this tree (the engine's price index prevents that).
    bool insert(PriceLevel *node);  // level_tree.cpp

    // O(log M) removal of a level node currently in this tree.
    void remove(PriceLevel *node);  // level_tree.cpp

    // O(log M) exact-key lookup.
    PriceLevel *find(Price price) const {
        PriceLevel *n = root;
        while (n != nullptr) {
            if (price == n->price) return n;
            n = price < n->price ? n->left : n->right;
        }
        return nullptr;
    }

private:
    void recompute_best(void);                 // level_tree.cpp
    void rebalance_chain(PriceLevel *start);   // level_tree.cpp
    PriceLevel *extract_min(PriceLevel *&ref);  // level_tree.cpp
    void remove_rec(PriceLevel *&ref, Price key);  // level_tree.cpp

#ifndef NDEBUG
    std::int64_t debug_count(const PriceLevel *n) const;
    void debug_full_check(void);
#endif
};

}  // namespace lob
