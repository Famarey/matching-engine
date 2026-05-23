#pragma once

#include <cstdint>
#include <unordered_map>
#include "Order.h"

namespace matching_engine {

struct PriceLevel {
    Order* head{nullptr};
    Order* tail{nullptr};
    size_t count{0};

    bool empty() const { return head == nullptr; }
};

class OrderBook {
public:
    static constexpr int64_t MIN_PRICE = 0;
    static constexpr int64_t MAX_PRICE = 100000;
    static constexpr size_t NUM_LEVELS = MAX_PRICE - MIN_PRICE + 1;

    OrderBook();
    ~OrderBook() = default;

    bool add_order(Order* order);
    bool cancel_order(uint64_t order_id);
    Order* get_order(uint64_t order_id);

    int64_t best_bid() const;
    int64_t best_ask() const;

    size_t bid_size() const;
    size_t ask_size() const;

    PriceLevel* bid_level(int64_t price);
    PriceLevel* ask_level(int64_t price);

    void clear();

    static inline void list_push_back(PriceLevel& level, Order* order) {
        order->next = nullptr;
        order->prev = level.tail;

        if (level.tail) {
            level.tail->next = order;
        } else {
            level.head = order;
        }
        level.tail = order;
        level.count++;
    }

    static inline Order* list_pop_front(PriceLevel& level) {
        if (!level.head) return nullptr;

        Order* front = level.head;
        level.head = front->next;

        if (level.head) {
            level.head->prev = nullptr;
        } else {
            level.tail = nullptr;
        }

        front->next = nullptr;
        front->prev = nullptr;
        level.count--;

        return front;
    }

    static inline void list_unlink(PriceLevel& level, Order* order) {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            level.head = order->next;
        }

        if (order->next) {
            order->next->prev = order->prev;
        } else {
            level.tail = order->prev;
        }

        order->next = nullptr;
        order->prev = nullptr;
        level.count--;
    }

private:
    PriceLevel bids_[NUM_LEVELS];
    PriceLevel asks_[NUM_LEVELS];

    std::unordered_map<uint64_t, Order*> order_index_;
};

}
