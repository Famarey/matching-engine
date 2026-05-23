#pragma once

#include <cstdint>

namespace matching_engine {

enum class Side : uint8_t {
    Buy = 0,
    Sell = 1
};

enum class OrderType : uint8_t {
    Limit = 0,
    Market = 1,
    Cancel = 2
};

enum class OrderStatus : uint8_t {
    New = 0,
    PartiallyFilled = 1,
    FullyFilled = 2,
    Cancelled = 3
};

struct alignas(64) Order {
    uint64_t order_id;
    Side side;
    OrderType type;
    int64_t price;
    uint32_t quantity;
    uint32_t remaining_qty;
    uint32_t filled_qty;
    uint64_t timestamp_ns;
    OrderStatus status;

    Order* next{nullptr};
    Order* prev{nullptr};

    Order() : order_id(0), side(Side::Buy), type(OrderType::Limit),
              price(0), quantity(0), remaining_qty(0), filled_qty(0),
              timestamp_ns(0), status(OrderStatus::New) {}

    Order(uint64_t id, Side s, OrderType t, int64_t p, uint32_t qty, uint64_t ts)
        : order_id(id), side(s), type(t), price(p), quantity(qty),
          remaining_qty(qty), filled_qty(0), timestamp_ns(ts),
          status(OrderStatus::New) {}

    bool is_buy() const { return side == Side::Buy; }
    bool is_sell() const { return side == Side::Sell; }
    bool is_limit() const { return type == OrderType::Limit; }
    bool is_market() const { return type == OrderType::Market; }
    bool is_cancel() const { return type == OrderType::Cancel; }

    bool is_live() const {
        return status == OrderStatus::New || status == OrderStatus::PartiallyFilled;
    }

    void fill(uint32_t qty) {
        if (qty >= remaining_qty) {
            filled_qty += remaining_qty;
            remaining_qty = 0;
            status = OrderStatus::FullyFilled;
        } else {
            filled_qty += qty;
            remaining_qty -= qty;
            status = OrderStatus::PartiallyFilled;
        }
    }
};

struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int64_t price;
    uint32_t quantity;
    uint64_t latency_ns;

    Trade() : buy_order_id(0), sell_order_id(0), price(0), quantity(0), latency_ns(0) {}

    Trade(uint64_t buy_id, uint64_t sell_id, int64_t p, uint32_t qty, uint64_t lat)
        : buy_order_id(buy_id), sell_order_id(sell_id), price(p),
          quantity(qty), latency_ns(lat) {}
};

}
