#include "zhttp/http_application.h"
#include "zhttp/rate_limiter.h"
#include "protocol/http/response_encoder.h"
#include "static_files/static_resource_store.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace zhttp;
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto warmup = std::chrono::milliseconds{50};
constexpr auto measurement = std::chrono::milliseconds{200};
constexpr int repetitions = 5;

// 每批检查时间，避免逐操作读时钟掩盖被测成本。校验和防止结果被优化掉。
template <class Operation>
void measure(const std::string &name, Operation operation) {
    for (int repeat = 0; repeat < repetitions; ++repeat) {
        auto until = Clock::now() + warmup;
        size_t checksum = 0;
        do {
            for (int i = 0; i < 1024; ++i) checksum += operation();
        } while (Clock::now() < until);
        size_t operations = 0;
        const auto begin = Clock::now();
        until = begin + measurement;
        do {
            for (int i = 0; i < 1024; ++i) checksum += operation();
            operations += 1024;
        } while (Clock::now() < until);
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - begin).count();
        std::cout << name << ',' << repeat + 1 << ',' << operations << ','
                  << elapsed / operations << ',' << checksum << '\n';
    }
}

struct Directory {
    std::string root;
    Directory() {
        char path[] = "/tmp/zhttp-core-benchmark-XXXXXX";
        auto *created = ::mkdtemp(path);
        if (!created) throw std::runtime_error("Cannot create benchmark directory");
        root = created;
    }
    ~Directory() { std::filesystem::remove_all(root); }
};

void cache_benchmarks() {
    Directory directory;
    for (int count : {1, 64, 256, 1024}) {
        detail::StaticResourceStore::CachePolicy policy;
        policy.ttl = std::chrono::seconds{3600};
        detail::StaticResourceStore store(directory.root, "index.html", false, policy);
        const std::vector<std::string> encodings{""};
        for (int i = 0; i < count; ++i) {
            const auto name = "file" + std::to_string(i);
            std::ofstream(directory.root + '/' + name) << std::string(128, 'x');
            if (!store.lookup(name, false, encodings, detail::StaticResourceStore::ReadMode::Content).resource)
                throw std::runtime_error("Cannot populate cache");
        }
        if (store.cached_entries() != static_cast<size_t>(count))
            throw std::runtime_error("Unexpected cache cardinality");
        const auto name = "file" + std::to_string(count - 1);
        measure("cache/" + std::to_string(count), [&] {
            auto result = store.lookup(name, false, encodings, detail::StaticResourceStore::ReadMode::Content);
            if (!result.resource) throw std::runtime_error("Cache miss");
            return result.resource->size;
        });
    }
}

void pipeline_benchmarks() {
    for (int count : {0, 4, 16}) {
        HttpApplication application;
        application.router().get("/api/v1/health", [](HttpContext &context) { context.response().text("OK"); });
        for (int i = 0; i < count; ++i) application.use(std::make_shared<mid::Middleware>());
        application.freeze();
        auto request = std::make_shared<HttpRequest>();
        request->set_method(HttpMethod::GET);
        request->set_target("/api/v1/health");
        measure("pipeline/" + std::to_string(count), [&] {
            HttpContext context(request);
            if (!application.handle(context))
                throw std::runtime_error("Pipeline route missing");
            return context.response().body_content().size();
        });
    }
}

void limiter_benchmarks() {
    for (bool shared : {false, true}) {
        for (int count : {1, 2, 4, 8}) {
            for (int repeat = 0; repeat < repetitions; ++repeat) {
                mid::FixedWindowRateLimiter limiter(1000000000000ULL, mid::RateLimiter::TimeUnit::HOUR);
                std::atomic<int> ready{0};
                std::atomic<bool> start{false};
                std::vector<size_t> operations(count, 0), checksums(count, 0);
                std::vector<std::thread> threads;
                Clock::time_point until;
                for (int i = 0; i < count; ++i) threads.emplace_back([&, i] {
                    const auto key = shared ? "shared" : "thread" + std::to_string(i);
                    const auto warm_until = Clock::now() + warmup;
                    do {
                        for (int j = 0; j < 1024; ++j) limiter.isAllowed(key);
                    } while (Clock::now() < warm_until);
                    ++ready;
                    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                    size_t total = 0, allowed = 0;
                    do {
                        for (int j = 0; j < 1024; ++j) allowed += limiter.isAllowed(key);
                        total += 1024;
                    } while (Clock::now() < until);
                    operations[i] = total;
                    checksums[i] = allowed;
                });
                while (ready.load() != count) std::this_thread::yield();
                const auto begin = Clock::now();
                until = begin + measurement;
                start.store(true, std::memory_order_release);
                for (auto &thread : threads) thread.join();
                const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - begin).count();
                size_t total = 0, checksum = 0;
                for (int i = 0; i < count; ++i) { total += operations[i]; checksum += checksums[i]; }
                if (checksum != total) throw std::runtime_error("Unexpected limiter rejection");
                std::cout << "limiter/" << (shared ? "same/" : "different/") << count << ','
                          << repeat + 1 << ',' << total << ',' << elapsed / total << ',' << checksum << '\n';
            }
        }
    }
}
} // namespace

int main(int argc, char **argv) {
    const std::string filter = argc > 1 ? argv[1] : "all";
    if (argc > 2 || (filter != "all" && filter != "cache" && filter != "pipeline" && filter != "encode" && filter != "limiter")) {
        std::cerr << "usage: zhttp_core_benchmark [all|cache|pipeline|encode|limiter]\n";
        return 2;
    }
    try {
        std::cout << "case,repeat,operations,ns_per_op,checksum\n";
        if (filter == "all" || filter == "cache") cache_benchmarks();
        if (filter == "all" || filter == "pipeline") pipeline_benchmarks();
        if (filter == "all" || filter == "encode") {
            HttpResponse response;
            response.text("OK").header("Server", "zhttp-bench");
            HttpRequestLine request;
            request.method = HttpMethod::GET;
            measure("encode", [&] { return ResponseEncoder::headers(request, response).size(); });
        }
        if (filter == "all" || filter == "limiter") limiter_benchmarks();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
