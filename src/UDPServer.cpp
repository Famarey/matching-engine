#include "UDPServer.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <xmmintrin.h>

namespace matching_engine {

UDPServer::UDPServer(OrderQueue& queue, uint16_t port)
    : queue_(queue), port_(port), sockfd_(-1) {}

UDPServer::~UDPServer() {
    stop();
}

bool UDPServer::start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

    sockfd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sockfd_ < 0) {
        std::cerr << "[UDPServer] Failed to create socket: " << strerror(errno) << std::endl;
        running_.store(false, std::memory_order_release);
        return false;
    }

    int optval = 1;
    setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    int rcvbuf = RECV_BUF_SIZE * 4;
    setsockopt(sockfd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[UDPServer] Failed to bind on port " << port_
                  << ": " << strerror(errno) << std::endl;
        ::close(sockfd_);
        sockfd_ = -1;
        running_.store(false, std::memory_order_release);
        return false;
    }

    recv_thread_ = std::thread(&UDPServer::recv_loop, this);

    std::cout << "[UDPServer] Listening on UDP 0.0.0.0:" << port_
              << " (SO_RCVBUF=" << (rcvbuf / 1024) << "KB)" << std::endl;
    return true;
}

void UDPServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }

    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }

    std::cout << "[UDPServer] Stopped. Stats: packets="
              << stats_.packets_received.load()
              << " orders=" << stats_.orders_dispatched.load()
              << " errors=" << stats_.parse_errors.load()
              << " drops=" << stats_.queue_full_drops.load() << std::endl;
}

void UDPServer::recv_loop() {
    alignas(64) char buffer[MAX_UDP_PAYLOAD];

    while (running_.load(std::memory_order_acquire)) {
        ssize_t nbytes = recv(sockfd_, buffer, sizeof(buffer), 0);

        if (nbytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                _mm_pause();
                continue;
            }
            continue;
        }

        stats_.packets_received.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_received.fetch_add(static_cast<uint64_t>(nbytes), std::memory_order_relaxed);

        size_t order_count = nbytes / sizeof(WireOrder);
        if (order_count == 0 || nbytes % sizeof(WireOrder) != 0) {
            stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const WireOrder* wire_orders = reinterpret_cast<const WireOrder*>(buffer);
        for (size_t i = 0; i < order_count; ++i) {
            Order order = parse_wire_order(wire_orders[i]);

            if (!queue_.push(order)) {
                stats_.queue_full_drops.fetch_add(1, std::memory_order_relaxed);
            } else {
                stats_.orders_dispatched.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

Order UDPServer::parse_wire_order(const WireOrder& wire) {
    return Order(
        wire.order_id,
        static_cast<Side>(wire.side),
        static_cast<OrderType>(wire.type),
        wire.price,
        wire.quantity,
        wire.timestamp_ns
    );
}

}
