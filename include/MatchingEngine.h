#pragma once

#include <atomic>
#include <vector>
#include <chrono>
#include "Order.h"
#include "OrderBook.h"
#include "LockFreeQueue.h"
#include "SlabAllocator.h"

namespace matching_engine {

struct alignas(64) EngineStats {
    std::atomic<uint64_t> total_orders{0};
    std::atomic<uint64_t> total_trades{0};
    std::atomic<uint64_t> total_cancelled{0};
    std::atomic<uint64_t> total_rejected{0};
    alignas(64) std::atomic<double> avg_latency_ns{0.0};
    std::atomic<uint64_t> max_latency_ns{0};
    std::atomic<uint64_t> min_latency_ns{UINT64_MAX};
};

class MatchingEngine {
public:
    MatchingEngine();
    ~MatchingEngine();

    bool start();
    void stop();

    void process_order(const Order& order);
    
    OrderQueue& get_input_queue() { return input_queue_; }

    const std::vector<Trade>& get_trades() const { return trades_; }
    const EngineStats& get_stats() const { return stats_; }
    OrderBook& get_order_book() { return order_book_; }

    void run_consumer_loop();
    void print_stats() const;

private:
    void match_limit_buy(Order* order, uint64_t recv_time);
    void match_limit_sell(Order* order, uint64_t recv_time);
    void match_market_buy(Order* order, uint64_t recv_time);
    void match_market_sell(Order* order, uint64_t recv_time);
    void handle_cancel(const Order& order);

    void record_trade(const Trade& trade);
    void update_latency(uint64_t latency_ns);

    OrderBook order_book_;
    SlabAllocator slab_;
    OrderQueue input_queue_;
    
    std::vector<Trade> trades_;
    EngineStats stats_;
    
    std::atomic<bool> running_{false};

    static constexpr int64_t MAX_PRICE = 999999999;
    static constexpr int64_t MIN_PRICE = 1;
};

}
