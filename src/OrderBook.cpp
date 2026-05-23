#include "OrderBook.h"
#include <cstring>

namespace matching_engine {

OrderBook::OrderBook() {
    std::memset(bids_, 0, sizeof(bids_));
    std::memset(asks_, 0, sizeof(asks_));
}

bool OrderBook::add_order(Order* order) {
    if (!order || !order->is_live()) return false;

    int64_t price = order->price;
    if (price < MIN_PRICE || price > MAX_PRICE) return false;

    if (order->is_buy()) {
        list_push_back(bids_[price - MIN_PRICE], order);
    } else {
        list_push_back(asks_[price - MIN_PRICE], order);
    }

    order_index_[order->order_id] = order;
    return true;
}

bool OrderBook::cancel_order(uint64_t order_id) {
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;

    Order* order = it->second;
    if (!order->is_live()) return false;

    order->status = OrderStatus::Cancelled;

    int64_t price = order->price;
    if (order->is_buy()) {
        list_unlink(bids_[price - MIN_PRICE], order);
    } else {
        list_unlink(asks_[price - MIN_PRICE], order);
    }

    order_index_.erase(it);
    return true;
}

Order* OrderBook::get_order(uint64_t order_id) {
    auto it = order_index_.find(order_id);
    return (it != order_index_.end()) ? it->second : nullptr;
}

int64_t OrderBook::best_bid() const {
    for (int64_t p = MAX_PRICE; p >= MIN_PRICE; --p) {
        if (!bids_[p - MIN_PRICE].empty()) return p;
    }
    return 0;
}

int64_t OrderBook::best_ask() const {
    for (int64_t p = MIN_PRICE; p <= MAX_PRICE; ++p) {
        if (!asks_[p - MIN_PRICE].empty()) return p;
    }
    return 0;
}

size_t OrderBook::bid_size() const {
    size_t total = 0;
    for (size_t i = 0; i < NUM_LEVELS; ++i) {
        total += bids_[i].count;
    }
    return total;
}

size_t OrderBook::ask_size() const {
    size_t total = 0;
    for (size_t i = 0; i < NUM_LEVELS; ++i) {
        total += asks_[i].count;
    }
    return total;
}

PriceLevel* OrderBook::bid_level(int64_t price) {
    if (price < MIN_PRICE || price > MAX_PRICE) return nullptr;
    return &bids_[price - MIN_PRICE];
}

PriceLevel* OrderBook::ask_level(int64_t price) {
    if (price < MIN_PRICE || price > MAX_PRICE) return nullptr;
    return &asks_[price - MIN_PRICE];
}

void OrderBook::clear() {
    std::memset(bids_, 0, sizeof(bids_));
    std::memset(asks_, 0, sizeof(asks_));
    order_index_.clear();
}

}
