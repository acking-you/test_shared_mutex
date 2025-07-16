#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include "SharedMutex.h"
#include "clang_shared_mutex.h"


// ====== 基准测试核心 ======
template<typename MutexType>
class BenchmarkRunner {
public:
    explicit BenchmarkRunner(int readers, int writers) :
        reader_count(readers), writer_count(writers), read_ops(0), write_ops(0), start_flag(false), run_time_ms(0) {}

    void run_test(int test_duration_ms) {
        std::vector<std::thread> threads;
        read_ops.store(0);
        write_ops.store(0);
        start_flag.store(false);

        // 创建读线程
        threads.reserve(reader_count);
        for (int i = 0; i < reader_count; ++i) {
            threads.emplace_back([this] { reader_job(); });
        }

        // 创建写线程
        for (int i = 0; i < writer_count; ++i) {
            threads.emplace_back([this] { writer_job(); });
        }

        // 同步启动所有线程
        start_flag.store(true);
        auto start_time = std::chrono::high_resolution_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(test_duration_ms));
        running.store(false);

        // 清理线程
        for (auto &t: threads) {
            if (t.joinable())
                t.join();
        }
        // 计算实际运行时间
        auto end_time = std::chrono::high_resolution_clock::now();
        run_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    }

    // 获取测试结果
    long get_read_ops() const { return read_ops.load(); }
    long get_write_ops() const { return write_ops.load(); }
    long get_total_ops() const { return read_ops.load() + write_ops.load(); }
    double get_ops_per_sec() const { return (run_time_ms > 0) ? get_total_ops() / (run_time_ms / 1000.0) : 0; }
    long get_run_time() const { return run_time_ms; }

private:
    void reader_job() {
        // 等待同步启动
        while (!start_flag.load()) {
            std::this_thread::yield();
        }

        while (running.load(std::memory_order_relaxed)) {
            {
                std::shared_lock<MutexType> lock(mtx);
                // 模拟读取操作 - 保持锁一定时间
                dummy_work(work_duration_us);
            }
            read_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void writer_job() {
        while (!start_flag.load()) {
            std::this_thread::yield();
        }

        while (running.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<MutexType> lock(mtx);
                dummy_work(work_duration_us * write_weight);
            }
            write_ops.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void dummy_work(int microseconds) {
        auto start = std::chrono::high_resolution_clock::now();
        while (true) {
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start);
            if (elapsed.count() >= microseconds)
                break;
        }
    }

    MutexType mtx;
    const int reader_count;
    const int writer_count;
    const int work_duration_us = 10; // 10us 模拟工作负载
    const int write_weight = 3; // 写操作耗时倍率

    std::atomic<bool> start_flag;
    std::atomic<bool> running{true};
    std::atomic<long> read_ops;
    std::atomic<long> write_ops;
    long run_time_ms;
};

// ====== 性能测试函数 ======
template<typename MutexType>
void run_performance_tests(const std::string &mutex_name, int duration_ms,
                           unsigned int max_threads = std::thread::hardware_concurrency()) {

    // 测试场景配置：{读者数, 写者数}
    std::vector<std::pair<int, int>> scenarios = {
            {max_threads, 0}, // 纯读测试
            {0, max_threads}, // 纯写测试
            {max_threads, max_threads / 4}, // 混合测试
            {max_threads / 4, max_threads}, // 写密集型
            {max_threads, 1}, // 低竞争写
            {1, max_threads} // 高竞争写
    };

    std::cout << "\n===== 性能测试 (" << mutex_name << ") =====\n";
    std::cout << "测试时长: " << duration_ms << "ms\n";
    std::cout << "硬件并发数: " << max_threads << std::endl;

    for (const auto &[readers, writers]: scenarios) {
        if (readers == 0 && writers == 0)
            continue;

        BenchmarkRunner<MutexType> benchmark(readers, writers);
        benchmark.run_test(duration_ms);
        std::cout << std::fixed << std::setprecision(0) << "\n场景: " << readers << " 读者, " << writers << " 写者\n"
                  << "  总操作: " << benchmark.get_total_ops() << " (" << benchmark.get_read_ops() << " 读 + "
                  << benchmark.get_write_ops() << " 写)\n"
                  << "  耗时: " << benchmark.get_run_time() << " ms\n"
                  << "  读写吞吐量: " << benchmark.get_ops_per_sec() << " op/s\n";
    }
}

// ====== 正确性验证 ======
template<typename MutexType>
void test_correctness() {
    constexpr int num_threads = 8;
    constexpr int iterations = 100000;

    MutexType mtx;
    int shared_value = 0;
    std::atomic<int> read_count{0};
    std::atomic<bool> start{false};

    auto reader = [&](int id) {
        while (!start) {
        }

        for (int i = 0; i < iterations; ++i) {
            int value_copy;
            {
                std::shared_lock lock(mtx);
                value_copy = shared_value;
            }
            // 验证读取时值不会减少
            if (value_copy < 0) {
                std::cerr << "错误! 读取到负值: " << value_copy << std::endl;
                std::abort();
            }
            read_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto writer = [&](int id) {
        while (!start) {
        }

        for (int i = 0; i < iterations; ++i) {
            {
                std::unique_lock lock(mtx);
                ++shared_value;
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads / 2; ++i) {
        threads.emplace_back(reader, i);
        threads.emplace_back(writer, i);
    }

    start = true;
    for (auto &t: threads) {
        t.join();
    }

    // 验证最终值
    int expected_writes = (num_threads / 2) * iterations;
    int actual_reads = read_count.load();

    std::cout << "\n===== 正确性测试 =====\n";
    std::cout << "最终值: " << shared_value << " (预期: " << expected_writes << ")\n";
    std::cout << "总读取: " << actual_reads << " (预期: ~" << (num_threads / 2) * iterations << ")\n";

    assert(shared_value == expected_writes);
    assert(actual_reads > 0);

    std::cout << "所有测试通过!\n";
}

using namespace std::chrono_literals;
// ====== 主函数 ======
int main() {
    // 第1步：验证正确性
    test_correctness<std::shared_mutex>();
    test_correctness<clang::shared_mutex>();
    test_correctness<DB::SharedMutex>();

    // 第2步：运行性能测试
    constexpr int test_duration = 2000; // 0.2秒测试时间

    // 测试标准库实现（可能是 gcc，在 Linux 平台下会使用 pthread_rwlock
    run_performance_tests<std::shared_mutex>("std::shared_mutex", test_duration, 64);
    // 测试模拟实现的 clang 中的源码实现
    std::this_thread::sleep_for(2s);
    run_performance_tests<clang::shared_mutex>("clang::shared_mutex", test_duration, 64);
    // 测试自定义实现
    std::this_thread::sleep_for(2s);
    run_performance_tests<DB::SharedMutex>("DB::SharedMutex", test_duration, 64);

    return 0;
}
