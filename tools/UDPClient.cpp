#include "UDPServer.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <random>
#include <cstring>

using namespace matching_engine;

int main(int argc, char* argv[]) {
    uint16_t port = 50000;
    uint64_t num_orders = 100000;
    int batch_size = 1;
    const char* ip = "127.0.0.1";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) num_orders = std::stoull(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) batch_size = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: UDPClient [-p PORT] [-n NUM_ORDERS] [-b BATCH_SIZE] [IP]\n"
                      << "  -p   UDP port (default: 50000)\n"
                      << "  -n   Number of orders (default: 100000)\n"
                      << "  -b   Orders per UDP packet (default: 1)\n";
            return 0;
        }
    }

    if (argc > 1 && argv[argc-1][0] != '-' && !isdigit(argv[argc-1][0])) {
        ip = argv[argc - 1];
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    int sndbuf = 1024 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, ip, &dest.sin_addr);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int64_t> price_dist(100, 200);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 500);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> type_dist(0, 10);

    std::vector<WireOrder> batch(batch_size);
    size_t total_sent = 0;

    std::cout << "[UDP Client] Sending " << num_orders << " orders to "
              << ip << ":" << port
              << " (batch=" << batch_size << "/pkt)" << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    for (uint64_t i = 1; i <= num_orders;) {
        int n_in_batch = std::min(batch_size, static_cast<int>(num_orders - i + 1));

        for (int b = 0; b < n_in_batch; ++b) {
            int type_roll = type_dist(rng);
            OrderType type = OrderType::Limit;
            if (type_roll >= 9) type = OrderType::Market;
            else if (type_roll == 8) type = OrderType::Cancel;

            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            int64_t price = price_dist(rng);
            uint32_t qty = qty_dist(rng);

            if (type == OrderType::Cancel && i > 50) {
                price = static_cast<int64_t>(rng() % i + 1);
                qty = 0;
            }

            auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();

            batch[b] = WireOrder{
                i + b,
                static_cast<uint8_t>(side),
                static_cast<uint8_t>(type),
                price,
                qty,
                0,
                static_cast<uint64_t>(ts)
            };
        }

        ssize_t sent = sendto(sockfd, batch.data(), n_in_batch * sizeof(WireOrder), 0,
                              reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));

        if (sent < 0) {
            _mm_pause();
            continue;
        }

        total_sent += n_in_batch;
        i += n_in_batch;

        if (i % 100000 < n_in_batch) {
            std::cout << "[UDP Client] Sent " << i << "/" << num_orders << " orders" << std::endl;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "\n[UDP Client] Done. Sent=" << total_sent
              << " packets=" << ((num_orders + batch_size - 1) / batch_size)
              << " duration=" << us << "us"
              << " throughput=" << (total_sent * 1e6 / us) << " ops/sec" << std::endl;

    close(sockfd);
    return 0;
}
