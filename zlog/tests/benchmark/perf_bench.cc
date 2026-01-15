/**
 * @brief zlog性能分析程序
 * 用于perf工具采集同步/异步日志器的性能数据
 *
 * 用法: ./zlog_perf_bench [options]
 *   -m <mode>      模式: sync/async/both (默认: both)
 *   -c <count>     日志条数 (默认: 1000000)
 *   -t <threads>   线程数 (默认: 4)
 *   -d <duration>  运行时长(秒)，0表示按条数运行 (默认: 0)
 *   -s <size>      日志消息大小(字节) (默认: 128)
 *   -o <output>    输出目录 (默认: perf_bench_logs)
 */
#include "logger.h"
#include "sink.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iomanip>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace zlog;

// 配置参数
struct Config {
  std::string mode;
  long long count;
  int threads;
  int duration;
  int msgSize;
  std::string outputDir;

  Config()
      : mode("both"), count(1000000), threads(4), duration(0), msgSize(128),
        outputDir("perf_bench_logs") {}
};

// 全局统计
struct Stats {
  std::atomic<long long> totalCount;
  std::atomic<long long> successCount;
  std::chrono::high_resolution_clock::time_point startTime;
  std::chrono::high_resolution_clock::time_point endTime;

  Stats() : totalCount(0), successCount(0) {}
};

// 生成指定大小的消息
std::string makeMessage(int size) {
  static const char charset[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::string msg;
  msg.reserve(size);
  for (int i = 0; i < size; i++) {
    msg += charset[i % (sizeof(charset) - 1)];
  }
  return msg;
}

// 确保目录存在
void ensureDir(const std::string &path) {
  struct stat st;
  memset(&st, 0, sizeof(st));
  if (stat(path.c_str(), &st) == -1) {
    mkdir(path.c_str(), 0755);
  }
}

// 打印配置
void printConfig(const Config &cfg) {
  std::cout << "========================================\n";
  std::cout << "zlog Performance Benchmark\n";
  std::cout << "========================================\n";
  std::cout << "Mode:        " << cfg.mode << "\n";
  std::cout << "Log Count:   " << cfg.count << "\n";
  std::cout << "Threads:     " << cfg.threads << "\n";
  std::cout << "Duration:    "
            << (cfg.duration > 0 ? std::to_string(cfg.duration) + "s" : "N/A")
            << "\n";
  std::cout << "Msg Size:    " << cfg.msgSize << " bytes\n";
  std::cout << "Output Dir:  " << cfg.outputDir << "\n";
  std::cout << "========================================\n\n";
}

// 打印统计结果
void printStats(const std::string &name, const Stats &stats, int msgSize) {
  long long duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                           stats.endTime - stats.startTime)
                           .count();

  double seconds = duration / 1000.0;
  double throughputMsg = stats.successCount.load() / seconds;
  double throughputKB =
      (stats.successCount.load() * msgSize) / seconds / 1024.0;
  double throughputMB = throughputKB / 1024.0;

  std::cout << "\n--- " << name << " Results ---\n";
  std::cout << "Total Messages: " << stats.successCount.load() << "\n";
  std::cout << "Duration:       " << std::fixed << std::setprecision(2)
            << seconds << "s\n";
  std::cout << "Throughput:     " << std::fixed << std::setprecision(0)
            << throughputMsg << " msg/s";

  if (throughputMB >= 1.0) {
    std::cout << " (" << std::fixed << std::setprecision(2) << throughputMB
              << " MB/s)";
  } else {
    std::cout << " (" << std::fixed << std::setprecision(2) << throughputKB
              << " KB/s)";
  }
  std::cout << "\n";

  std::cout << "Latency (avg):  " << std::fixed << std::setprecision(3)
            << (1000000.0 / throughputMsg) << " us/msg\n";
}

// 同步日志基准测试
void runSyncBenchmark(const Config &cfg) {
  std::cout << "\n[1] Running Sync Logger Benchmark...\n";

  ensureDir(cfg.outputDir);
  std::string logFile = cfg.outputDir + "/sync_bench.log";

  // 创建同步日志器 (同步模式需要autoFlush=true保证持久性)
  Formatter::ptr formatter =
      std::make_shared<Formatter>("[%d{%H:%M:%S}][%t][%p] %m%n");
  std::vector<LogSink::ptr> sinks;
  sinks.push_back(std::make_shared<FileSink>(logFile, true)); // autoFlush=true

  std::shared_ptr<SyncLogger> logger = std::make_shared<SyncLogger>(
      "sync_bench", LogLevel::value::INFO, formatter, sinks);

  std::string msg = makeMessage(cfg.msgSize);
  Stats stats;
  std::atomic<bool> running(true);

  // 启动
  stats.startTime = std::chrono::high_resolution_clock::now();

  std::vector<std::thread> threads;
  long long countPerThread = cfg.count / cfg.threads;

  for (int t = 0; t < cfg.threads; t++) {
    threads.push_back(std::thread([&logger, &msg, &stats, &running,
                                   countPerThread]() {
      for (long long i = 0; i < countPerThread && running.load(); i++) {
        logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__, msg.c_str());
        stats.successCount++;
      }
    }));
  }

  // 如果指定了时长，等待超时后停止
  if (cfg.duration > 0) {
    std::this_thread::sleep_for(std::chrono::seconds(cfg.duration));
    running = false;
  }

  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }

  stats.endTime = std::chrono::high_resolution_clock::now();
  printStats("Sync Logger", stats, cfg.msgSize);
}

// 异步日志基准测试
void runAsyncBenchmark(const Config &cfg, AsyncType asyncType) {
  std::string typeName =
      (asyncType == AsyncType::ASYNC_SAFE) ? "Safe" : "Unsafe";
  std::cout << "\n[2] Running Async Logger Benchmark (" << typeName << ")...\n";

  ensureDir(cfg.outputDir);
  std::string logFile =
      cfg.outputDir + "/async_" +
      (asyncType == AsyncType::ASYNC_SAFE ? "safe" : "unsafe") + "_bench.log";

  Stats stats;

  {
    // 创建异步日志器
    Formatter::ptr formatter =
        std::make_shared<Formatter>("[%d{%H:%M:%S}][%t][%p] %m%n");
    std::vector<LogSink::ptr> sinks;
    sinks.push_back(std::make_shared<FileSink>(logFile));

    std::shared_ptr<AsyncLogger> logger = std::make_shared<AsyncLogger>(
        "async_bench", LogLevel::value::INFO, formatter, sinks, asyncType,
        std::chrono::milliseconds(100));

    std::string msg = makeMessage(cfg.msgSize);
    std::atomic<bool> running(true);

    // 启动
    stats.startTime = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    long long countPerThread = cfg.count / cfg.threads;

    for (int t = 0; t < cfg.threads; t++) {
      threads.push_back(
          std::thread([&logger, &msg, &stats, &running, countPerThread]() {
            for (long long i = 0; i < countPerThread && running.load(); i++) {
              logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                              msg.c_str());
              stats.successCount++;
            }
          }));
    }

    // 如果指定了时长，等待超时后停止
    if (cfg.duration > 0) {
      std::this_thread::sleep_for(std::chrono::seconds(cfg.duration));
      running = false;
    }

    for (size_t i = 0; i < threads.size(); ++i) {
      threads[i].join();
    }

    stats.endTime = std::chrono::high_resolution_clock::now();

    // 等待异步日志刷新完成
    std::cout << "Waiting for async flush...\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  printStats("Async Logger (" + typeName + ")", stats, cfg.msgSize);
}

// 解析命令行参数
Config parseArgs(int argc, char *argv[]) {
  Config cfg;
  int opt;

  while ((opt = getopt(argc, argv, "m:c:t:d:s:o:h")) != -1) {
    switch (opt) {
    case 'm':
      cfg.mode = optarg;
      break;
    case 'c':
      cfg.count = std::atoll(optarg);
      break;
    case 't':
      cfg.threads = std::atoi(optarg);
      break;
    case 'd':
      cfg.duration = std::atoi(optarg);
      break;
    case 's':
      cfg.msgSize = std::atoi(optarg);
      break;
    case 'o':
      cfg.outputDir = optarg;
      break;
    case 'h':
    default:
      std::cout
          << "Usage: " << argv[0] << " [options]\n"
          << "  -m <mode>      Mode: sync/async/both (default: both)\n"
          << "  -c <count>     Log count (default: 1000000)\n"
          << "  -t <threads>   Thread count (default: 4)\n"
          << "  -d <duration>  Duration in seconds, 0 for count-based "
             "(default: 0)\n"
          << "  -s <size>      Message size in bytes (default: 128)\n"
          << "  -o <output>    Output directory (default: perf_bench_logs)\n"
          << "  -h             Show this help\n";
      exit(0);
    }
  }

  return cfg;
}

int main(int argc, char *argv[]) {
  Config cfg = parseArgs(argc, argv);
  printConfig(cfg);

  ensureDir(cfg.outputDir);

  if (cfg.mode == "sync" || cfg.mode == "both") {
    runSyncBenchmark(cfg);
  }

  if (cfg.mode == "async" || cfg.mode == "both") {
    // // 测试Safe模式
    // runAsyncBenchmark(cfg, AsyncType::ASYNC_SAFE);

    // // 短暂休息
    // std::this_thread::sleep_for(std::chrono::seconds(2));

    // 测试Unsafe模式
    runAsyncBenchmark(cfg, AsyncType::ASYNC_UNSAFE);
  }

  std::cout << "\n========================================\n";
  std::cout << "Benchmark Complete!\n";
  std::cout << "Log files saved to: " << cfg.outputDir << "\n";
  std::cout << "========================================\n";

  return 0;
}
