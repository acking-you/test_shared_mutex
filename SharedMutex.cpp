#include "SharedMutex.h"


#include <bit>
#include <linux/futex.h>


namespace DB {
    namespace {
        inline int64_t futexWait(void *address, uint32_t value) {
            return syscall(SYS_futex, address, FUTEX_WAIT_PRIVATE, value, nullptr, nullptr, 0);
        }

        inline int64_t futexWake(void *address, int count) {
            return syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
        }

        inline constexpr uint32_t lowerHalf(uint64_t value) { return static_cast<uint32_t>(value & 0xffffffffull); }

        inline constexpr uint32_t upperHalf(uint64_t value) { return static_cast<uint32_t>(value >> 32ull); }


        inline uint32_t *upperHalfAddress(void *address) {
            return reinterpret_cast<uint32_t *>(address) + (std::endian::native == std::endian::little);
        }

        inline uint32_t *lowerHalfAddress(void *address) {
            return reinterpret_cast<uint32_t *>(address) + (std::endian::native == std::endian::big);
        }

        inline void futexWaitUpperFetch(std::atomic<uint64_t> &address, uint64_t &value) {
            futexWait(upperHalfAddress(&address), upperHalf(value));
            value = address.load();
        }

        inline void futexWaitLowerFetch(std::atomic<uint64_t> &address, uint64_t &value) {
            futexWait(lowerHalfAddress(&address), lowerHalf(value));
            value = address.load();
        }

        inline void futexWakeUpperAll(std::atomic<uint64_t> &address) {
            futexWake(upperHalfAddress(&address), INT_MAX);
        }

        inline void futexWakeLowerOne(std::atomic<uint64_t> &address) { futexWake(lowerHalfAddress(&address), 1); }
    } // namespace

    SharedMutex::SharedMutex() : state(0), waiters(0) {}

    void SharedMutex::lock() {
        uint64_t value = state.load();
        while (true) {
            if (value & writers) {
                waiters++;
                futexWaitUpperFetch(state, value);
                waiters--;
            } else if (state.compare_exchange_strong(value, value | writers))
                break;
        }

        value |= writers;
        while (value & readers)
            futexWaitLowerFetch(state, value);
    }

    bool SharedMutex::try_lock() {
        uint64_t value = 0;
        return state.compare_exchange_strong(value, writers);
    }

    void SharedMutex::unlock() {
        state.store(0);
        if (waiters)
            futexWakeUpperAll(state);
    }

    void SharedMutex::lock_shared() {
        uint64_t value = state.load();
        while (true) {
            if (value & writers) {
                waiters++;
                futexWaitUpperFetch(state, value);
                waiters--;
            } else if (state.compare_exchange_strong(value, value + 1))
                break;
        }
    }

    bool SharedMutex::try_lock_shared() {
        uint64_t value = state.load();
        while (true) {
            if (value & writers)
                return false;
            if (state.compare_exchange_strong(value, value + 1))
                break;
            // Concurrent try_lock_shared() should not fail, so we have to retry CAS, but avoid blocking wait
        }
        return true;
    }

    void SharedMutex::unlock_shared() {
        uint64_t value = state.fetch_sub(1) - 1;
        if (value == writers)
            futexWakeLowerOne(state); // Wake writer
    }

} // namespace DB
