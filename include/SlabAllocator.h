#pragma once

#include <cstdint>
#include <atomic>
#include <vector>
#include "Order.h"

namespace matching_engine {

class SlabAllocator {
public:
    static constexpr size_t NUM_CLASSES = 3;
    static constexpr size_t CLASS_SIZES[NUM_CLASSES] = {32, 64, 128};
    static constexpr size_t DEFAULT_CAPACITY = 100000;

    explicit SlabAllocator(size_t capacity_per_class = DEFAULT_CAPACITY);
    ~SlabAllocator();

    Order* allocate();
    void deallocate(Order* order);

    size_t allocated_count() const { return allocated_count_.load(std::memory_order_relaxed); }
    size_t available_count() const { return available_count_.load(std::memory_order_relaxed); }

    size_t class_index() const { return ORDER_CLASS; }
    static constexpr size_t ORDER_CLASS = 1;

private:
    struct alignas(64) SlabHeader {
        char* memory{nullptr};
        size_t block_size{0};
        size_t capacity{0};
        alignas(64) std::atomic<void*> free_list{nullptr};
        alignas(64) std::atomic<size_t> allocated{0};
        alignas(64) std::atomic<size_t> available{0};
    };

    static size_t select_class(size_t size);
    void* alloc_from_slab(size_t class_idx);
    void free_to_slab(void* ptr, size_t class_idx);

    SlabHeader slabs_[NUM_CLASSES];
    alignas(64) std::atomic<size_t> allocated_count_{0};
    alignas(64) std::atomic<size_t> available_count_{0};

    static_assert(sizeof(Order) <= 128, "Order must fit in 128B slab");
    static_assert(sizeof(Order) > 64, "Order should use 128B slab for intrusive pointers");
};

}
