// LOB engine microservice - stage 2: the real matching core is live.
//
// Responsibilities (evolves the stage-1 stub contract):
//   - discover the active run id (lob:active-run, TTL-refreshed by the
//     Python pipeline so a dead backend's marker expires)
//   - consume lob:{run}:commands via the engine consumer group and XACK
//     after processing (no pending-entry leak across restarts)
//   - own one OrderBook per symbol (price-time priority, AVL levels)
//   - journal engine events to lob:{run}:{symbol}:events with the exact
//     payload shapes app/storage/pipeline.py's pg-flusher consumes
//     (accepted / modified / cancelled / stop-triggered / trade)
//   - reply {corr_id, status, events} to lob:{run}:reply:{corr_id} (TTL'd)
//   - reclaim entries a dead engine left pending (XAUTOCLAIM); corr-id dedup
//     guarantees replays never double-execute an order
//
// Command format (JSON in the stream entry's `payload` field):
//   submit: {corr_id, action:"submit", symbol, agent_id,
//            type: market|limit|market-limit|stop|stop-limit, side: buy|sell,
//            quantity, limit_price?, stop_price?, client_order_ref?}
//   cancel: {corr_id, action:"cancel", symbol, agent_id, order_id}
//   modify: {corr_id, action:"modify", symbol, agent_id, order_id,
//            quantity? / limit_price? / stop_price?}     (at least one field)
//   price:  {corr_id, action:"price", symbol, price}     (market-data tick)
//
// Order ids are engine-assigned per run; the reply's accepted entry carries
// the assigned order_id alongside the client_order_ref for correlation.

#include <hiredis/hiredis.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/book.hpp"

using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

std::string iso_now() {
    const auto now = std::time(nullptr);
    char buffer[64];
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

struct ReplyDeleter {
    void operator()(redisReply *reply) const {
        if (reply != nullptr) {
            freeReplyObject(reply);
        }
    }
};
using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

ReplyPtr command(redisContext *context, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    redisReply *raw = static_cast<redisReply *>(redisvCommand(context, fmt, args));
    va_end(args);
    return ReplyPtr(raw);
}

// returns `r->elements` if `r` is an array, otherwise nullopt
std::optional<std::size_t> arr_length(const redisReply *reply) {
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) {
        return std::nullopt;
    }
    return reply->elements;
}

std::optional<std::string> as_string(const redisReply *reply) {
    if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
        return std::nullopt;
    }
    return std::string(reply->str, reply->len);
}

// looks up `field` in a stream-entry fields array [k, v, k, v, ...]
std::optional<std::string> field_value(const redisReply *fields_array, const char *field) {
    const auto count = arr_length(fields_array);
    if (!count) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i + 1 < *count; i += 2) {
        auto name = as_string(fields_array->element[i]);
        if (name && *name == field) {
            return as_string(fields_array->element[i + 1]);
        }
    }
    return std::nullopt;
}

long long ll_ticks(double price) {
    // PRICE_TICK = 0.01 (see backend/app/core/lob/contracts.py)
    return static_cast<long long>(std::llround(price * 100.0));
}

double from_ticks(double ticks) { return ticks * 0.01; }

// one per submitted order (mirrors the python engine's OrderRecord)
struct OrderRecord {
    std::string agent;
    std::string side;  // "buy"|"sell"
    std::string type;  // wire type string of the submission
    long long quantity = 0;      // original quantity
    long long remaining = 0;     // current working size
    long long limit_ticks = 0;   // 0 = none
    long long stop_ticks = 0;    // 0 = none
    std::string status;          // pending-trigger|active|triggered|cancelled|filled
    long long seq = 0;           // seq of the event that created/rekeyed it
};

// per-command context the engine's event callbacks ride on
struct Request {
    std::string symbol;
    std::string agent_id;
    std::string type;
    std::string side;
    std::string client_order_ref;
    long long quantity = 0;
    double limit_price = 0.0;
    double stop_price = 0.0;

    std::vector<json> events;      // reply = journal entries + reply-only rejections

    // a trade's journal entry waits for its two Filled events
    bool pending = false;
    std::uint64_t maker_id = 0;
    std::uint64_t taker_id = 0;
    double trade_price = 0.0;
    long long trade_qty = 0;
    bool maker_seen = false;
    bool taker_seen = false;
    std::uint64_t trade_seq = 0;
};

// generic per-command event sink: forwards callbacks through a plain
// function pointer, so it can be declared before the service class
class CallbackSink final : public lob::EventSink {
public:
    using Handler = void (*)(void *self, Request *req, const lob::Event &event);

    CallbackSink(Handler handler, void *self, Request *request)
        : handler_{handler}, self_{self}, request_{request} {}

    void on_event(const lob::Event &event) override {
        handler_(self_, request_, event);
    }

private:
    Handler handler_;
    void *self_;
    Request *request_;
};

class Service {
public:
    Service(redisContext *context, std::string run_id)
        : context_{context}, prefix_{"lob:" + run_id} {}

    // static trampoline for CallbackSink
    static void event_trampoline(void *self, Request *req, const lob::Event &e) {
        static_cast<Service *>(self)->on_engine_event(*req, e);
    }

    // false when corr_id was seen before (a replayed/reclaimed entry)
    bool mark_processed(const json &cmd) {
        const std::string corr = cmd.value("corr_id", std::string());
        if (corr.empty()) return true;  // nothing to dedup on; process anyway
        return seen_corr_.insert(corr).second;
    }

    void handle(const json &cmd) {
        Request req;
        req.agent_id = cmd.value("agent_id", std::string());
        req.type = cmd.value("type", std::string());
        req.side = cmd.value("side", std::string());
        req.client_order_ref = cmd.value("client_order_ref", std::string());
        req.quantity = cmd.value("quantity", 0LL);
        req.limit_price = cmd.value("limit_price", 0.0);
        req.stop_price = cmd.value("stop_price", 0.0);

        json reply{{"status", "ok"}, {"events", json::array()}};
        try {
            const std::string corr_id = cmd.value("corr_id", std::string());
            const std::string action = cmd.value("action", std::string());
            req.symbol = cmd.value("symbol", std::string());
            if (req.symbol.empty()) throw std::invalid_argument("missing symbol");
            if (req.symbol.size() > 16) throw std::invalid_argument("symbol too long");
            reply["corr_id"] = corr_id;

            if (action == "submit") {
                handle_submit(req);
            } else if (action == "cancel") {
                handle_cancel(req, cmd.value("order_id", 0ULL));
            } else if (action == "modify") {
                handle_modify(req, cmd);
            } else if (action == "price") {
                const double price = cmd.value("price", 0.0);
                if (!(price > 0.0)) throw std::invalid_argument("price must be positive");
                CallbackSink sink{&Service::event_trampoline, this, &req};
                book_for(req.symbol).update_last_price(price, &sink);
                // the engine's `last` moved, so the mirror follows (matches
                // the python engine's last_trade_price semantics)
                command(context_, "SET %s:%s:last %s", prefix_.c_str(), req.symbol.c_str(),
                        std::to_string(ll_ticks(price)).c_str());
            } else {
                throw std::invalid_argument("unknown action '" + action + "'");
            }
        } catch (const std::exception &error) {
            reply["status"] = "error";
            reply["detail"] = error.what();
        }
        for (const json &event : req.events) {
            reply["events"].push_back(event);
        }
        mirror_command(req);
        const std::string corr_id = cmd.value("corr_id", std::string());
        if (!corr_id.empty()) {
            save_reply(corr_id, reply);
        }
    }

    // -------------------------------------------------------------- mirror

    // Maintains the hot Redis layer (schema doc: redis_schema.md) so readers
    // never touch the matching engine directly. Dev-scale simple: full ladder
    // rebuild per command (python did the same), plus piped order hash /
    // agent index / trade tape updates driven by the journal events.
    void mirror_command(const Request &req) {
        command(context_, "SADD %s:symbols %s", prefix_.c_str(), req.symbol.c_str());
        for (const json &event : req.events) {
            if (!event.is_object() || !event.contains("affected") ||
                !event.at("affected").is_object()) {
                continue;  // reply-only "rejected" entries carry no mirror state
            }
            const std::string kind = event.value("event", std::string());
            if (kind == "trade") {
                mirror_trade(req, event);
            }
            for (const auto &item : event.at("affected").items()) {
                const json &state = item.value();
                if (!state.is_object()) {
                    continue;
                }
                mirror_order(req, std::stoull(item.key()), state);
            }
        }
        mirror_ladder(req);
    }

    void mirror_trade(const Request &req, const json &event) {
        const std::string tape = prefix_ + ":" + req.symbol + ":trades";
        command(context_,
                "XADD %s * seq %s buy_order_id %s sell_order_id %s buy_agent %s sell_agent %s "
                "price_ticks %s quantity %lld aggressor_side %s ts %s",
                tape.c_str(),
                std::to_string(event.value("seq", 0LL)).c_str(),
                std::to_string(event.value("buy_order_id", 0ULL)).c_str(),
                std::to_string(event.value("sell_order_id", 0ULL)).c_str(),
                event.value("buy_agent", std::string()).c_str(),
                event.value("sell_agent", std::string()).c_str(),
                std::to_string(ll_ticks(event.value("price", 0.0))).c_str(),
                event.value("quantity", 0LL),
                event.value("aggressor_side", std::string()).c_str(),
                iso_now().c_str());
        command(context_, "SET %s:%s:last %s", prefix_.c_str(), req.symbol.c_str(),
                std::to_string(ll_ticks(event.value("price", 0.0))).c_str());
    }

    // ticks may be null in the state -> "" , matching the python writer
    static std::string tick_json_string(const json &state, const char *key) {
        const json value = state.value(key, json(nullptr));
        return value.is_number_integer() ? std::to_string(value.get<long long>())
                                         : std::string();
    }

    void mirror_order(const Request &req, std::uint64_t order_id, const json &state) {
        const std::string status = state.value("status", std::string());
        const std::string agent = state.value("agent_id", std::string());
        const std::string member = req.symbol + ":" + std::to_string(order_id);
        const std::string order_hash =
            prefix_ + ":" + req.symbol + ":order:" + std::to_string(order_id);
        const std::string agent_key = prefix_ + ":agent:" + agent + ":orders";
        if (status == "cancelled" || status == "filled") {
            command(context_, "DEL %s", order_hash.c_str());
            command(context_, "ZREM %s %s", agent_key.c_str(), member.c_str());
            return;
        }
        const std::string limit_str = tick_json_string(state, "limit_ticks");
        const std::string stop_str = tick_json_string(state, "stop_ticks");
        const std::string quantity = std::to_string(state.value("quantity", 0LL));
        const std::string remaining = std::to_string(state.value("remaining", 0LL));
        const std::string side = state.value("side", std::string());
        const std::string type = state.value("type", std::string());
        const std::string seq = std::to_string(state.value("seq", 0LL));
        command(context_,
                "HSET %s agent %s side %s type %s quantity %s remaining %s limit_ticks %s "
                "stop_ticks %s status %s seq %s",
                order_hash.c_str(), agent.c_str(), side.c_str(), type.c_str(), quantity.c_str(),
                remaining.c_str(), limit_str.c_str(), stop_str.c_str(), status.c_str(),
                seq.c_str());
        command(context_, "ZADD %s %s %s", agent_key.c_str(), seq.c_str(), member.c_str());
    }

    void mirror_ladder(const Request &req) {
        lob::OrderBook &book = book_for(req.symbol);
        const auto rebuild = [&](lob::Side side, const char *ladder_name) {
            const std::string ladder_key = prefix_ + ":" + req.symbol + ":" + ladder_name;
            command(context_, "DEL %s", ladder_key.c_str());
            for (const double price : book.resting_prices(side)) {
                const std::string ticks = std::to_string(ll_ticks(price));
                command(context_, "ZADD %s %s %s", ladder_key.c_str(), ticks.c_str(),
                        ticks.c_str());
                command(context_, "HSET %s:%s:level:%s:%s size %lld volume %lld",
                        prefix_.c_str(), req.symbol.c_str(),
                        side == lob::Side::Buy ? "buy" : "sell", ticks.c_str(),
                        static_cast<long long>(book.level_size(side, price)),
                        static_cast<long long>(book.volume_at(side, price)));
            }
        };
        rebuild(lob::Side::Buy, "bids");
        rebuild(lob::Side::Sell, "asks");
    }

    // ---------------------------------------------------------------- sink API

    // called by RequestSink for every engine event of the running command
    void on_engine_event(Request &req, const lob::Event &e) {
        switch (e.kind) {
            case lob::EventKind::Accepted: {
                OrderRecord rec;
                rec.agent = req.agent_id;
                rec.side = req.side;
                rec.type = req.type;
                rec.quantity = req.quantity;
                rec.remaining = req.quantity;
                rec.limit_ticks = req.limit_price > 0.0 ? ll_ticks(req.limit_price) : 0;
                rec.stop_ticks = req.stop_price > 0.0 ? ll_ticks(req.stop_price) : 0;
                rec.status = (req.type == "stop" || req.type == "stop-limit")
                                 ? std::string("pending-trigger")
                                 : std::string("active");
                rec.seq = next_seq();
                records_.emplace(e.order_id, rec);

                json affected;
                affected[std::to_string(e.order_id)] = state_of(rec);
                json payload{
                    {"event", "accepted"},
                    {"seq", rec.seq},
                    {"order", order_json(rec, e.order_id)},
                    {"client_order_ref", req.client_order_ref},
                    {"affected", affected},
                };
                journal(req, "accepted", std::move(payload));
                break;
            }
            case lob::EventKind::Trade: {
                req.pending = true;
                req.maker_id = e.order_id;
                req.taker_id = e.taker_order_id;
                req.trade_price = e.price;
                req.trade_qty = e.quantity;
                req.maker_seen = false;
                req.taker_seen = false;
                req.trade_seq = next_seq();
                break;
            }
            case lob::EventKind::Filled: {
                if (auto *rec = record(e.order_id); rec != nullptr) {
                    rec->remaining = e.remaining;
                    rec->status = e.remaining > 0 ? std::string("active") : std::string("filled");
                }
                if (req.pending && e.order_id == req.maker_id) req.maker_seen = true;
                if (req.pending && e.order_id == req.taker_id) req.taker_seen = true;
                if (req.pending && req.maker_seen && req.taker_seen) {
                    emit_trade(req);
                    req.pending = false;
                }
                break;
            }
            case lob::EventKind::Cancelled: {
                json affected;
                if (auto *rec = record(e.order_id); rec != nullptr) {
                    rec->status = "cancelled";
                    affected[std::to_string(e.order_id)] = state_of(*rec);
                } else {
                    affected[std::to_string(e.order_id)] = json(nullptr);
                }
                json payload{
                    {"event", "cancelled"},
                    {"seq", next_seq()},
                    {"order_id", e.order_id},
                    {"affected", affected},
                };
                journal(req, "cancelled", std::move(payload));
                break;
            }
            case lob::EventKind::Modified: {
                if (auto *rec = record(e.order_id); rec != nullptr) {
                    rec->quantity = e.quantity;
                    rec->remaining = e.quantity;  // working size becomes the new size
                    if (e.price > 0.0) rec->limit_ticks = ll_ticks(e.price);
                    rec->seq = next_seq();
                    json affected;
                    affected[std::to_string(e.order_id)] = state_of(*rec);
                    json payload{
                        {"event", "modified"},
                        {"seq", rec->seq},
                        {"order", order_json(*rec, e.order_id)},
                        {"affected", affected},
                    };
                    journal(req, "modified", std::move(payload));
                }
                break;
            }
            case lob::EventKind::Triggered: {
                if (auto *rec = record(e.order_id); rec != nullptr) {
                    rec->status = "triggered";
                    json converted = order_json(*rec, e.order_id);
                    converted["type"] = rec->type == "stop" ? json("market") : json("market-limit");
                    json affected;
                    affected[std::to_string(e.order_id)] = state_of(*rec);
                    json payload{
                        {"event", "stop-triggered"},
                        {"seq", next_seq()},
                        {"order_id", e.order_id},
                        {"converted", converted},
                        {"affected", affected},
                    };
                    journal(req, "stop-triggered", std::move(payload));
                }
                break;
            }
            case lob::EventKind::Rejected: {
                reject(req, e.order_id, e.reason != nullptr ? e.reason : "unknown reason");
                break;
            }
        }
    }

private:
    json state_of(const OrderRecord &r) {
        return json{
            {"agent_id", r.agent},
            {"side", r.side},
            {"type", r.type},
            {"quantity", r.quantity},
            {"remaining", r.remaining},
            {"limit_ticks", r.limit_ticks > 0 ? json(r.limit_ticks) : json(nullptr)},
            {"stop_ticks", r.stop_ticks > 0 ? json(r.stop_ticks) : json(nullptr)},
            {"status", r.status},
            {"seq", r.seq},
        };
    }

    json order_json(const OrderRecord &r, std::uint64_t order_id) const {
        return json{
            {"order_id", order_id},
            {"agent_id", r.agent},
            {"side", r.side},
            {"type", r.type},
            {"quantity", r.quantity},
            {"limit_price",
             r.limit_ticks > 0 ? json(from_ticks(r.limit_ticks)) : json(nullptr)},
            {"stop_price",
             r.stop_ticks > 0 ? json(from_ticks(r.stop_ticks)) : json(nullptr)},
        };
    }

    OrderRecord *record(std::uint64_t order_id) {
        auto it = records_.find(order_id);
        return it != records_.end() ? &it->second : nullptr;
    }

    // publishes to the journal stream and collects for the reply
    void journal(Request &req, const char *event_type, json payload) {
        publish_event(req.symbol, payload.at("seq"), event_type, payload.dump());
        req.events.push_back(std::move(payload));
    }

    void reject(Request &req, std::uint64_t order_id, const std::string &reason) {
        // Rejected events are transport-level: reply only, never journaled
        // (mirrors pipeline._build_payload skipping Rejected).
        req.events.push_back(json{
            {"event", "rejected"},
            {"order_id", order_id > 0 ? json(order_id) : json(nullptr)},
            {"reason", reason},
        });
    }

    void publish_event(const std::string &symbol, std::uint64_t seq, const std::string &type,
                       const std::string &payload) {
        const std::string event_key = prefix_ + ":" + symbol + ":events";
        command(context_, "XADD %s * seq %llu type %s ts %s payload %s", event_key.c_str(),
                static_cast<unsigned long long>(seq), type.c_str(), iso_now().c_str(),
                payload.c_str());
    }

    void save_reply(const std::string &corr_id, const json &reply) {
        command(context_, "SET %s %s EX 8", (prefix_ + ":reply:" + corr_id).c_str(),
                reply.dump().c_str());
    }

    // -------------------------------------------------------------- actions

    void handle_submit(Request &req) {
        const lob::SubmitRequest request = build_submit(req);
        CallbackSink sink{&Service::event_trampoline, this, &req};
        book_for(req.symbol).submit(request, &sink);
    }

    static lob::SubmitRequest build_submit(const Request &req) {
        lob::SubmitRequest request;
        if (req.side == "buy") request.side = lob::Side::Buy;
        else if (req.side == "sell") request.side = lob::Side::Sell;
        else throw std::invalid_argument("unknown side '" + req.side + "'");
        if (req.type == "market") {
            request.type = lob::Type::Market;
        } else if (req.type == "limit") {
            request.limit_price = req.limit_price;
            request.type = lob::Type::Limit;
        } else if (req.type == "market-limit") {
            request.limit_price = req.limit_price;
            request.type = lob::Type::MarketableLimit;
        } else if (req.type == "stop") {
            request.stop_price = req.stop_price;
            request.type = lob::Type::Stop;
        } else if (req.type == "stop-limit") {
            request.limit_price = req.limit_price;
            request.stop_price = req.stop_price;
            request.type = lob::Type::StopLimit;
        } else {
            throw std::invalid_argument("unknown order type '" + req.type + "'");
        }
        request.quantity = req.quantity;
        return request;
    }

    void handle_cancel(Request &req, std::uint64_t order_id) {
        if (const OrderRecord *rec = record(order_id);
            rec != nullptr && (req.agent_id.empty() || rec->agent != req.agent_id)) {
            reject(req, order_id,
                   "order " + std::to_string(order_id) + " does not belong to agent " +
                       req.agent_id);
            return;
        }
        CallbackSink sink{&Service::event_trampoline, this, &req};
        book_for(req.symbol).cancel(order_id, &sink);
    }

    void handle_modify(Request &req, const json &cmd) {
        const auto order_id = cmd.value("order_id", 0ULL);
        if (const OrderRecord *rec = record(order_id);
            rec != nullptr && (req.agent_id.empty() || rec->agent != req.agent_id)) {
            reject(req, order_id,
                   "order " + std::to_string(order_id) + " does not belong to agent " +
                       req.agent_id);
            return;
        }
        if (!cmd.contains("quantity") && !cmd.contains("limit_price") &&
            !cmd.contains("stop_price")) {
            reject(req, order_id, "no fields passed to modify");
            return;
        }
        lob::ModifyRequest request;
        if (cmd.contains("quantity")) {
            request.has_new_quantity = true;
            request.new_quantity = cmd.value("quantity", 0LL);
        }
        if (cmd.contains("limit_price")) {
            request.has_new_price = true;
            request.new_price = cmd.value("limit_price", 0.0);
        }
        if (cmd.contains("stop_price")) {
            request.has_new_stop_price = true;
            request.new_stop_price = cmd.value("stop_price", 0.0);
        }
        CallbackSink sink{&Service::event_trampoline, this, &req};
        book_for(req.symbol).modify(order_id, request, &sink);
    }

    // -------------------------------------------------------------- storage

    void emit_trade(Request &req) {
        const OrderRecord *maker_rec = record(req.maker_id);
        const OrderRecord *taker_rec = record(req.taker_id);
        if (maker_rec == nullptr || taker_rec == nullptr) {
            req.pending = false;
            return;  // defensive: journals reference tracked orders only
        }
        const bool taker_is_buy = (taker_rec->side == "buy");
        json affected;
        affected[std::to_string(req.maker_id)] = state_of(*maker_rec);
        affected[std::to_string(req.taker_id)] = state_of(*taker_rec);
        json payload{
            {"event", "trade"},
            {"seq", req.trade_seq},
            {"buy_order_id", taker_is_buy ? req.taker_id : req.maker_id},
            {"sell_order_id", taker_is_buy ? req.maker_id : req.taker_id},
            {"buy_agent", taker_is_buy ? taker_rec->agent : maker_rec->agent},
            {"sell_agent", taker_is_buy ? maker_rec->agent : taker_rec->agent},
            {"price", req.trade_price},
            {"quantity", req.trade_qty},
            {"aggressor_side", taker_is_buy ? "buy" : "sell"},
            {"affected", affected},
        };
        journal(req, "trade", std::move(payload));
    }

    // -------------------------------------------------------------- storage

    redisContext *context_;
    std::string prefix_;
    std::unordered_map<std::string, std::unique_ptr<lob::OrderBook>> books_;
    std::unordered_map<std::uint64_t, OrderRecord> records_;
    std::unordered_set<std::string> seen_corr_;
    std::uint64_t seq_ = 0;

    std::uint64_t next_seq() { return ++seq_; }

    lob::OrderBook &book_for(const std::string &symbol) {
        auto it = books_.find(symbol);
        if (it != books_.end()) {
            return *it->second;
        }
        auto book = std::make_unique<lob::OrderBook>();
        auto *raw = book.get();
        books_.emplace(symbol, std::move(book));
        return *raw;
    }
};
// end of Service

// handles once-claimed stuck PEL entries (dead-engine leftovers); counts
// claimed entries (acked entries included)
std::size_t claim_stuck(redisContext *context, const std::string &stream, Service &service,
                        const redisReply *response) {
    std::size_t counted = 0;
    // XAUTOCLAIM response: [next_cursor, [[id, [field, value, ...]], ...], [deleted...]]
    const auto outer = arr_length(response);
    if (!outer || *outer < 2) {
        return 0;
    }
    const redisReply *entries = response->element[1];
    const auto entry_count = arr_length(entries);
    if (!entry_count) {
        return 0;
    }
    for (std::size_t i = 0; i < *entry_count; ++i) {
        const redisReply *entry = entries->element[i];
        if (auto fields = arr_length(entry); fields && *fields == 2) {
            auto payload = field_value(entry->element[1], "payload");
            try {
                if (payload) {
                    const json cmd = json::parse(*payload);
                    if (service.mark_processed(cmd)) {
                        std::cout << "[lob-engine] reclaim: " << *payload << "\n";
                        service.handle(cmd);
                    }
                }
                if (auto id = as_string(entry->element[0])) {
                    command(context, "XACK %s engine %s", stream.c_str(), id->c_str());
                }
                ++counted;
            } catch (const std::exception &error) {
                std::cerr << "[lob-engine] unreclaimable: " << error.what() << "\n";
            }
        }
    }
    return counted;
}

void ensure_group(redisContext *context, const std::string &stream) {
    auto reply = command(context, "XGROUP CREATE %s engine $ MKSTREAM", stream.c_str());
    if (reply == nullptr) {
        throw std::runtime_error("XGROUP CREATE failed");
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        const std::string message(reply->str, reply->len);
        if (message.find("BUSYGROUP") == std::string::npos) {
            throw std::runtime_error("XGROUP CREATE: " + message);
        }
    }
}

// drains one XREADGROUP response; returns handled command count
std::size_t process_response(redisContext *context, Service &service, const std::string &stream,
                             const redisReply *response) {
    std::size_t counted = 0;
    // response: [ [stream_name, [ [id, [field, value, ...]], ...] ], ... ]
    const auto streams = arr_length(response);
    if (!streams) {
        return 0;
    }
    for (std::size_t s = 0; s < *streams; ++s) {
        const redisReply *stream_entry = response->element[s];
        const auto pair = arr_length(stream_entry);
        if (!pair || *pair != 2) {
            continue;
        }
        const redisReply *entries = stream_entry->element[1];
        const auto entry_count = arr_length(entries);
        if (!entry_count) {
            continue;
        }
        for (std::size_t i = 0; i < *entry_count; ++i) {
            const redisReply *entry = entries->element[i];
            if (auto fields = arr_length(entry); fields && *fields == 2) {
                auto payload = field_value(entry->element[1], "payload");
                if (!payload) {
                    continue;
                }
                try {
                    const json cmd = json::parse(*payload);
                    if (service.mark_processed(cmd)) {
                        service.handle(cmd);
                    }
                    if (auto id = as_string(entry->element[0])) {
                        command(context, "XACK %s engine %s", stream.c_str(), id->c_str());
                    }
                    ++counted;
                } catch (const std::exception &error) {
                    std::cerr << "[lob-engine] rejected unparseable command: " << error.what()
                              << "\n";
                }
            }
        }
    }
    return counted;
}

}  // namespace

int main() {
    std::cout << std::unitbuf;  // unbuffered: container logs stream in realtime
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const char *host = std::getenv("REDIS_HOST");
    const char *port = std::getenv("REDIS_PORT");
    redisContext *context = redisConnect(host ? host : "redis", port ? std::atoi(port) : 6379);
    if (context == nullptr || context->err) {
        std::cerr << "[lob-engine] redis connect failed\n";
        return 1;
    }

    std::cout << "[lob-engine] connected to redis, waiting for an active run\n";

    std::optional<Service> service;
    std::string run_id;

    while (!g_stop) {
        const auto marker = as_string(command(context, "GET lob:active-run").get());
        if (!marker) {
            service.reset();
            run_id.clear();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        const std::string stream = "lob:" + *marker + ":commands";
        if (!service || *marker != run_id) {
            run_id = *marker;
            ensure_group(context, stream);
            service.emplace(context, run_id);
            std::cout << "[lob-engine] attached to run " << run_id << "\n";
            // take over entries a dead engine left pending
            auto claimed =
                command(context, "XAUTOCLAIM %s engine lob-engine 15000 0-0 COUNT 100",
                        stream.c_str());
            if (claimed != nullptr && claimed->type == REDIS_REPLY_ARRAY) {
                const auto reclaimed =
                    claim_stuck(context, stream, *service, claimed.get());
                if (reclaimed > 0) {
                    std::cout << "[lob-engine] reclaimed " << reclaimed
                              << " stuck entries\n";
                }
            }
        }

        auto response =
            command(context, "XREADGROUP GROUP engine lob-engine BLOCK 1000 COUNT 16 STREAMS %s >",
                    stream.c_str());
        if (response != nullptr && response->type == REDIS_REPLY_ARRAY) {
            const auto handled = process_response(context, *service, stream, response.get());
            if (handled > 0) {
                std::cout << "[lob-engine] handled " << handled << " commands\n";
            }
        } else if (response != nullptr && response->type == REDIS_REPLY_ERROR) {
            std::cerr << "[lob-engine] XREADGROUP error: "
                      << std::string(response->str, response->len) << "\n";
        }
    }

    redisFree(context);
    std::cout << "[lob-engine] stopped\n";
    return 0;
}
