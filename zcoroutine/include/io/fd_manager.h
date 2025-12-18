#ifndef ZCOROUTINE_FD_MANAGER_H
#define ZCOROUTINE_FD_MANAGER_H

#include <memory>
#include <vector>
#include "io/fd_context.h"
#include "sync/rwmutex.h"

namespace zcoroutine {

/**
 * @brief 文件描述符管理器
 * 
 * 统一管理所有文件描述符的上下文，支持动态扩容。
 * 线程安全，使用读写锁保护。
 */
class FdManager {
public:
    using ptr = std::shared_ptr<FdManager>;
    
    FdManager();
    ~FdManager() = default;
    
    /**
     * @brief 获取文件描述符上下文
     * @param fd 文件描述符
     * @param auto_create 如果不存在是否自动创建
     * @return 文件描述符上下文指针，不存在且不自动创建则返回nullptr
     */
    FdContext::ptr get(int fd, bool auto_create = false);
    
    /**
     * @brief 删除文件描述符上下文
     * @param fd 文件描述符
     */
    void del(int fd);
    
    /**
     * @brief 获取单例
     */
    static FdManager::ptr GetInstance();

private:
    std::vector<FdContext::ptr> fd_contexts_;  // 文件描述符上下文数组
    RWMutex mutex_;                             // 读写锁
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_FD_MANAGER_H
