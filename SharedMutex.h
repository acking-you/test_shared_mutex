#pragma once

#include <atomic>

namespace DB {

    // Faster implementation of `std::shared_mutex` based on a pair of futexes
    class SharedMutex {
    public:
        SharedMutex();
        ~SharedMutex() = default;
        SharedMutex(const SharedMutex &) = delete;
        SharedMutex &operator=(const SharedMutex &) = delete;

        // Exclusive ownership
        void lock();
        bool try_lock();
        void unlock();

        // Shared ownership
        void lock_shared();
        bool try_lock_shared();
        void unlock_shared();

    private:
        static constexpr uint64_t readers = (1ull << 32ull) - 1ull; // Lower 32 bits of state
        static constexpr uint64_t writers = ~readers; // Upper 32 bits of state

        alignas(64) std::atomic<uint64_t> state;
        std::atomic<uint32_t> waiters;
    };

} // namespace DB
