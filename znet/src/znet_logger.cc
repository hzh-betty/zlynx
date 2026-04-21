#include "znet/znet_logger.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <utility>

namespace znet {

namespace {

constexpr char kLoggerName[] = "znet_logger";
constexpr char kDefaultFormatter[] = "[%d{%H:%M:%S}][%c][%p]%T%m%n";
constexpr char kDefaultFilePath[] = "./logfile/znet.log";

zlog::Logger::ptr &cached_logger() {
    static zlog::Logger::ptr logger;
    return logger;
}

std::mutex &cached_logger_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::string normalize_sink(std::string sink) {
    std::transform(sink.begin(), sink.end(), sink.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return sink;
}

std::string resolve_log_file_path(const LoggerInitOptions &options) {
    if (!options.file_path.empty()) {
        return options.file_path;
    }
    return kDefaultFilePath;
}

void build_logger_sinks(zlog::GlobalLoggerBuilder &builder,
                        const LoggerInitOptions &options) {
    const std::string sink = normalize_sink(options.sink);
    if (sink == "file") {
        builder.build_logger_sink<zlog::FileSink>(
            resolve_log_file_path(options));
        return;
    }
    if (sink == "both" || sink == "stdout+file" || sink == "file+stdout") {
        builder.build_logger_sink<zlog::StdOutSink>();
        builder.build_logger_sink<zlog::FileSink>(
            resolve_log_file_path(options));
        return;
    }
    builder.build_logger_sink<zlog::StdOutSink>();
}

} // namespace

void init_logger_locked(const LoggerInitOptions &options) {
    zlog::GlobalLoggerBuilder builder;
    builder.build_logger_name(kLoggerName);
    builder.build_logger_level(options.level);
    builder.build_logger_type(options.async ? zlog::LoggerType::LOGGER_ASYNC
                                            : zlog::LoggerType::LOGGER_SYNC);
    builder.build_logger_formatter(
        options.formatter.empty() ? kDefaultFormatter : options.formatter);
    build_logger_sinks(builder, options);

    zlog::Logger::ptr logger = builder.build();
    zlog::LoggerManager::get_instance().upsert_logger(kLoggerName, logger);
    cached_logger() = std::move(logger);
}

void init_logger(const LoggerInitOptions &options) {
    std::lock_guard<std::mutex> lock(cached_logger_mutex());
    init_logger_locked(options);
}

void init_logger(zlog::LogLevel::value level) {
    LoggerInitOptions options;
    options.level = level;
    init_logger(options);
}

zlog::Logger::ptr get_logger_ptr() {
    std::lock_guard<std::mutex> lock(cached_logger_mutex());
    auto &logger = cached_logger();
    if (!logger) {
        LoggerInitOptions options;
        options.level = zlog::LogLevel::value::INFO;
        init_logger_locked(options);
    }
    return logger;
}

zlog::Logger *get_logger() { return get_logger_ptr().get(); }

} // namespace znet
