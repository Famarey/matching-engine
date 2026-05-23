#include "MatchingEngine.h"
#include "UDPServer.h"
#include "LockFreeQueue.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <iomanip>
#include <cstring>
#include <csignal>

using namespace matching_engine;

struct alignas(64) LoggerStats {
    std::atomic<uint64_t> log_count{0};
    std::atomic<uint64_t> error_count{0};
};

static std::atomic<bool> g_signal_shutdown{false};

void signal_handler(int) {
    g_signal_shutdown.store(true, std::memory_order_release);
}

class OrderGenerator {
public:
    static void generate_orders(OrderQueue& queue, 
                                uint64_t num_orders,
                                std::atomic<bool>& done,
                                std::atomic<uint64_t>& produced) {
        std::mt19937_64 rng(12345);
        std::uniform_int_distribution<int64_t> price_dist(100, 200);
        std::uniform_int_distribution<uint32_t> qty_dist(1, 500);
        std::uniform_int_distribution<int> side_dist(0, 1);
        std::uniform_int_distribution<int> type_dist(0, 10);

        for (uint64_t i = 1; i <= num_orders; ++i) {
            if (g_signal_shutdown.load(std::memory_order_relaxed)) break;

            int type_roll = type_dist(rng);
            OrderType type;
            
            switch (type_roll) {
                case 9: case 10:
                    type = OrderType::Market;
                    break;
                case 8:
                    type = OrderType::Cancel;
                    break;
                default:
                    type = OrderType::Limit;
                    break;
            }

            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            int64_t price = price_dist(rng);
            uint32_t qty = qty_dist(rng);

            if (type == OrderType::Cancel && i > 50) {
                price = static_cast<int64_t>(rng() % i + 1);
                qty = 0;
            }

            auto now = std::chrono::high_resolution_clock::now();
            uint64_t ts = now.time_since_epoch().count();

            Order order(i, side, type, price, qty, ts);

            while (!queue.push(order)) {
                if (g_signal_shutdown.load(std::memory_order_relaxed)) goto done;
                _mm_pause();
            }

            produced.fetch_add(1, std::memory_order_relaxed);

            if (i % 100000 == 0) {
                std::cout << "[Producer] Generated " << i << "/" 
                          << num_orders << " orders" << std::endl;
            }
        }
done:
        done.store(true, std::memory_order_release);
        std::cout << "[Producer] Finished. Generated " 
                  << produced.load() << " orders" << std::endl;
    }
};

void logger_thread_func(MatchingEngine& engine, 
                        std::atomic<bool>& running,
                        LoggerStats& stats) {
    uint64_t last_trade_count = 0;
    
    while (running.load(std::memory_order_acquire)) {
        auto current_trades = engine.get_stats().total_trades.load();
        
        if (current_trades > last_trade_count) {
            uint64_t new_trades = current_trades - last_trade_count;
            stats.log_count.fetch_add(new_trades, std::memory_order_relaxed);
            last_trade_count = current_trades;
            
            if (stats.log_count.load() % 10000 == 0) {
                std::cout << "[Logger] Processed " << stats.log_count.load() 
                          << " trade logs" << std::endl;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[Logger] Shutdown complete. Total logs: " 
              << stats.log_count.load() << std::endl;
}

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "\n"
              << "HFT Low-Latency Matching Engine\n"
              << "\n"
              << "Options:\n"
              << "  --mode MODE       Run mode: 'direct' (default) or 'udp'\n"
              << "  -n NUM_ORDERS    Number of orders for direct mode (default: 100000)\n"
              << "  -p PORT          UDP listen port for udp mode (default: 50000)\n"
              << "  --help           Show this help\n"
              << "\n"
              << "Modes:\n"
              << "  direct           In-memory order generation + matching (benchmark mode)\n"
              << "  udp              Listen on UDP socket for external order feed\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << "                        # Direct mode, 100K orders\n"
              << "  " << prog << " -n 1000000             # Direct mode, 1M orders\n"
              << "  " << prog << " --mode udp -p 50000     # UDP server on port 50000\n";
}

int run_direct_mode(uint64_t num_orders) {
    const uint64_t NUM_ORDERS = num_orders;

    std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   LOW-LATENCY MATCHING ENGINE - DIRECT MODE   ║" << std::endl;
    std::cout << "║     C++17 / Linux / Lock-Free / No-Mutex      ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
    std::cout << "\nInitializing matching engine..." << std::endl;
    std::cout << "- Allocator:  Slab (32B / 64B / 128B)" << std::endl;
    std::cout << "- Queue Capacity: 65,536 orders" << std::endl;
    std::cout << "- Matching: Price-Time Priority" << std::endl;
    std::cout << "- Concurrency: SPSC Lock-Free Queue" << std::endl;
    std::cout << "- OrderBook: Price Ladder + Intrusive Linked List" << std::endl;
    std::cout << "\nStarting multi-threaded processing..." << std::endl;
    std::cout << "----------------------------------------\n" << std::endl;

    MatchingEngine engine;
    engine.start();

    std::atomic<bool> producer_done{false};
    std::atomic<bool> system_running{true};
    std::atomic<uint64_t> produced{0};
    LoggerStats logger_stats;

    auto start_time = std::chrono::high_resolution_clock::now();

    std::thread producer(OrderGenerator::generate_orders,
                         std::ref(engine.get_input_queue()),
                         NUM_ORDERS,
                         std::ref(producer_done),
                         std::ref(produced));

    std::thread consumer(&MatchingEngine::run_consumer_loop, &engine);

    std::thread logger(logger_thread_func,
                       std::ref(engine),
                       std::ref(system_running),
                       std::ref(logger_stats));

    producer.join();

    while (!engine.get_input_queue().empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    engine.stop();
    system_running.store(false, std::memory_order_release);

    consumer.join();
    logger.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                        end_time - start_time).count();

    engine.print_stats();

    double throughput = (static_cast<double>(produced.load()) * 1000000.0) / duration;

    std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║              PERFORMANCE SUMMARY              ║" << std::endl;
    std::cout << "╠══════════════════════════════════════════════╣" << std::endl;
    std::cout << "║  Orders Processed:    " << std::setw(12) << produced.load() << "               ║" << std::endl;
    std::cout << "║  Duration:            " << std::setw(12) << duration << " us             ║" << std::endl;
    std::cout << "║  Throughput:          " << std::setw(12) << static_cast<uint64_t>(throughput)
              << " ops/sec       ║" << std::endl;
    std::cout << "║  Avg Latency:         " << std::setw(12) 
              << static_cast<uint64_t>(engine.get_stats().avg_latency_ns.load()) 
              << " ns           ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

    return 0;
}

int run_udp_mode(uint16_t port) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║   LOW-LATENCY MATCHING ENGINE - NETWORK MODE  ║" << std::endl;
    std::cout << "║    C++17 / Linux / UDP / Lock-Free / No-Mutex  ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
    std::cout << "\nInitializing matching engine..." << std::endl;
    std::cout << "- Allocator:  Slab (32B / 64B / 128B)" << std::endl;
    std::cout << "- Queue Capacity: 65,536 orders" << std::endl;
    std::cout << "- UDP Port: " << port << std::endl;
    std::cout << "- Wire Protocol: Binary WireOrder (34 bytes/packet)" << std::endl;
    std::cout << "- OrderBook: Price Ladder + Intrusive Linked List" << std::endl;
    std::cout << "\nStarting network matching engine..." << std::endl;
    std::cout << "----------------------------------------\n" << std::endl;
    std::cout << "(Send orders via: ./build/UDPClient -p " << port << ")" << std::endl;
    std::cout << "(Ctrl+C to gracefully shutdown)\n" << std::endl;

    MatchingEngine engine;
    engine.start();

    UDPServer udp_server(engine.get_input_queue(), port);
    if (!udp_server.start()) {
        std::cerr << "[Error] Failed to start UDP server" << std::endl;
        return 1;
    }

    std::atomic<bool> system_running{true};
    LoggerStats logger_stats;

    std::thread consumer(&MatchingEngine::run_consumer_loop, &engine);
    std::thread logger(logger_thread_func,
                       std::ref(engine),
                       std::ref(system_running),
                       std::ref(logger_stats));

    while (!g_signal_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto& net_stats = udp_server.get_stats();
        auto& eng_stats = engine.get_stats();
        std::cout << "[Status] "
                  << "pkts=" << net_stats.packets_received.load()
                  << " orders=" << net_stats.orders_dispatched.load()
                  << " trades=" << eng_stats.total_trades.load()
                  << " drops=" << net_stats.queue_full_drops.load()
                  << " errors=" << net_stats.parse_errors.load()
                  << std::endl;
    }

    std::cout << "\n[Main] Shutdown signal received, draining queue..." << std::endl;

    udp_server.stop();
    engine.stop();
    system_running.store(false, std::memory_order_release);

    while (!engine.get_input_queue().empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    consumer.join();
    logger.join();

    engine.print_stats();

    auto& net_stats = udp_server.get_stats();
    std::cout << "\n╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║            NETWORK STATISTICS                 ║" << std::endl;
    std::cout << "╠══════════════════════════════════════════════╣" << std::endl;
    std::cout << "║  Packets Received:    " << std::setw(12) << net_stats.packets_received.load() << "               ║" << std::endl;
    std::cout << "║  Bytes Received:      " << std::setw(12) << net_stats.bytes_received.load() << "               ║" << std::endl;
    std::cout << "║  Orders Dispatched:   " << std::setw(12) << net_stats.orders_dispatched.load() << "               ║" << std::endl;
    std::cout << "║  Queue Full Drops:    " << std::setw(12) << net_stats.queue_full_drops.load() << "               ║" << std::endl;
    std::cout << "║  Parse Errors:        " << std::setw(12) << net_stats.parse_errors.load() << "               ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;

    return 0;
}

int main(int argc, char* argv[]) {
    const char* mode = "direct";
    uint64_t num_orders = 100000;
    uint16_t udp_port = 50000;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            num_orders = std::stoull(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            udp_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    if (strcmp(mode, "udp") == 0) {
        return run_udp_mode(udp_port);
    } else {
        return run_direct_mode(num_orders);
    }
}
