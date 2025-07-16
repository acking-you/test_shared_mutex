#pragma once
#include <climits>
#include <condition_variable>
#include <mutex>

namespace clang {
    class shared_mutex {
        mutable std::mutex mut_;
        mutable std::condition_variable gate1_;
        mutable std::condition_variable gate2_;
        unsigned state_;

        static constexpr unsigned _write_entered_ = 1U << (sizeof(unsigned) * CHAR_BIT - 1);
        static constexpr unsigned _n_readers_ = ~_write_entered_;

    public:
        shared_mutex() : state_(0) {}
        shared_mutex(const shared_mutex &) = delete;
        shared_mutex &operator=(const shared_mutex &) = delete;

        // Exclusive locking (write)
        void lock() {
            std::unique_lock<std::mutex> lk(mut_);
            // 等待无写锁标记
            while (state_ & _write_entered_) {
                gate1_.wait(lk);
            }
            // 设置写锁标记
            state_ |= _write_entered_;
            // 等待所有读者退出
            while (state_ & _n_readers_) {
                gate2_.wait(lk);
            }
        }

        bool try_lock() {
            std::unique_lock<std::mutex> lk(mut_, std::try_to_lock);
            if (!lk.owns_lock()) {
                return false;
            }
            if (state_ != 0) { // 已存在读锁或写锁
                return false;
            }
            state_ = _write_entered_; // 设置写锁标记
            return true;
        }

        void unlock() {
            {
                std::lock_guard<std::mutex> lk(mut_);
                state_ = 0; // 清除写锁标记和读者计数
            }
            gate1_.notify_all(); // 唤醒所有等待的读写者
        }

        // Shared locking (read)
        void lock_shared() {
            std::unique_lock<std::mutex> lk(mut_);
            // 等待可读条件：无写锁且读者未达上限
            while ((state_ & _write_entered_) || ((state_ & _n_readers_) == _n_readers_)) {
                gate1_.wait(lk);
            }
            // 增加读者计数
            unsigned num_readers = (state_ & _n_readers_) + 1;
            state_ &= ~_n_readers_;
            state_ |= num_readers;
        }

        bool try_lock_shared() {
            std::unique_lock<std::mutex> lk(mut_, std::try_to_lock);
            if (!lk.owns_lock()) {
                return false;
            }
            if ((state_ & _write_entered_) || ((state_ & _n_readers_) == _n_readers_)) {
                return false;
            }
            state_ += 1; // 增加读者计数
            return true;
        }

        void unlock_shared() {
            std::unique_lock<std::mutex> lk(mut_);
            // 减少读者计数
            unsigned num_readers = (state_ & _n_readers_) - 1;
            state_ &= ~_n_readers_;
            state_ |= num_readers;

            if (state_ & _write_entered_) {
                if (num_readers == 0) {
                    lk.unlock();
                    gate2_.notify_one();
                }
            } else {
                if (num_readers == _n_readers_ - 1) {
                    lk.unlock();
                    gate1_.notify_one();
                }
            }
        }
    };
} // namespace clang
