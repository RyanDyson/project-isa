// AVL implementation for LevelTree (see level_tree.hpp for the structure).
#include "core/level_tree.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace lob {

OrderBook::LevelTree::LevelTree(bool best_is_min_) : best_is_min(best_is_min_) {}

bool OrderBook::LevelTree::insert(PriceLevel *node) {
    node->parent = nullptr;
    node->left = node->right = nullptr;
    node->height = 1;
    PriceLevel *parent = nullptr;
    PriceLevel **link = &root;
    while (*link != nullptr) {
        PriceLevel *v = *link;
        if (v->price == node->price) return false;  // duplicates prevented via index map
        parent = v;
        link = node->price < v->price ? &v->left : &v->right;
    }
    *link = node;
    node->parent = parent;
    // AVL insert fix-up: heights only propagate upward while a subtree's
    // height actually changes; a single or double rotation at the lowest
    // unbalanced ancestor restores the pre-insert height, so the walk
    // terminates early in the common (already balanced) case.
    for (PriceLevel *cur = parent; cur != nullptr;) {
        PriceLevel *p = cur->parent;
        const bool was_left = (p != nullptr && p->left == cur);
        const std::uint32_t pre_h = cur->height;  // read before rebalance updates it
        if (p == nullptr) rebalance_here(root);
        else if (was_left) rebalance_here(p->left);
        else rebalance_here(p->right);
        // The slot content may have been re-rooted by a rotation, but the
        // slot itself is fixed; its new content's height is the subtree's
        // new height.
        const PriceLevel *sub_root = p == nullptr ? root : (was_left ? p->left : p->right);
        if (sub_root->height == pre_h) break;  // ancestors unaffected
        cur = p;
    }
    if (best == nullptr || (best_is_min ? node->price < best->price
                                        : node->price > best->price)) {
        best = node;
    }
#ifndef NDEBUG
    debug_full_check();
#endif
    return true;
}

void OrderBook::LevelTree::remove(PriceLevel *node) {
    PriceLevel *parent = node->parent;
#ifndef NDEBUG
    const std::int64_t pre = debug_count(root);
#endif
    if (parent == nullptr) {
        remove_rec(root, node->price);
    } else if (parent->left == node) {
        remove_rec(parent->left, node->price);
    } else {
        remove_rec(parent->right, node->price);
    }
    // The recursion rebalances only the levels below its entry slot when it
    // navigates into children; walking from the removed node's parent upward
    // also covers removal-at-entry-frame cases.
    rebalance_chain(parent);
    if (best == node) recompute_best();
#ifndef NDEBUG
    const std::int64_t post = debug_count(root);
    if (post != pre - 1) {
        std::fprintf(stderr, "[avl] remove dropped %lld nodes (expected 1)\n",
                     static_cast<long long>(pre - post));
        std::abort();
    }
    debug_full_check();
#endif
}

void OrderBook::LevelTree::recompute_best(void) {
    best = root;
    if (root == nullptr) return;
    for (;;) {
        PriceLevel *step = best_is_min ? best->left : best->right;
        if (step == nullptr) break;
        best = step;
    }
}

// Re-walks ancestors from `start` (exclusive) restoring heights/balance.
void OrderBook::LevelTree::rebalance_chain(PriceLevel *start) {
    for (PriceLevel *cur = start; cur != nullptr;) {
        PriceLevel *p = cur->parent;
        if (p == nullptr) rebalance_here(root);
        else if (p->left == cur) rebalance_here(p->left);
        else rebalance_here(p->right);
        cur = p;
    }
}

OrderBook::PriceLevel *OrderBook::LevelTree::extract_min(PriceLevel *&ref) {
    PriceLevel *v = ref;
    if (v->left == nullptr) {
        ref = v->right;
        if (ref != nullptr) ref->parent = v->parent;
        return v;
    }
    PriceLevel *m = extract_min(v->left);
    rebalance_here(ref);
    return m;
}

void OrderBook::LevelTree::remove_rec(PriceLevel *&ref, Price key) {
    PriceLevel *v = ref;
    if (v == nullptr) return;
    if (key < v->price) {
        remove_rec(v->left, key);
        rebalance_here(ref);
    } else if (v->price < key) {
        remove_rec(v->right, key);
        rebalance_here(ref);
    } else if (v->left == nullptr) {
        ref = v->right;
        if (ref != nullptr) ref->parent = v->parent;
    } else if (v->right == nullptr) {
        ref = v->left;
        ref->parent = v->parent;
    } else {
        // The successor s must be extracted from the right subtree. While
        // extracting, that subtree may rotate, so `v->right` no longer holds
        // the pre-extract pointer. Capture it only to detect the direct-child
        // case; reattach the CURRENT subtree root after.
        PriceLevel *pre_right = v->right;
        PriceLevel *s = extract_min(v->right);
        s->left = v->left;
        s->left->parent = s;
        if (s != pre_right) {
            // s came from deeper inside; v->right now holds the (possibly
            // rebalanced) remainder of the right subtree
            s->right = v->right;
            s->right->parent = s;
        } else if (s->right != nullptr) {
            // s was the direct right child; its subtree was detached to the
            // promoted slot during extraction and must re-parent to s
            s->right->parent = s;
        }
        s->height = v->height;  // provisional; recomputed by rebalance below
        s->parent = v->parent;
        ref = s;
        rebalance_here(ref);
    }
}

#ifndef NDEBUG

std::int64_t OrderBook::LevelTree::debug_count(const PriceLevel *n) const {
    return n == nullptr ? 0 : 1 + debug_count(n->left) + debug_count(n->right);
}

void OrderBook::LevelTree::debug_full_check(void) {
    // full verification: parent pointers, strict key order, exact AVL heights
    struct Checker {
        static std::int64_t node(PriceLevel *n, PriceLevel *parent) {
            if (n == nullptr) return 0;
            if (n->parent != parent) {
                std::fprintf(stderr, "[avl] parent broken at %p price %f\n",
                             static_cast<void *>(n), n->price);
                std::abort();
            }
            const std::int64_t hl = node(n->left, n);
            const std::int64_t hr = node(n->right, n);
            if (n->left != nullptr && !(n->left->price < n->price)) {
                std::fprintf(stderr, "[avl] left key violation at price %f\n", n->price);
                std::abort();
            }
            if (n->right != nullptr && !(n->price < n->right->price)) {
                std::fprintf(stderr, "[avl] right key violation at price %f\n", n->price);
                std::abort();
            }
            if (n->height != static_cast<std::uint32_t>(1 + std::max(hl, hr)) ||
                std::abs(hl - hr) > 1) {
                std::fprintf(stderr, "[avl] height/balance violation at price %f\n", n->price);
                std::abort();
            }
            return n->height;
        }
    };
    debug_count(root);
    Checker::node(root, nullptr);
}

#endif  // NDEBUG

}  // namespace lob
