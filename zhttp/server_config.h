/**
 * server_config.h
 * server_config 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_SERVER_CONFIG_H_
#define ZHTTP_SERVER_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "zco/sched.h"

namespace zhttp {

/**
 * HTTP 服务器配置
 *
 * 该结构汇总了服务器运行时常见的所有可调参数，主要作为 Builder 和
 * 配置加载流程之间的内部承载对象。
 */
struct ServerConfig {
    // 网络配置。
    /** 监听地址，默认绑定所有 IPv4 接口。 */
    std::string host = "0.0.0.0";
    /** 监听端口，validate() 要求非零。 */
    uint16_t port = 8080;

    // 线程与协程配置。
    /** IO 线程数，validate() 要求大于零。 */
    size_t num_threads = 4;
    /** 协程栈模式，默认独立栈。 */
    zco::StackModel stack_mode = zco::StackModel::kIndependent;

    // SSL/TLS 配置。
    /** 是否在当前监听器上启用 TLS。 */
    bool enable_https = false;
    /** PEM 证书文件路径，启用 HTTPS 时必填。 */
    std::string cert_file;
    /** PEM 私钥文件路径，启用 HTTPS 时必填。 */
    std::string key_file;

    // 服务器行为配置。
    /** Server 响应头的字段值。 */
    std::string server_name = "zhttp/1.0";
    /** 可选的首页跳转目标，空串表示关闭。 */
    std::string homepage;
    /** 是否以守护进程模式运行。 */
    bool daemon = false;

    // 日志配置。
    /** 日志等级字符串，默认 info。 */
    std::string log_level = "info";

    // 超时配置，单位毫秒。
    /** 网络读取超时，单位毫秒；0 关闭。 */
    uint64_t read_timeout = 30000;
    /** 网络写出超时，单位毫秒；0 关闭。 */
    uint64_t write_timeout = 30000;
    /** 保持连接空闲超时，单位毫秒；0 关闭。 */
    uint64_t keepalive_timeout = 60000;

    /**
     * 从 TOML 文件加载配置
     *
     * @param filepath TOML 配置文件路径
     * @return 加载的配置
     * @throws std::runtime_error 如果文件不存在或解析失败
     */
    static ServerConfig from_toml(const std::string &filepath);

    /**
     * 验证配置有效性
     *
     * @return true 表示配置组合可用于启动服务
     */
    bool validate() const;
};

/**
 * 将字符串转换为 zco::StackModel
 *
 * @param str 字符串形式的栈模式
 * @return 对应的枚举值
 */
zco::StackModel string_to_stack_mode(const std::string &str);

/**
 * 将 zco::StackModel 转换为字符串
 *
 * @param mode 栈模式枚举
 * @return 字符串形式的模式名
 */
std::string stack_mode_to_string(zco::StackModel mode);

} // namespace zhttp

#endif // ZHTTP_SERVER_CONFIG_H_
