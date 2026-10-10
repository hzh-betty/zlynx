/**
 * @file logger_registry.h
 * @brief 可选的全局日志器注册接口。
 */

#ifndef ZLOG_LOGGER_REGISTRY_H_
#define ZLOG_LOGGER_REGISTRY_H_

#include <mutex>
#include <string>
#include <unordered_map>

#include "zlog/logger.h"

namespace zlog {
/**
 * @brief 全局日志器管理器
 * 负责管理所有日志器并提供全局访问接口
 */
class LoggerManager {
  public:
    /**
     * @brief 获取单例实例
     * @return 日志器管理器实例引用
     */
    static LoggerManager &get_instance() {
        static LoggerManager eton;
        return eton;
    }

    /**
     * @brief 添加日志器
     * @param logger 日志器智能指针
     * @throws std::invalid_argument 空指针或名称已经注册；不替换已有实例。
     */
    void add_logger(const Logger::ptr &logger);

    /**
     * @brief 获取指定名称的日志器
     * @param name 日志器名称
     * @return 日志器智能指针，不存在则返回空指针
     */
    Logger::ptr get_logger(const std::string &name);

    /**
     * @brief 获取根日志器
     * @return 根日志器智能指针
     */
    Logger::ptr root_logger();

  private:
    /**
     * @brief 私有构造函数
     * 初始化根日志器
     */
    LoggerManager();

  private:
    std::mutex mutex_;                                     // 互斥锁
    Logger::ptr root_logger_;                              // 默认根日志器
    std::unordered_map<std::string, Logger::ptr> loggers_; // 日志器映射表
};

} // namespace zlog

#endif // ZLOG_LOGGER_REGISTRY_H_
