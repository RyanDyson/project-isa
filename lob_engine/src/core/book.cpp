// Public facade of the LOB matching core. The internals live in:
//   nodes.hpp                internal node types (Order / PriceLevel)
//   level_tree.{hpp,cpp}     AVL tree of price levels
//   state.hpp                engine state definition (data + inline hot paths)
//   state_pools.cpp          pools + price-level book-keeping
//   state_matching.cpp       matching + stop-trigger drain
//   state_commands.cpp       validation + submit/cancel/modify
//   state_queries.cpp        queries + invariant audit
#include "core/state.hpp"

#include <memory>

namespace lob {

namespace {

// RAII per-call sink override: a shared engine serves many commands, each
// with its own event collector; restores the default sink at scope end.
struct SinkScope {
    EventSink *&slot;
    EventSink *previous;
    explicit SinkScope(EventSink *&slot_, EventSink *override_sink)
        : slot(slot_), previous(slot_) {
        if (override_sink != nullptr) slot = override_sink;
    }
    ~SinkScope() { slot = previous; }
    SinkScope(const SinkScope &) = delete;
    SinkScope &operator=(const SinkScope &) = delete;
};

}  // namespace

OrderBook::OrderBook(EventSink *sink) : state_(new State(sink)) {}
OrderBook::~OrderBook() = default;

std::uint64_t OrderBook::submit(const SubmitRequest &request, EventSink *per_call_sink) {
    SinkScope scope{state_->sink, per_call_sink};
    return state_->submit(request);
}
bool OrderBook::cancel(std::uint64_t order_id, EventSink *per_call_sink) {
    SinkScope scope{state_->sink, per_call_sink};
    return state_->cancel(order_id);
}
bool OrderBook::modify(std::uint64_t order_id, const ModifyRequest &request,
                       EventSink *per_call_sink) {
    SinkScope scope{state_->sink, per_call_sink};
    return state_->modify(order_id, request);
}
void OrderBook::update_last_price(Price price, EventSink *per_call_sink) {
    SinkScope scope{state_->sink, per_call_sink};
    state_->update_last_price(price);
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
std::size_t OrderBook::level_size(Side side, Price price) const {
    return state_->level_size(side, price);
}
std::vector<Price> OrderBook::resting_prices(Side side) const {
    return state_->resting_prices(side);
}
std::size_t OrderBook::order_count() const { return state_->order_count(); }
std::string OrderBook::audit() const { return state_->audit(); }

}  // namespace lob
