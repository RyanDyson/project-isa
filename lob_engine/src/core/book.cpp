// Public facade of the LOB matching core. The internals live in:
//   level_tree.{hpp,cpp}      AVL tree of price levels
//   state.hpp                 engine state definition (data + inline hot paths)
//   state_pools.cpp           pools + price-level book-keeping
//   state_matching.cpp        matching + stop-trigger drain
//   state_commands.cpp        validation + submit/cancel/modify
//   state_queries.cpp         queries + invariant audit
#include "core/state.hpp"

#include <memory>

namespace lob {

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
