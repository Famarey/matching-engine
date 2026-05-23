#pragma once

#include <atomic>
#include <thread>
#include <string>
#include <cstdint>
#include "LockFreeQueue.h"
#include "Order.h"

namespace matching_engine {

struct alignas(64) NetworkStats {
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> orders_dispatched{0};
    std::atomic<uint64_t> parse_errors{0};
    alignas(64) std::atomic<uint64_t> queue_full_drops{0};
};

#pragma pack(push, 1)
struct WireOrder {
    uint64_t order_id;
    uint8_t  side;
    uint8_t  type;
    int64_t  price;
    uint32_t quantity;
    uint32_t padding;
    uint64_t timestamp_ns;
};
#pragma pack(pop)

static_assert(sizeof(WireOrder) == 34, "WireOrder must be 34 bytes for wire compatibility");

class UDPServer {
public:
    UDPServer(OrderQueue& queue, uint16_t port = 50000);
    ~UDPServer();

    bool start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    const NetworkStats& get_stats() const { return stats_; }

    uint16_t get_port() const { return port_; }

private:
    void recv_loop();
    Order parse_wire_order(const WireOrder& wire);

    OrderQueue& queue_;
    uint16_t port_;

    int sockfd_;
    std::thread recv_thread_;
    std::atomic<bool> running_{false};

    NetworkStats stats_;

    static constexpr size_t RECV_BUF_SIZE = 65536;
    static constexpr int MAX_UDP_PAYLOAD = 65507;
};

}
