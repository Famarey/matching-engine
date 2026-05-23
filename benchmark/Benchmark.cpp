#include "MatchingEngine.h"
#include "LockFreeQueue.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <xmmintrin.h>

using namespace matching_engine;

struct alignas(64) LatencyRecorder {
    static constexpr size_t MAX_SAMPLES = 1000000;
    alignas(64) std::atomic<size_t> write_idx{0};
    uint64_t* samples;

    LatencyRecorder() { samples = new uint64_t[MAX_SAMPLES]; }
    ~LatencyRecorder() { delete[] samples; }

    LatencyRecorder(const LatencyRecorder&) = delete;
    LatencyRecorder& operator=(const LatencyRecorder&) = delete;

    void record(uint64_t ns) {
        size_t idx = write_idx.fetch_add(1, std::memory_order_relaxed);
        if (idx < MAX_SAMPLES) {
            samples[idx] = ns;
        }
    }

    struct Percentiles {
        uint64_t p50, p90, p99, p99_9;
        double avg, min_val, max_val;
        size_t count;
    };

    Percentiles compute() const {
        size_t n = std::min(write_idx.load(std::memory_order_relaxed), MAX_SAMPLES);
        if (n == 0) return {0, 0, 0, 0, 0.0, 0, 0, 0};

        auto* sorted = new uint64_t[n];
        std::copy_n(samples, n, sorted);
        std::sort(sorted, sorted + n);

        Percentiles p;
        p.count = n;
        p.p50 = sorted[n * 50 / 100];
        p.p90 = sorted[n * 90 / 100];
        p.p99 = sorted[n * 99 / 100];
        p.p99_9 = (n >= 1000) ? sorted[n * 999 / 1000] : sorted[n - 1];

        double sum = 0;
        for (size_t i = 0; i < n; ++i) sum += sorted[i];
        p.avg = sum / n;
        p.min_val = sorted[0];
        p.max_val = sorted[n - 1];

        delete[] sorted;
        return p;
    }
};

void generate_orders(OrderQueue& queue,
                     uint64_t num_orders,
                     std::atomic<bool>& done,
                     std::atomic<uint64_t>& produced) {
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int64_t> price_dist(100, 200);
    std::uniform_int_distribution<uint32_t> qty_dist(1, 500);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> type_dist(0, 10);

    for (uint64_t i = 1; i <= num_orders; ++i) {
        int type_roll = type_dist(rng);
        OrderType type;

        switch (type_roll) {
            case 9: case 10: type = OrderType::Market; break;
            case 8:         type = OrderType::Cancel; break;
            default:        type = OrderType::Limit; break;
        }

        Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        int64_t price = price_dist(rng);
        uint32_t qty = qty_dist(rng);

        if (type == OrderType::Cancel && i > 50) {
            price = static_cast<int64_t>(rng() % i + 1);
            qty = 0;
        }

        auto ts = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        Order order(i, side, type, price, qty, ts);

        while (!queue.push(order)) {
            _mm_pause();
        }
        produced.fetch_add(1, std::memory_order_relaxed);
    }

    done.store(true, std::memory_order_release);
}

void run_benchmark_phase(MatchingEngine& engine,
                         OrderQueue& queue,
                         uint64_t num_orders,
                         LatencyRecorder& recorder) {
    std::atomic<bool> done{false};
    std::atomic<uint64_t> produced{0};

    std::thread producer(generate_orders,
                         std::ref(queue),
                         num_orders,
                         std::ref(done),
                         std::ref(produced));

    std::thread consumer([&]() {
        Order order;
        while (!done.load(std::memory_order_acquire) || !queue.empty()) {
            if (queue.pop(order)) {
                auto t0 = std::chrono::high_resolution_clock::now();
                engine.process_order(order);
                auto t1 = std::chrono::high_resolution_clock::now();
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                recorder.record(ns);
            } else {
                _mm_pause();
            }
        }
    });

    producer.join();

    while (!queue.empty()) {
        _mm_pause();
    }

    consumer.join();
}

int main(int argc, char* argv[]) {
    uint64_t num_orders = 1000000;
    uint64_t warmup_orders = 50000;

    if (argc > 1) {
        try { num_orders = std::stoull(argv[1]); } catch (...) {}
    }

    std::cout << "========== HFT MATCHING ENGINE BENCHMARK ==========" << std::endl;
    std::cout << "- Orders:       " << num_orders << " (+ warmup " << warmup_orders << ")" << std::endl;
    std::cout << "- Allocator:    Slab (32B/64B/128B)" << std::endl;
    std::cout << "- OrderBook:    Price Ladder + Intrusive List" << std::endl;
    std::cout << "- Timer:        std::chrono (VM-safe)" << std::endl;
    std::cout << "- Latency:      Lock-free ring buffer (no mutex)" << std::endl;
    std::cout << "==================================================" << std::endl;

    std::cout << "\n[Warmup] Running " << warmup_orders << " orders..." << std::endl;
    {
        MatchingEngine warmup_engine;
        warmup_engine.start();
        OrderQueue warmup_queue;
        LatencyRecorder dummy;

        run_benchmark_phase(warmup_engine, warmup_queue, warmup_orders, dummy);

        std::cout << "[Warmup] Done." << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::cout << "\n[Run] Starting " << num_orders << " orders benchmark..." << std::endl;

    LatencyRecorder recorder;
    MatchingEngine engine;
    engine.start();
    OrderQueue input_queue;

    auto start_time = std::chrono::high_resolution_clock::now();

    run_benchmark_phase(engine, input_queue, num_orders, recorder);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                           end_time - start_time).count();

    engine.stop();

    auto pct = recorder.compute();

    double throughput = (static_cast<double>(pct.count) * 1e6) / duration_us;

    std::cout << "\n========== HFT MATCHING ENGINE BENCHMARK ==========" << std::endl;
    std::cout << "Total Orders:        " << pct.count << std::endl;
    std::cout << "Duration:             " << duration_us << " us" << std::endl;
    std::cout << "Throughput:           " << std::fixed << std::setprecision(0)
              << throughput << " ops/sec" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "Latency (MATCHING ONLY, nanoseconds):" << std::endl;
    std::cout << "  p50:                 " << pct.p50 << " ns" << std::endl;
    std::cout << "  p90:                 " << pct.p90 << " ns" << std::endl;
    std::cout << "  p99:                 " << pct.p99 << " ns" << std::endl;
    std::cout << "  p99.9:               " << pct.p99_9 << " ns" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "  avg:                 " << static_cast<uint64_t>(pct.avg) << " ns" << std::endl;
    std::cout << "  min:                 " << pct.min_val << " ns" << std::endl;
    std::cout << "  max:                 " << pct.max_val << " ns" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "==================================================" << std::endl;

    engine.print_stats();

    return 0;
}
