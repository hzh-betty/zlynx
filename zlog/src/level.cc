#include "level.h"

namespace zlog {

std::string LogLevel::toString(const LogLevel::value level) {
    switch (level) {
    case LogLevel::value::DEBUG:
        return "DEBUG";
    case LogLevel::value::INFO:
        return "INFO";
    case LogLevel::value::WARNING:
        return "WARNING";
    case LogLevel::value::ERROR:
        return "ERROR";
    case LogLevel::value::FATAL:
        return "FATAL";
    case LogLevel::value::OFF:
        return "OFF";
    case LogLevel::value::UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

} // namespace zlog
