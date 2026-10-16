#include "core/book.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// intrusive structures
//
// PriceLevel is an AVL tree node and simultaneously the head/tail of an
// intrusive doubly-linked FIFO list of orders. Orders are pool allocated;
// resting orders occupy a book level, dormant stops occupy a stop level.
// ---------------------------------------------------------------------------

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

// AVL tree of price levels. Tree order is always ascending by price; the
// `best_is_min` flag picks which extreme is the best price:
//   resting asks, buy-stop triggers -> min first
//   resting bids, sell-stop triggers -> max first
struct OrderBook::LevelTree {
    bool best_is_min = true;
    PriceLevel *root = nullptr;
    PriceLevel *best = nullptr;  // cached extreme; O(1) reads

    explicit LevelTree(bool best_is_min_) : best_is_min(best_is_min_) {}

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
    // Returns nothing; the caller keeps walking toward the root.
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

    bool insert(PriceLevel *node) {
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
        rebalance_chain(parent);
        if (best == nullptr || (best_is_min ? node->price < best->price
                                            : node->price > best->price)) {
            best = node;
        }
#ifndef NDEBUG
        debug_full_check();
#endif
        return true;
    }

    void remove(PriceLevel *node) {
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
        // The recursion rebalances only the levels below its entry slot when
        // it navigates into children; walking from the removed node's parent
        // upward also covers removal-at-entry-frame cases.
        rebalance_chain(parent);
        if (best == node) recompute_best();
#ifndef NDEBUG
        const std::int64_t post = debug_count(root);
        if (post != pre - 1) {
            std::fprintf(stderr, "[avl] remove dropped %lld nodes (expected 1)\n",
                         (long long)(pre - post));
            std::abort();
        }
        debug_full_check();
#endif
    }

    PriceLevel *find(Price price) const {
        PriceLevel *n = root;
        while (n != nullptr) {
            if (price == n->price) return n;
            n = price < n->price ? n->left : n->right;
        }
        return nullptr;
    }

private:
    void recompute_best() {
        best = root;
        if (root == nullptr) return;
        for (;;) {
            PriceLevel *step = best_is_min ? best->left : best->right;
            if (step == nullptr) break;
            best = step;
        }
    }

    // re-walks ancestors from `start` (exclusive) restoring heights/balance
    void rebalance_chain(PriceLevel *start) {
        for (PriceLevel *cur = start; cur != nullptr;) {
            PriceLevel *p = cur->parent;
            if (p == nullptr) rebalance_here(root);
            else if (p->left == cur) rebalance_here(p->left);
            else rebalance_here(p->right);
            cur = p;
        }
    }

    PriceLevel *extract_min(PriceLevel *&ref) {
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

    void remove_rec(PriceLevel *&ref, Price key) {
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
            // extracting, that subtree may rotate, so `v->right` no longer
            // holds the pre-extract pointer. Capture it only to detect the
            // direct-child case; reattach the CURRENT subtree root after.
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
                // s was the direct right child; its subtree was detached to
                // the promoted slot during extraction and must re-parent to s
                s->right->parent = s;
            }
            s->height = v->height;  // provisional; recomputed by rebalance below
            s->parent = v->parent;
            ref = s;
            rebalance_here(ref);
        }
    }

#ifndef NDEBUG
    std::int64_t debug_count(const PriceLevel *n) const {
        return n == nullptr ? 0 : 1 + debug_count(n->left) + debug_count(n->right);
    }

    void debug_check_node(PriceLevel *n, PriceLevel *parent) {
        if (n == nullptr) return;
        if (n->parent != parent) {
            std::fprintf(stderr, "[avl] parent broken at %p price %f\n", (void *)n, n->price);
            std::abort();
        }
        debug_check_node(n->left, n);
        if (n->left != nullptr && !(n->left->price < n->price)) {
            std::fprintf(stderr, "[avl] left key violation at price %f\n", n->price);
            std::abort();
        }
        debug_check_node(n->right, n);
        if (n->right != nullptr && !(n->price < n->right->price)) {
            std::fprintf(stderr, "[avl] right key violation at price %f\n", n->price);
            std::abort();
        }
        if (n->height != 1 + std::max(height_of(n->left), height_of(n->right)) ||
            std::abs(static_cast<int>(height_of(n->left)) - static_cast<int>(height_of(n->right))) >
                1) {
            std::fprintf(stderr, "[avl] height/balance violation at price %f\n", n->price);
            std::abort();
        }
    }

    void debug_full_check() {
        debug_count(root);
        debug_check_node(root, nullptr);
    }
#endif
};

// ---------------------------------------------------------------------------
// engine state + operations
// ---------------------------------------------------------------------------

struct OrderBook::State {
    explicit State(EventSink *sink_)
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

    EventSink *sink;

    LevelTree bids;
    LevelTree asks;
    LevelTree stop_buys;   // dormant buy stops, keyed by trigger price
    LevelTree stop_sells;  // dormant sell stops, keyed by trigger price

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

    // ---------------------------------------------------------------- pools

    Order &acquire_order() {
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

    void recycle_order(Order *o) {
        by_id.erase(o->id);
        o->mark_dead();
        free_orders.push_back(o);
    }

    PriceLevel *acquire_level() {
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

    void recycle_level(PriceLevel *lvl) {
        assert(lvl->depth == 0 && lvl->total == 0);
        free_levels.push_back(lvl);
    }

    // -------------------------------------------------- level book-keeping

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

    PriceLevel &level_for(SideRef side, Price price) {
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

    void drop_level_if_empty(SideRef side, PriceLevel *lvl) {
        if (lvl->depth != 0) return;
        assert(side.tree.find(lvl->price) == lvl);  // tree/index agree at entry
        side.tree.remove(lvl);
        assert(side.tree.find(lvl->price) == nullptr);
        side.index.erase(lvl->price);
        recycle_level(lvl);
    }

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

    void insert_resting(Order &o) { link_order(level_for(resting_side(o.side), o.limit_price), o); }

    static bool crosses(bool buy, Price limit, Price maker_price) {
        return buy ? limit >= maker_price : limit <= maker_price;
    }

    // -------------------------------------------------------------- events

    void emit(Event ev) const { sink->on_event(ev); }

    Event make(EventKind kind, std::uint64_t order_id) const {
        Event ev;
        ev.kind = kind;
        ev.order_id = order_id;
        return ev;
    }

    // ------------------------------------------------------------ matching

    // Taker phase for a transient order. Consumes the opposite side from the
    // best level outward, FIFO within each level; trade price is the maker's
    // resting price. Publishes the last traded price. Returns true when at
    // least one trade occurred.
    bool match_taker(Order &taker) {
        const bool buy = (taker.side == Side::Buy);
        SideRef opp = resting_side(buy ? Side::Sell : Side::Buy);
        bool any_trade = false;
        while (taker.quantity > 0) {
            PriceLevel *lvl = opp.tree.get_best();
            if (lvl == nullptr) break;
            if (taker.type != Type::Market && !crosses(buy, taker.limit_price, lvl->price)) break;

            Order &maker = *lvl->head;
            const Quantity q = taker.quantity <= maker.quantity ? taker.quantity : maker.quantity;
            taker.quantity -= q;
            maker.quantity -= q;
            lvl->total -= q;
            s_last(lvl->price);
            any_trade = true;

            Event ev = make(EventKind::Trade, maker.id);
            ev.taker_order_id = taker.id;
            ev.price = lvl->price;
            ev.quantity = q;
            emit(ev);

            ev = make(EventKind::Filled, maker.id);
            ev.price = lvl->price;
            ev.quantity = q;
            ev.remaining = maker.quantity;
            emit(ev);

            ev = make(EventKind::Filled, taker.id);
            ev.price = lvl->price;
            ev.quantity = q;
            ev.remaining = taker.quantity;
            emit(ev);

            if (maker.quantity == 0) {
                unlink_order(*lvl, maker);
                drop_level_if_empty(opp, lvl);
                recycle_order(&maker);
            }
        }
        return any_trade;
    }

    // Taker lifecycle for Market / MarketableLimit (and converted stops).
    // Market remainder is cancelled; crossing-limit remainder rests. Returns
    // true when at least one trade occurred (the caller then checks stop
    // triggers - kept iterative to support arbitrarily deep cascades).
    bool run_taker(Order &taker) {
        const bool traded = match_taker(taker);
        if (taker.quantity > 0) {
            if (taker.type == Type::Market) {
                Event ev = make(EventKind::Cancelled, taker.id);
                ev.quantity = taker.quantity;
                emit(ev);
                recycle_order(&taker);
            } else {
                insert_resting(taker);  // remainder rests at the limit price
            }
        } else {
            recycle_order(&taker);
        }
        return traded;
    }

    void s_last(Price price) {
        has_last = true;
        last_price = price;
    }

    // ------------------------------------------------------- stop triggers

    Order &pop_front_stop(Side side, PriceLevel *lvl) {
        Order &o = *lvl->head;
        unlink_stop(*lvl, o);
        drop_level_if_empty(dormant_side(side), lvl);
        return o;
    }

    // Activates every dormant stop whose trigger is met by the last traded
    // price, in trigger order (ascending for buys, descending for sells),
    // FIFO within a trigger. Conversions can trade, which moves the last
    // price and can trigger further stops - hence the drain loop.
    void trigger_pass() {
        if (!has_last) return;
        for (;;) {
            PriceLevel *buy_lvl = stop_buys.empty() ? nullptr : stop_buys.get_best();
            PriceLevel *sell_lvl = stop_sells.empty() ? nullptr : stop_sells.get_best();
            const bool buy_fires = buy_lvl != nullptr && buy_lvl->price <= last_price;
            const bool sell_fires = sell_lvl != nullptr && sell_lvl->price >= last_price;
            if (!buy_fires && !sell_fires) break;

            // deterministic order when both sides fire: buy stops first;
            // ties keep FIFO within the trigger level
            Order &next = buy_fires ? pop_front_stop(Side::Buy, buy_lvl)
                                    : pop_front_stop(Side::Sell, sell_lvl);

            emit(make(EventKind::Triggered, next.id));
            if (next.type == Type::Stop) next.type = Type::Market;
            else next.type = Type::MarketableLimit;
            run_taker(next);
        }
    }

    // --------------------------------------------------------- validation

    const char *validate(const SubmitRequest &r) const {
        if (r.quantity <= 0) return "quantity-must-be-positive";
        switch (r.type) {
            case Type::Market:
                break;
            case Type::Limit:
            case Type::MarketableLimit:
                if (!(r.limit_price > 0.0)) return "limit-price-must-be-positive";
                break;
            case Type::Stop:
                if (!(r.stop_price > 0.0)) return "stop-price-must-be-positive";
                break;
            case Type::StopLimit:
                if (!(r.stop_price > 0.0)) return "stop-price-must-be-positive";
                if (!(r.limit_price > 0.0)) return "limit-price-must-be-positive";
                break;
        }
        return nullptr;
    }

    const char *validate_modify(const Order &o, const ModifyRequest &m, bool dormant) const {
        if (m.has_new_quantity && m.new_quantity <= 0) return "quantity-must-be-positive";
        if (m.has_new_price) {
            if (o.type == Type::Stop || o.type == Type::Market) return "price-not-applicable";
            if (!(m.new_price > 0.0)) return "price-must-be-positive";
        }
        if (m.has_new_stop_price) {
            if ((o.type != Type::Stop && o.type != Type::StopLimit) || !dormant) {
                return "stop-price-not-applicable";
            }
            if (!(m.new_stop_price > 0.0)) return "stop-price-must-be-positive";
        }
        return nullptr;
    }

    // -------------------------------------------------------------- public

    std::uint64_t submit(const SubmitRequest &req) {
        if (req.order_id != 0) next_id = std::max(next_id, req.order_id);
        const bool provided_id = req.order_id != 0;
        std::uint64_t id = provided_id ? req.order_id : ++next_id;

        const char *reason = validate(req);
        if (provided_id && by_id.count(req.order_id) != 0) reason = "duplicate-order-id";
        if (reason != nullptr) {
            Event ev = make(EventKind::Rejected, id);
            ev.reason = reason;
            emit(ev);
            return id;
        }

        Order &o = acquire_order();
        o.set(req.type, req.side, req.quantity, req.limit_price, req.stop_price, id);
        by_id[id] = &o;

        switch (req.type) {
            case Type::Market:
            case Type::MarketableLimit: {
                emit(make(EventKind::Accepted, id));
                if (run_taker(o)) trigger_pass();
                return id;
            }
            case Type::Limit: {
                const bool buy = (req.side == Side::Buy);
                SideRef opp = resting_side(buy ? Side::Sell : Side::Buy);
                const PriceLevel *best = opp.tree.get_best();
                if (best != nullptr &&
                    (buy ? req.limit_price >= best->price : req.limit_price <= best->price)) {
                    recycle_order(&o);
                    Event ev = make(EventKind::Rejected, id);
                    ev.reason = "would-cross-book";
                    emit(ev);
                    return id;
                }
                insert_resting(o);
                emit(make(EventKind::Accepted, id));
                return id;
            }
            case Type::Stop:
            case Type::StopLimit: {
                link_stop(level_for(dormant_side(req.side), req.stop_price), o);
                emit(make(EventKind::Accepted, id));
                trigger_pass();  // activates immediately if market already beyond trigger
                return id;
            }
        }
        assert(false);
        return id;
    }

    bool cancel(std::uint64_t order_id) {
        auto it = by_id.find(order_id);
        if (it == by_id.end()) {
            Event ev = make(EventKind::Rejected, order_id);
            ev.reason = "unknown-order";
            emit(ev);
            return false;
        }
        Order &o = *it->second;
        if (o.level != nullptr) {
            PriceLevel *lvl = o.level;
            unlink_order(*lvl, o);
            drop_level_if_empty(resting_side(o.side), lvl);
        } else {
            assert(o.stop_level != nullptr);
            PriceLevel *lvl = o.stop_level;
            unlink_stop(*lvl, o);
            drop_level_if_empty(dormant_side(o.side), lvl);
        }
        Event ev = make(EventKind::Cancelled, o.id);
        ev.quantity = o.quantity;
        emit(ev);
        recycle_order(&o);
        return true;
    }

    bool modify(std::uint64_t order_id, const ModifyRequest &req) {
        auto it = by_id.find(order_id);
        if (it == by_id.end()) {
            Event ev = make(EventKind::Rejected, order_id);
            ev.reason = "unknown-order";
            emit(ev);
            return false;
        }
        Order &o = *it->second;
        const bool dormant = (o.stop_level != nullptr);
        const char *reason = validate_modify(o, req, dormant);
        if (reason != nullptr) {
            Event ev = make(EventKind::Rejected, order_id);
            ev.reason = reason;
            emit(ev);
            return false;
        }

        bool changed = false;
        if (dormant) {
            if (req.has_new_stop_price && req.new_stop_price != o.stop_price) {
                PriceLevel *lvl = o.stop_level;
                unlink_stop(*lvl, o);
                drop_level_if_empty(dormant_side(o.side), lvl);
                o.stop_price = req.new_stop_price;
                link_stop(level_for(dormant_side(o.side), o.stop_price), o);
                changed = true;
            }
            if (req.has_new_price && req.new_price != o.limit_price) {
                o.limit_price = req.new_price;
                changed = true;
            }
            if (req.has_new_quantity && req.new_quantity != o.quantity) {
                o.stop_level->total += req.new_quantity - o.quantity;
                o.quantity = req.new_quantity;
                changed = true;
            }
        } else {
            const Price new_price = req.has_new_price ? req.new_price : o.limit_price;
            const Quantity new_qty = req.has_new_quantity ? req.new_quantity : o.quantity;
            const bool rebook = (req.has_new_price && new_price != o.limit_price) ||
                                (req.has_new_quantity && new_qty > o.quantity);
            if (rebook) {
                PriceLevel *lvl = o.level;
                unlink_order(*lvl, o);
                drop_level_if_empty(resting_side(o.side), lvl);
                o.limit_price = new_price;
                o.quantity = new_qty;
                insert_resting(o);
                changed = true;
            } else if (req.has_new_quantity && new_qty != o.quantity) {
                // pure quantity reduction: time priority kept, O(1)
                o.level->total -= o.quantity - new_qty;
                o.quantity = new_qty;
                changed = true;
            }
        }

        if (changed) {
            Event ev = make(EventKind::Modified, o.id);
            ev.price = o.limit_price;
            ev.quantity = o.quantity;
            emit(ev);
        }
        return true;
    }

    // -------------------------------------------------------------- queries

    std::optional<Price> best_bid() const {
        const PriceLevel *lvl = bids.get_best();
        return lvl != nullptr ? std::optional<Price>(lvl->price) : std::nullopt;
    }

    std::optional<Price> best_ask() const {
        const PriceLevel *lvl = asks.get_best();
        return lvl != nullptr ? std::optional<Price>(lvl->price) : std::nullopt;
    }

    Quantity volume_at(Side side, Price price) const {
        const auto &index = side == Side::Buy ? bid_index : ask_index;
        auto it = index.find(price);
        return it != index.end() ? it->second->total : 0;
    }

    Quantity stop_volume_at(Side side, Price trigger) const {
        const auto &index = side == Side::Buy ? stop_buy_index : stop_sell_index;
        auto it = index.find(trigger);
        return it != index.end() ? it->second->total : 0;
    }

    std::size_t order_count() const { return by_id.size(); }

    // ---------------------------------------------------------------- audit

    std::string audit() const {
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

private:
    static std::int64_t audit_heights(const PriceLevel *n, const PriceLevel *parent,
                                      std::string &error) {
        if (n == nullptr) return 0;
        if (n->parent != parent) {
            if (error.empty())
                error = "broken AVL parent pointer at price " + std::to_string(n->price);
        }
        const std::int64_t hl = audit_heights(n->left, n, error);
        const std::int64_t hr = audit_heights(n->right, n, error);
        if (n->height != static_cast<std::uint32_t>(1 + std::max(hl, hr))) {
            if (error.empty()) error = "stale AVL height at price " + std::to_string(n->price);
        }
        if (std::abs(hl - hr) > 1) {
            if (error.empty())
                error = "unbalanced AVL subtree at price " + std::to_string(n->price);
        }
        return n->height;
    }

    static void audit_side(const LevelTree &tree,
                           const std::unordered_map<Price, PriceLevel *> &index, bool dormant,
                           const char *name, std::string &error) {
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
};

// ---------------------------------------------------------------------------
// public facade
// ---------------------------------------------------------------------------

OrderBook::OrderBook(EventSink *sink) : state_(new State(sink)) {}
OrderBook::~OrderBook() = default;

std::uint64_t OrderBook::submit(const SubmitRequest &request) { return state_->submit(request); }
bool OrderBook::cancel(std::uint64_t order_id) { return state_->cancel(order_id); }
bool OrderBook::modify(std::uint64_t order_id, const ModifyRequest &request) {
    return state_->modify(order_id, request);
}
std::optional<Price> OrderBook::best_bid() const { return state_->best_bid(); }
std::optional<Price> OrderBook::best_ask() const { return state_->best_ask(); }
std::optional<Price> OrderBook::last_trade_price() const {
    return state_->has_last ? std::optional<Price>(state_->last_price) : std::nullopt;
}
Quantity OrderBook::volume_at(Side side, Price price) const {
    return state_->volume_at(side, price);
}
Quantity OrderBook::stop_volume_at(Side side, Price trigger) const {
    return state_->stop_volume_at(side, trigger);
}
std::size_t OrderBook::order_count() const { return state_->order_count(); }
std::string OrderBook::audit() const { return state_->audit(); }

}  // namespace lob
