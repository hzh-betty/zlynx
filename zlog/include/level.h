#ifndef ZLOG_LEVEL_H_
#define ZLOG_LEVEL_H_
#include <string>

/**
 * @brief 日志等级模块
 * 定义日志等级枚举和相关转换接口
 */

namespace zlog {
/**
 * @brief 日志级别类
 */
class LogLevel {
public:
  /**
   * @brief 日志级别枚举
   */
  enum class value : int {
    UNKNOWN = 0,
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL,
    OFF,
  };

  /**
   * @brief 将日志级别枚举转换为字符串
   * @param level 日志级别
   * @return std::string 对应的字符串
   */
  static std::string toString(LogLevel::value level);
};

inline bool operator<(LogLevel::value lhs, LogLevel::value rhs) {
  return static_cast<int>(lhs) < static_cast<int>(rhs);
}

inline bool operator<=(LogLevel::value lhs, LogLevel::value rhs) {
  return static_cast<int>(lhs) <= static_cast<int>(rhs);
}

inline bool operator>(LogLevel::value lhs, LogLevel::value rhs) {
  return static_cast<int>(lhs) > static_cast<int>(rhs);
}

inline bool operator>=(LogLevel::value lhs, LogLevel::value rhs) {
  return static_cast<int>(lhs) >= static_cast<int>(rhs);
}
} // namespace zlog

#endif // ZLOG_LEVEL_H_
