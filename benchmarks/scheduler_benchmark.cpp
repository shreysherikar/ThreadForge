#include <threadforge/task_scheduler.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {

// Workload A: CPU-Bound deterministic calculation (iterative bit-mixing / hashing)
// Returns result to prevent dead-code elimination.
uint64_t cpu_workload(uint64_t iterations) {
    uint64_t hash = 14695981039346656037ULL;
    for (uint64_t i = 0; i < iterations; ++i) {
        hash ^= (i + 0x9e3779b97f4a7c15ULL);
        hash *= 1099511628211ULL;
        hash = (hash << 13) | (hash >> 51);
    }
    return hash;
}

// Workload B: Simulated I/O bound latency (sleep delay representing network/disk I/O wait)
void io_workload(std::chrono::microseconds delay) {
    std::this_thread::sleep_for(delay);
}

struct BenchmarkResult {
    std::string workload;
    std::size_t task_count;
    std::size_t worker_count;
    double elapsed_ms;
    double throughput;
    double speedup;
};

void print_table_header() {
    std::cout << std::left
              << std::setw(26) << "Workload"
              << std::setw(10) << "Tasks"
              << std::setw(10) << "Workers"
              << std::setw(16) << "Elapsed (ms)"
              << std::setw(18) << "Throughput (t/s)"
              << std::setw(12) << "Speedup"
              << "\n"
              << std::string(92, '-') << "\n";
}

void print_result(const BenchmarkResult& res) {
    std::cout << std::left
              << std::setw(26) << res.workload
              << std::setw(10) << res.task_count
              << std::setw(10) << res.worker_count
              << std::setw(16) << std::fixed << std::setprecision(2) << res.elapsed_ms
              << std::setw(18) << std::fixed << std::setprecision(1) << res.throughput
              << std::setw(12) << std::fixed << std::setprecision(2) << res.speedup
              << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    bool csv_mode = false;
    std::size_t repetitions = 3;
    std::size_t hw_workers = std::max(2u, std::thread::hardware_concurrency());

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--csv") == 0) {
            csv_mode = true;
        } else if (std::strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            repetitions = static_cast<std::size_t>(std::max(1, std::atoi(argv[++i])));
        } else if (std::strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            hw_workers = static_cast<std::size_t>(std::max(1, std::atoi(argv[++i])));
        }
    }

    const std::size_t fine_cpu_tasks = 20000;
    const uint64_t fine_iterations = 2500; // ~3 µs per task

    const std::size_t med_cpu_tasks = 2000;
    const uint64_t med_iterations = 100000; // ~120 µs per task

    const std::size_t io_tasks = 400;
    const auto io_delay = std::chrono::microseconds(1000); // 1 ms

    std::vector<BenchmarkResult> results;

    // Helper to average over repetitions
    auto measure_avg = [&](auto&& fn) {
        double total_ms = 0.0;
        for (std::size_t r = 0; r < repetitions; ++r) {
            const auto t0 = std::chrono::steady_clock::now();
            fn();
            const auto t1 = std::chrono::steady_clock::now();
            total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        return total_ms / static_cast<double>(repetitions);
    };

    // =========================================================================
    // 1. Fine-Grained CPU-Bound Benchmark (~3 µs per task)
    // =========================================================================
    double fine_seq_ms = measure_avg([&]() {
        uint64_t checksum = 0;
        for (std::size_t i = 0; i < fine_cpu_tasks; ++i) {
            checksum += cpu_workload(fine_iterations);
        }
        if (checksum == 0) std::cerr << "warning\n";
    });
    results.push_back({
        "CPU Fine ~3us (Seq)", fine_cpu_tasks, 1, fine_seq_ms,
        (static_cast<double>(fine_cpu_tasks) / fine_seq_ms) * 1000.0, 1.0
    });

    double fine_tf1_ms = measure_avg([&]() {
        threadforge::TaskScheduler scheduler(1);
        std::atomic<uint64_t> checksum{0};
        for (std::size_t i = 0; i < fine_cpu_tasks; ++i) {
            scheduler.submit([&checksum, fine_iterations]() {
                checksum.fetch_add(cpu_workload(fine_iterations), std::memory_order_relaxed);
            });
        }
        scheduler.wait_idle();
        scheduler.shutdown(true);
    });
    results.push_back({
        "CPU Fine ~3us (TF-1)", fine_cpu_tasks, 1, fine_tf1_ms,
        (static_cast<double>(fine_cpu_tasks) / fine_tf1_ms) * 1000.0, fine_seq_ms / fine_tf1_ms
    });

    double fine_tfn_ms = measure_avg([&]() {
        threadforge::TaskScheduler scheduler(hw_workers);
        std::atomic<uint64_t> checksum{0};
        for (std::size_t i = 0; i < fine_cpu_tasks; ++i) {
            scheduler.submit([&checksum, fine_iterations]() {
                checksum.fetch_add(cpu_workload(fine_iterations), std::memory_order_relaxed);
            });
        }
        scheduler.wait_idle();
        scheduler.shutdown(true);
    });
    results.push_back({
        "CPU Fine ~3us (TF-N)", fine_cpu_tasks, hw_workers, fine_tfn_ms,
        (static_cast<double>(fine_cpu_tasks) / fine_tfn_ms) * 1000.0, fine_seq_ms / fine_tfn_ms
    });

    // =========================================================================
    // 2. Medium-Grained CPU-Bound Benchmark (~120 µs per task)
    // =========================================================================
    double med_seq_ms = measure_avg([&]() {
        uint64_t checksum = 0;
        for (std::size_t i = 0; i < med_cpu_tasks; ++i) {
            checksum += cpu_workload(med_iterations);
        }
        if (checksum == 0) std::cerr << "warning\n";
    });
    results.push_back({
        "CPU Med ~120us (Seq)", med_cpu_tasks, 1, med_seq_ms,
        (static_cast<double>(med_cpu_tasks) / med_seq_ms) * 1000.0, 1.0
    });

    double med_tfn_ms = measure_avg([&]() {
        threadforge::TaskScheduler scheduler(hw_workers);
        std::atomic<uint64_t> checksum{0};
        for (std::size_t i = 0; i < med_cpu_tasks; ++i) {
            scheduler.submit([&checksum, med_iterations]() {
                checksum.fetch_add(cpu_workload(med_iterations), std::memory_order_relaxed);
            });
        }
        scheduler.wait_idle();
        scheduler.shutdown(true);
    });
    results.push_back({
        "CPU Med ~120us (TF-N)", med_cpu_tasks, hw_workers, med_tfn_ms,
        (static_cast<double>(med_cpu_tasks) / med_tfn_ms) * 1000.0, med_seq_ms / med_tfn_ms
    });

    // =========================================================================
    // 3. Simulated I/O Benchmark (1 ms delay per task)
    // =========================================================================
    double io_seq_ms = measure_avg([&]() {
        for (std::size_t i = 0; i < io_tasks; ++i) {
            io_workload(io_delay);
        }
    });
    results.push_back({
        "Sim-IO 1ms (Seq)", io_tasks, 1, io_seq_ms,
        (static_cast<double>(io_tasks) / io_seq_ms) * 1000.0, 1.0
    });

    double io_tfn_ms = measure_avg([&]() {
        threadforge::TaskScheduler scheduler(hw_workers);
        for (std::size_t i = 0; i < io_tasks; ++i) {
            scheduler.submit([io_delay]() {
                io_workload(io_delay);
            });
        }
        scheduler.wait_idle();
        scheduler.shutdown(true);
    });
    results.push_back({
        "Sim-IO 1ms (TF-N)", io_tasks, hw_workers, io_tfn_ms,
        (static_cast<double>(io_tasks) / io_tfn_ms) * 1000.0, io_seq_ms / io_tfn_ms
    });

    // =========================================================================
    // Output Formatting
    // =========================================================================
    if (csv_mode) {
        std::cout << "workload,task_count,workers,elapsed_ms,throughput_tps,speedup\n";
        for (const auto& r : results) {
            std::cout << r.workload << ","
                      << r.task_count << ","
                      << r.worker_count << ","
                      << std::fixed << std::setprecision(2) << r.elapsed_ms << ","
                      << std::fixed << std::setprecision(1) << r.throughput << ","
                      << std::fixed << std::setprecision(2) << r.speedup << "\n";
        }
    } else {
        std::cout << "\n=== ThreadForge Performance Benchmark Results (Averaged over "
                  << repetitions << " Repetitions) ===\n";
        print_table_header();
        for (const auto& r : results) {
            print_result(r);
        }
        std::cout << "\nEngineering Insights:\n"
                  << "  - CPU Fine (~3us): Microscopic tasks highlight queue lock contention and scheduling overhead.\n"
                  << "  - CPU Med (~120us): Amortizes queue synchronization, delivering multi-core parallel speedup.\n"
                  << "  - Sim-IO (1ms): Latency-bound workloads achieve near-linear multi-threaded speedup ("
                  << hw_workers << " workers).\n";
    }

    return 0;
}
