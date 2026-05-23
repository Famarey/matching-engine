#include "MatchingEngine.h"
#include <iostream>
#include <xmmintrin.h>

namespace matching_engine {

MatchingEngine::MatchingEngine() : slab_(100000) {
    trades_.reserve(100000);
}

MatchingEngine::~MatchingEngine() {
    stop();
}

bool MatchingEngine::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    return true;
}

void MatchingEngine::stop() {
    running_.store(false, std::memory_order_release);
}

void MatchingEngine::process_order(const Order& order) {
    auto recv_time = std::chrono::high_resolution_clock::now()
                         .time_since_epoch()
                         .count();

    stats_.total_orders.fetch_add(1, std::memory_order_relaxed);

    if (order.is_cancel()) {
        handle_cancel(order);
        return;
    }

    Order* new_order = slab_.allocate();
    if (!new_order) {
        stats_.total_rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    *new_order = order;

    switch (order.type) {
        case OrderType::Limit:
            if (order.is_buy()) {
                match_limit_buy(new_order, recv_time);
            } else {
                match_limit_sell(new_order, recv_time);
            }
            break;
        case OrderType::Market:
            if (order.is_buy()) {
                match_market_buy(new_order, recv_time);
            } else {
                match_market_sell(new_order, recv_time);
            }
            break;
        default:
            stats_.total_rejected.fetch_add(1, std::memory_order_relaxed);
            slab_.deallocate(new_order);
            break;
    }
}

void MatchingEngine::match_limit_buy(Order* order, uint64_t recv_time) {
    while (order->is_live()) {
        int64_t ask_price = order_book_.best_ask();
        if (ask_price == 0 || ask_price > order->price) break;

        PriceLevel* level = order_book_.ask_level(ask_price);

        while (order->is_live() && !level->empty()) {
            Order* maker = OrderBook::list_pop_front(*level);
            
            if (!maker->is_live()) continue;

            uint32_t fill_qty = std::min(order->remaining_qty, maker->remaining_qty);
            int64_t fill_price = maker->price;

            order->fill(fill_qty);
            maker->fill(fill_qty);

            auto complete_time = std::chrono::high_resolution_clock::now()
                                     .time_since_epoch()
                                     .count();
            uint64_t latency = static_cast<uint64_t>(complete_time - recv_time);

            record_trade(Trade(order->order_id, maker->order_id, 
                              fill_price, fill_qty, latency));
            update_latency(latency);

            if (!maker->is_live()) {
                slab_.deallocate(maker);
            }

            if (!order->is_live()) break;
        }

        if (!order->is_live()) break;
    }

    if (order->is_live()) {
        order_book_.add_order(order);
    } else {
        slab_.deallocate(order);
    }
}

void MatchingEngine::match_limit_sell(Order* order, uint64_t recv_time) {
    while (order->is_live()) {
        int64_t bid_price = order_book_.best_bid();
        if (bid_price == 0 || bid_price < order->price) break;

        PriceLevel* level = order_book_.bid_level(bid_price);

        while (order->is_live() && !level->empty()) {
            Order* maker = OrderBook::list_pop_front(*level);
            
            if (!maker->is_live()) continue;

            uint32_t fill_qty = std::min(order->remaining_qty, maker->remaining_qty);
            int64_t fill_price = maker->price;

            order->fill(fill_qty);
            maker->fill(fill_qty);

            auto complete_time = std::chrono::high_resolution_clock::now()
                                     .time_since_epoch()
                                     .count();
            uint64_t latency = static_cast<uint64_t>(complete_time - recv_time);

            record_trade(Trade(maker->order_id, order->order_id,
                              fill_price, fill_qty, latency));
            update_latency(latency);

            if (!maker->is_live()) {
                slab_.deallocate(maker);
            }

            if (!order->is_live()) break;
        }

        if (!order->is_live()) break;
    }

    if (order->is_live()) {
        order_book_.add_order(order);
    } else {
        slab_.deallocate(order);
    }
}

void MatchingEngine::match_market_buy(Order* order, uint64_t recv_time) {
    while (order->is_live()) {
        int64_t ask_price = order_book_.best_ask();
        if (ask_price == 0) break;

        PriceLevel* level = order_book_.ask_level(ask_price);

        while (order->is_live() && !level->empty()) {
            Order* maker = OrderBook::list_pop_front(*level);
            
            if (!maker->is_live()) continue;

            uint32_t fill_qty = std::min(order->remaining_qty, maker->remaining_qty);
            int64_t fill_price = maker->price;

            order->fill(fill_qty);
            maker->fill(fill_qty);

            auto complete_time = std::chrono::high_resolution_clock::now()
                                     .time_since_epoch()
                                     .count();
            uint64_t latency = static_cast<uint64_t>(complete_time - recv_time);

            record_trade(Trade(order->order_id, maker->order_id,
                              fill_price, fill_qty, latency));
            update_latency(latency);

            if (!maker->is_live()) {
                slab_.deallocate(maker);
            }

            if (!order->is_live()) break;
        }

        if (!order->is_live()) break;
    }

    if (order->remaining_qty > 0) {
        stats_.total_rejected.fetch_add(1, std::memory_order_relaxed);
    }

    slab_.deallocate(order);
}

void MatchingEngine::match_market_sell(Order* order, uint64_t recv_time) {
    while (order->is_live()) {
        int64_t bid_price = order_book_.best_bid();
        if (bid_price == 0) break;

        PriceLevel* level = order_book_.bid_level(bid_price);

        while (order->is_live() && !level->empty()) {
            Order* maker = OrderBook::list_pop_front(*level);
            
            if (!maker->is_live()) continue;

            uint32_t fill_qty = std::min(order->remaining_qty, maker->remaining_qty);
            int64_t fill_price = maker->price;

            order->fill(fill_qty);
            maker->fill(fill_qty);

            auto complete_time = std::chrono::high_resolution_clock::now()
                                     .time_since_epoch()
                                     .count();
            uint64_t latency = static_cast<uint64_t>(complete_time - recv_time);

            record_trade(Trade(maker->order_id, order->order_id,
                              fill_price, fill_qty, latency));
            update_latency(latency);

            if (!maker->is_live()) {
                slab_.deallocate(maker);
            }

            if (!order->is_live()) break;
        }

        if (!order->is_live()) break;
    }

    if (order->remaining_qty > 0) {
        stats_.total_rejected.fetch_add(1, std::memory_order_relaxed);
    }

    slab_.deallocate(order);
}

void MatchingEngine::handle_cancel(const Order& order) {
    bool success = order_book_.cancel_order(order.order_id);
    
    if (success) {
        stats_.total_cancelled.fetch_add(1, std::memory_order_relaxed);
    } else {
        stats_.total_rejected.fetch_add(1, std::memory_order_relaxed);
    }
}

void MatchingEngine::record_trade(const Trade& trade) {
    trades_.push_back(trade);
    stats_.total_trades.fetch_add(1, std::memory_order_relaxed);
}

void MatchingEngine::update_latency(uint64_t latency_ns) {
    stats_.avg_latency_ns.store(
        (stats_.avg_latency_ns.load(std::memory_order_relaxed) * 
         (stats_.total_trades.load(std::memory_order_relaxed) - 1) + latency_ns) /
         stats_.total_trades.load(std::memory_order_relaxed),
        std::memory_order_relaxed
    );

    uint64_t current_max = stats_.max_latency_ns.load(std::memory_order_relaxed);
    while (latency_ns > current_max) {
        if (stats_.max_latency_ns.compare_exchange_weak(
                current_max, latency_ns,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            break;
        }
    }

    uint64_t current_min = stats_.min_latency_ns.load(std::memory_order_relaxed);
    while (latency_ns < current_min) {
        if (stats_.min_latency_ns.compare_exchange_weak(
                current_min, latency_ns,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            break;
        }
    }
}

void MatchingEngine::run_consumer_loop() {
    Order order;
    
    while (running_.load(std::memory_order_acquire) || !input_queue_.empty()) {
        if (input_queue_.pop(order)) {
            process_order(order);
        } else {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            _mm_pause();
        }
    }
}

void MatchingEngine::print_stats() const {
    std::cout << "\n========== MATCHING ENGINE STATS ==========" << std::endl;
    std::cout << "Total Orders Processed: " << stats_.total_orders.load() << std::endl;
    std::cout << "Total Trades Executed:  " << stats_.total_trades.load() << std::endl;
    std::cout << "Total Cancelled:        " << stats_.total_cancelled.load() << std::endl;
    std::cout << "Total Rejected:         " << stats_.total_rejected.load() << std::endl;
    std::cout << "Avg Latency:            " << stats_.avg_latency_ns.load() << " ns" << std::endl;
    std::cout << "Min Latency:            " << stats_.min_latency_ns.load() << " ns" << std::endl;
    std::cout << "Max Latency:            " << stats_.max_latency_ns.load() << " ns" << std::endl;
    std::cout << "Best Bid:               " << order_book_.best_bid() << std::endl;
    std::cout << "Best Ask:               " << order_book_.best_ask() << std::endl;
    std::cout << "Bid Depth:              " << order_book_.bid_size() << std::endl;
    std::cout << "Ask Depth:              " << order_book_.ask_size() << std::endl;
    std::cout << "Slab Allocated:         " << slab_.allocated_count() << std::endl;
    std::cout << "Slab Available:         " << slab_.available_count() << std::endl;
    std::cout << "============================================\n" << std::endl;
}

}
