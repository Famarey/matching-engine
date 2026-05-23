#include "SlabAllocator.h"
#include <new>
#include <cstring>

namespace matching_engine {

SlabAllocator::SlabAllocator(size_t capacity_per_class) {
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        auto& slab = slabs_[i];
        slab.block_size = CLASS_SIZES[i];
        slab.capacity = capacity_per_class;

        slab.memory = static_cast<char*>(
            ::operator new[](slab.block_size * slab.capacity, std::align_val_t{64})
        );

        for (size_t j = 0; j < slab.capacity; ++j) {
            char* block = slab.memory + j * slab.block_size;
            void** next_ptr = reinterpret_cast<void**>(block);
            if (j < slab.capacity - 1) {
                *next_ptr = static_cast<void*>(slab.memory + (j + 1) * slab.block_size);
            } else {
                *next_ptr = nullptr;
            }
        }

        slab.free_list.store(static_cast<void*>(slab.memory), std::memory_order_release);
        slab.allocated.store(0, std::memory_order_release);
        slab.available.store(slab.capacity, std::memory_order_release);

        available_count_.fetch_add(slab.capacity, std::memory_order_relaxed);
    }
}

SlabAllocator::~SlabAllocator() {
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        if (slabs_[i].memory) {
            ::operator delete[](slabs_[i].memory, std::align_val_t{64});
        }
    }
}

size_t SlabAllocator::select_class(size_t size) {
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        if (size <= CLASS_SIZES[i]) return i;
    }
    return NUM_CLASSES - 1;
}

void* SlabAllocator::alloc_from_slab(size_t class_idx) {
    auto& slab = slabs_[class_idx];

    void* head = slab.free_list.load(std::memory_order_acquire);
    while (head) {
        void* next = *reinterpret_cast<void**>(head);

        if (slab.free_list.compare_exchange_weak(
                head, next,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {

            slab.allocated.fetch_add(1, std::memory_order_relaxed);
            slab.available.fetch_sub(1, std::memory_order_relaxed);
            allocated_count_.fetch_add(1, std::memory_order_relaxed);
            available_count_.fetch_sub(1, std::memory_order_relaxed);

            return head;
        }

        head = slab.free_list.load(std::memory_order_acquire);
    }

    return nullptr;
}

void SlabAllocator::free_to_slab(void* ptr, size_t class_idx) {
    auto& slab = slabs_[class_idx];

    void* old_head = slab.free_list.load(std::memory_order_acquire);
    do {
        *reinterpret_cast<void**>(ptr) = old_head;
    } while (!slab.free_list.compare_exchange_weak(
                old_head, ptr,
                std::memory_order_acq_rel,
                std::memory_order_acquire));

    slab.allocated.fetch_sub(1, std::memory_order_relaxed);
    slab.available.fetch_add(1, std::memory_order_relaxed);
    allocated_count_.fetch_sub(1, std::memory_order_relaxed);
    available_count_.fetch_add(1, std::memory_order_relaxed);
}

Order* SlabAllocator::allocate() {
    size_t idx = select_class(sizeof(Order));
    void* raw = alloc_from_slab(idx);
    if (!raw) return nullptr;

    memset(raw, 0, sizeof(Order));
    return new (raw) Order();
}

void SlabAllocator::deallocate(Order* order) {
    if (!order) return;

    order->~Order();

    size_t idx = select_class(sizeof(Order));
    free_to_slab(static_cast<void*>(order), idx);
}

}
