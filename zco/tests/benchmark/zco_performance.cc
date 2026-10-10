#include "execution/continuation.h"
#include "zco/zco.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
using namespace zco;

namespace {
std::atomic<size_t> allocations{0};
}

void *operator new(size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto *memory = std::malloc(size ? size : 1))
        return memory;
    throw std::bad_alloc();
}

void *operator new[](size_t size) { return ::operator new(size); }

void operator delete(void *memory) noexcept { std::free(memory); }

void operator delete[](void *memory) noexcept { std::free(memory); }

void operator delete(void *memory, size_t) noexcept { std::free(memory); }

void operator delete[](void *memory, size_t) noexcept { std::free(memory); }

namespace {
size_t resident_kib() {
    std::ifstream input("/proc/self/statm");
    size_t total = 0, resident = 0;
    input >> total >> resident;
    return resident * static_cast<size_t>(::sysconf(_SC_PAGESIZE)) / 1024;
}

void benchmark(const char *name, size_t operations,
               const std::function<void()> &fn) {
    auto allocated = allocations.load();
    auto copied = detail::snapshot_bytes_copied();
    auto start = Deadline::Clock::now();
    fn();
    auto seconds =
        std::chrono::duration<double>(Deadline::Clock::now() - start).count();
    std::cout << name << " operations=" << operations << " seconds=" << seconds
              << " ops/s=" << operations / seconds
              << " new_allocations=" << allocations.load() - allocated
              << " snapshot_bytes=" << detail::snapshot_bytes_copied() - copied
              << '\n';
}
} // namespace

int main() {
    size_t scale = 100;
    if (auto *value = std::getenv("ZCO_PERF_SCALE_PCT"))
        scale = std::max(1, std::atoi(value));
    size_t count = 1000 * scale;
    const char *selected_model = std::getenv("ZCO_PERF_STACK_MODEL");
    if (selected_model && std::string(selected_model) != "shared" &&
        std::string(selected_model) != "independent")
        throw std::invalid_argument("ZCO_PERF_STACK_MODEL");
    for (auto model : {StackModel::kShared, StackModel::kIndependent}) {
        const char *model_name =
            model == StackModel::kShared ? "shared" : "independent";
        if (selected_model && std::string(selected_model) != model_name)
            continue;
        std::cout << "stack="
                  << model_name
                  << '\n';
        RuntimeOptions options{2};
        options.stack_model = model;
        Runtime runtime(options);
        std::vector<TaskHandle> warmup;
        warmup.reserve(2000);
        for (size_t i = 0; i < 2000; ++i)
            warmup.push_back(std::move(runtime.spawn([] {})).value());
        for (auto &task : warmup)
            task.join().value();
        warmup.clear();
        benchmark("submit", count, [&] {
            std::vector<TaskHandle> tasks;
            tasks.reserve(count);
            for (size_t i = 0; i < count; ++i)
                tasks.push_back(std::move(runtime.spawn([] {})).value());
            for (auto &task : tasks) {
                task.join().value();
                if (task.status() != TaskStatus::succeeded)
                    throw std::runtime_error("submit status mismatch");
            }
        });
        benchmark("yield", count, [&] {
            auto task = runtime.spawn([&] {
                for (size_t i = 0; i < count; ++i)
                    yield();
            });
            task.value().join().value();
        });
        benchmark("channel", count, [&] {
            Channel<size_t> channel(64);
            auto sender = runtime.spawn([&] {
                for (size_t i = 0; i < count; ++i)
                    channel.send(i).value();
                channel.close();
            });
            size_t received = 0;
            while (auto value = channel.receive()) {
                if (value.value() != received++)
                    throw std::runtime_error("channel value mismatch");
            }
            sender.value().join().value();
            if (received != count)
                throw std::runtime_error("channel count mismatch");
        });
        size_t waits = std::max<size_t>(10, count / 100);
        benchmark("timer", waits, [&] {
            auto task = runtime.spawn([&] {
                for (size_t i = 0; i < waits; ++i)
                    sleep_for(std::chrono::milliseconds(1)).value();
            });
            task.value().join().value();
        });
        std::vector<double> latency(waits);
        benchmark("timer_latency", waits, [&] {
            auto task = runtime.spawn([&] {
                for (size_t i = 0; i < waits; ++i) {
                    auto deadline =
                        Deadline::after(std::chrono::milliseconds(1));
                    sleep_until(deadline).value();
                    latency[i] = std::chrono::duration<double, std::micro>(
                                     Deadline::Clock::now() - deadline.time())
                                     .count();
                }
            });
            task.value().join().value();
        });
        std::sort(latency.begin(), latency.end());
        std::cout << "timer_lateness_us p50=" << latency[latency.size() / 2]
                  << " p95=" << latency[(latency.size() - 1) * 95 / 100]
                  << " p99=" << latency[(latency.size() - 1) * 99 / 100]
                  << " max=" << latency.back() << '\n';
        int pair[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair))
            return 1;
        io::Descriptor reader(pair[0]), writer(pair[1]);
        benchmark("io", count, [&] {
            auto sending = runtime.spawn([&] {
                char data[64];
                for (size_t i = 0; i < count; ++i) {
                    std::fill(std::begin(data), std::end(data),
                              static_cast<char>(i));
                    auto result = io::write_all(writer, data, sizeof(data));
                    if (result.error)
                        throw std::system_error(result.error);
                    if (result.bytes != sizeof(data) || result.eof)
                        throw std::runtime_error("incomplete write");
                }
            });
            auto reading = runtime.spawn([&] {
                char data[64];
                for (size_t i = 0; i < count; ++i) {
                    auto result = io::read_exact(reader, data, sizeof(data));
                    if (result.error)
                        throw std::system_error(result.error);
                    if (result.bytes != sizeof(data) || result.eof ||
                        !std::all_of(std::begin(data), std::end(data),
                                     [i](char byte) {
                                         return byte == static_cast<char>(i);
                                     }))
                        throw std::runtime_error("io content mismatch");
                }
            });
            sending.value().join().value();
            reading.value().join().value();
        });
        auto before_stop = resident_kib();
        runtime.request_stop();
        runtime.join();
        std::cout << "rss_kib before_stop=" << before_stop
                  << " after_join=" << resident_kib() << '\n';
    }
    rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
    std::cout << "max_rss_kib=" << usage.ru_maxrss << '\n';
}
