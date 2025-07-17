#pragma once
#include <climits>
#include <condition_variable>
#include <mutex>

namespace clang {
    /// A shared mutex type implemented using std::condition_variable.
    class shared_mutex {
        // Based on Howard Hinnant's reference implementation from N2406.

        // The high bit of _M_state is the write-entered flag which is set to
        // indicate a writer has taken the lock or is queuing to take the lock.
        // The remaining bits are the count of reader locks.
        //
        // To take a reader lock, block on gate1 while the write-entered flag is
        // set or the maximum number of reader locks is held, then increment the
        // reader lock count.
        // To release, decrement the count, then if the write-entered flag is set
        // and the count is zero then signal gate2 to wake a queued writer,
        // otherwise if the maximum number of reader locks was held signal gate1
        // to wake a reader.
        //
        // To take a writer lock, block on gate1 while the write-entered flag is
        // set, then set the write-entered flag to start queueing, then block on
        // gate2 while the number of reader locks is non-zero.
        // To release, unset the write-entered flag and signal gate1 to wake all
        // blocked readers and writers.
        //
        // This means that when no reader locks are held readers and writers get
        // equal priority. When one or more reader locks is held a writer gets
        // priority and no more reader locks can be taken while the writer is
        // queued.

        // Only locked when accessing _M_state or waiting on condition variables.
        mutable std::mutex mut_;
        mutable std::condition_variable gate1_;
        mutable std::condition_variable gate2_;
        unsigned state_;

        static constexpr unsigned write_entered_ = 1U << (sizeof(unsigned) * CHAR_BIT - 1);
        static constexpr unsigned max_readers_ = ~write_entered_;

    private:
        bool write_entered() const { return state_ & write_entered_; }

        unsigned readers() const { return state_ & max_readers_; }

    public:
        shared_mutex() : state_(0) {}
        shared_mutex(const shared_mutex &) = delete;
        shared_mutex &operator=(const shared_mutex &) = delete;

        // Exclusive locking (write)
        void lock() {
            std::unique_lock<std::mutex> lk(mut_);
            // Wait until we can set the write-entered flag.
            gate1_.wait(lk, [this]() { return !write_entered(); });
            state_ |= write_entered_; // Writers are preferred
            // Wait for all readers to exit.
            gate2_.wait(lk, [this]() { return readers() == 0; });
        }

        bool try_lock() {
            std::unique_lock<std::mutex> lk(mut_, std::try_to_lock);
            if (!lk.owns_lock()) {
                return false;
            }
            if (state_ != 0) { // A read lock or write lock already exists
                return false;
            }
            state_ = write_entered_; // Set write lock flag
            return true;
        }

        void unlock() {
            {
                std::lock_guard<std::mutex> lk(mut_);
                state_ = 0; // Clear the write lock flag and reader count
            }
            // call notify_all() while mutex is held so that another thread can't
            // lock and unlock the mutex then destroy *this before we make the call.
            gate1_.notify_all();
        }

        // Shared locking (read)
        void lock_shared() {
            std::unique_lock<std::mutex> lk(mut_);
            gate1_.wait(lk, [this]() { return state_ < max_readers_; });
            ++state_;
        }

        bool try_lock_shared() {
            std::unique_lock<std::mutex> lk(mut_, std::try_to_lock);
            if (!lk.owns_lock()) {
                return false;
            }
            if (state_ < max_readers_) {
                state_ += 1;
                return true;
            }
            return false;
        }

        void unlock_shared() {
            std::unique_lock<std::mutex> lk(mut_);
            assert(readers() > 0);
            auto prev = state_--;
            if (write_entered()) { // Writers are preferred
                // Wake the queued writer if there are no more readers.
                if (readers() == 0) {
                    lk.unlock();
                    gate2_.notify_one();
                }
                // No need to notify gate1 because we give priority to the queued
                // writer, and that writer will eventually notify gate1 after it
                // clears the write-entered flag.
            } else {
                // Wake any thread that was blocked on reader overflow.
                if (prev == max_readers_) {
                    lk.unlock();
                    gate1_.notify_one();
                }
            }
        }
    };
} // namespace clang
