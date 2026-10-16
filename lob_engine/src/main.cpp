// LOB engine microservice - stage 1: service wiring only (no matching yet).
//
// Responsibilities verified at this stage:
//   - discover the active run id (lob:active-run, TTL-refreshed by the
//     Python pipeline so a dead backend's marker expires)
//   - consume the run's command stream via a consumer group
//   - reply to each command at lob:{run}:reply:{corr_id} (TTL'd)
//   - publish stub events into lob:{run}:{symbol}:events with the same
//     field layout the Python pipeline journals
//
// Matching core arrives in a later stage; every submission is acked with a
// stub "accepted" event.

#include <hiredis/hiredis.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

std::string iso_now() {
    auto now = std::time(nullptr);
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

class StubEngine {
public:
    StubEngine(redisContext *context, std::string run_id)
        : context_{context}, run_id_{std::move(run_id)}, prefix_{"lob:" + run_id_} {}

    void handle(const json &cmd) {
        const std::string corr_id = cmd.value("corr_id", "");
        if (corr_id.empty()) {
            return;  // no addressable reply channel
        }

        json reply{{"corr_id", corr_id}, {"status", "ok"}, {"events", json::array()}};
        try {
            const std::string action = cmd.value("action", "");
            const std::string symbol = cmd.value("symbol", "");
            if (symbol.empty()) throw std::invalid_argument("missing symbol");

            if (action == "submit") {
                reply["events"].push_back(stub_accepted(cmd, symbol));
            } else if (action == "cancel") {
                reply["events"].push_back(stub_lifecycle(cmd, symbol, "cancelled"));
            } else if (action == "modify") {
                reply["events"].push_back(stub_lifecycle(cmd, symbol, "modified"));
            } else if (action == "price") {
                reply["events"].push_back(stub_price_feed(cmd, symbol));
            } else {
                throw std::invalid_argument("unknown action '" + action + "'");
            }
        } catch (const std::exception &error) {
            reply["status"] = "error";
            reply["detail"] = error.what();
        }
        save_reply(corr_id, reply);
    }

private:
    std::uint64_t next_order_id() { return ++next_order_id_; }
    std::uint64_t next_seq() { return ++next_seq_; }

    json stub_accepted(const json &cmd, const std::string &symbol) {
        const auto seq = next_seq();
        const json event = {
            {"event", "accepted"},
            {"order_id", next_order_id()},
            {"sequence", seq},
            {"client_order_ref", cmd.value("client_order_ref", "")},
            {"agent_id", cmd.value("agent_id", "")},
            {"symbol", symbol},
            {"side", cmd.value("side", "")},
            {"type", cmd.value("type", "")},
            {"quantity", cmd.value("quantity", 0)},
        };
        publish_event(symbol, event);
        return event;
    }

    json stub_lifecycle(const json &cmd, const std::string &symbol, const char *name) {
        const auto seq = next_seq();
        const json event = {
            {"event", name},
            {"order_id", cmd.value("order_id", 0)},
            {"sequence", seq},
        };
        publish_event(symbol, event);
        return event;
    }

    json stub_price_feed(const json &cmd, const std::string &symbol) {
        const auto seq = next_seq();
        const json event = {
            {"event", "price-feed"},
            {"price", cmd.value("price", 0.0)},
            {"sequence", seq},
        };
        publish_event(symbol, event);
        return event;
    }

    void publish_event(const std::string &symbol, const json &event) {
        std::string event_key = prefix_ + ":" + symbol + ":events";
        command(context_,
                "XADD %s seq %llu type %s ts %s payload %s",
                event_key.c_str(),
                static_cast<unsigned long long>(event.at("sequence").get<unsigned long long>()),
                event.at("event").get<std::string>().c_str(),
                iso_now().c_str(),
                event.dump().c_str());
    }

    void save_reply(const std::string &corr_id, const json &reply) {
        command(context_,
                "SET %s %s EX 8",
                (prefix_ + ":reply:" + corr_id).c_str(),
                reply.dump().c_str());
    }

    redisContext *context_;
    std::string run_id_;
    std::string prefix_;
    std::uint64_t next_order_id_ = 0;
    std::uint64_t next_seq_ = 0;
};

// handles once-claimed stuck PEL entries (dead consumer leftovers)
std::size_t claim_stuck(const redisReply *response, StubEngine &engine) {
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
                    std::cerr << "[lob-engine] reclaimed: " << *payload << "\n";
                    engine.handle(json::parse(*payload));
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
        std::string message(reply->str, reply->len);
        if (message.find("BUSYGROUP") == std::string::npos) {
            throw std::runtime_error("XGROUP CREATE: " + message);
        }
    }
}

// drains one XREADGROUP response into handled commands; returns entry count
std::size_t process_response(const redisReply *response, StubEngine &engine) {
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
                    std::cerr << "[lob-engine] handling: " << *payload << "\n";
                    engine.handle(json::parse(*payload));
                } catch (const std::exception &error) {
                    std::cerr << "[lob-engine] unparseable command: " << error.what() << "\n";
                }
                ++counted;
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

    std::optional<StubEngine> engine;
    std::string run_id;

    while (!g_stop) {
        const auto marker = as_string(command(context, "GET lob:active-run").get());
        if (!marker) {
            engine.reset();
            run_id.clear();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        if (!engine || *marker != run_id) {
            run_id = *marker;
            ensure_group(context, "lob:" + run_id + ":commands");
            engine.emplace(context, run_id);
            std::cout << "[lob-engine] attached to run " << run_id << "\n";
            // take over entries a dead engine left pending
            auto claimed = command(context,
                                   "XAUTOCLAIM %s engine lob-stub 15000 0-0 COUNT 100",
                                   ("lob:" + run_id + ":commands").c_str());
            if (claimed != nullptr && claimed->type == REDIS_REPLY_ARRAY) {
                const auto reclaimed = claim_stuck(claimed.get(), *engine);
                if (reclaimed > 0) {
                    std::cout << "[lob-engine] reclaimed " << reclaimed << " stuck entries\n";
                }
            }
        }

        auto response = command(context,
                                "XREADGROUP GROUP engine lob-%s BLOCK 1000 COUNT 16 STREAMS lob:%s:commands >",
                                "stub",
                                run_id.c_str());
        if (response != nullptr && response->type == REDIS_REPLY_ARRAY) {
            const auto handled = process_response(response.get(), *engine);
            if (handled > 0) {
                std::cout << "[lob-engine] handled " << handled << " commands\n";
            }
        } else if (response != nullptr && response->type == REDIS_REPLY_ERROR) {
            std::cerr << "[lob-engine] XREADGROUP error: " << std::string(response->str, response->len) << "\n";
        }
    }

    redisFree(context);
    std::cout << "[lob-engine] stopped\n";
    return 0;
}
