// Matching: taker sweeps, taker lifecycle (market/crossing-limit), and the
// dormant-stop trigger drain.
#include "core/state.hpp"

namespace lob {

bool OrderBook::State::match_taker(Order &taker) {
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

bool OrderBook::State::run_taker(Order &taker) {
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

OrderBook::Order &OrderBook::State::pop_front_stop(Side side, PriceLevel *lvl) {
    Order &o = *lvl->head;
    unlink_stop(*lvl, o);
    drop_level_if_empty(dormant_side(side), lvl);
    return o;
}

// Activates every dormant stop whose trigger is met by the last traded
// price, in trigger order (ascending for buys, descending for sells), FIFO
// within a trigger. Conversions can trade, which moves the last price and
// can trigger further stops - hence the drain loop.
void OrderBook::State::trigger_pass() {
    if (!has_last) return;
    for (;;) {
        PriceLevel *buy_lvl = stop_buys.empty() ? nullptr : stop_buys.get_best();
        PriceLevel *sell_lvl = stop_sells.empty() ? nullptr : stop_sells.get_best();
        const bool buy_fires = buy_lvl != nullptr && buy_lvl->price <= last_price;
        const bool sell_fires = sell_lvl != nullptr && sell_lvl->price >= last_price;
        if (!buy_fires && !sell_fires) break;

        // deterministic order when both sides fire: buy stops first; ties
        // keep FIFO within the trigger level
        Order &next = buy_fires ? pop_front_stop(Side::Buy, buy_lvl)
                                : pop_front_stop(Side::Sell, sell_lvl);

        emit(make(EventKind::Triggered, next.id));
        if (next.type == Type::Stop) next.type = Type::Market;
        else next.type = Type::MarketableLimit;
        run_taker(next);
    }
}

// External market-data feed (the ingestion pipeline's price tick).
void OrderBook::State::update_last_price(Price price) {
    s_last(price);
    trigger_pass();
}

}  // namespace lob
