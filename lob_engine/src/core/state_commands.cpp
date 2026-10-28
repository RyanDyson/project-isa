// Commands: request validation and the submit / cancel / modify paths.
#include "core/state.hpp"

#include <algorithm>
#include <cassert>

namespace lob {

const char *OrderBook::State::validate(const SubmitRequest &r) const {
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

const char *OrderBook::State::validate_modify(const Order &o, const ModifyRequest &m,
                                              bool dormant) const {
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

std::uint64_t OrderBook::State::submit(const SubmitRequest &req) {
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

bool OrderBook::State::cancel(std::uint64_t order_id) {
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

bool OrderBook::State::modify(std::uint64_t order_id, const ModifyRequest &req) {

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
            trigger_pass();  // a moved trigger past the market fires immediately
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

}  // namespace lob

