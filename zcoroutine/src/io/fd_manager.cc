#include "io/fd_manager.h"
#include "zcoroutine_logger.h"

namespace zcoroutine {

FdManager::FdManager() {
    // 预分配一定数量的空间
    fd_contexts_.resize(64);
    ZCOROUTINE_LOG_DEBUG("FdManager initialized with capacity={}", fd_contexts_.size());
}

FdContext::ptr FdManager::get(int fd, bool auto_create) {
    if (fd < 0) {
        ZCOROUTINE_LOG_WARN("FdManager::get invalid fd={}", fd);
        return nullptr;
    }
    
    // 读锁
    {
        RWMutex::ReadLock lock(mutex_);
        if (static_cast<size_t>(fd) < fd_contexts_.size()) {
            if (fd_contexts_[fd] || !auto_create) {
                return fd_contexts_[fd];
            }
        } else if (!auto_create) {
            return nullptr;
        }
    }
    
    // 写锁
    RWMutex::WriteLock lock(mutex_);
    
    // 扩容
    if (static_cast<size_t>(fd) >= fd_contexts_.size()) {
        size_t old_size = fd_contexts_.size();
        size_t new_size = static_cast<size_t>(fd * 1.5);
        fd_contexts_.resize(new_size);
        ZCOROUTINE_LOG_INFO("FdManager::get resize fd_contexts: old_size={}, new_size={}", old_size, new_size);
    }
    
    // 创建新的上下文
    if (!fd_contexts_[fd]) {
        fd_contexts_[fd] = std::make_shared<FdContext>(fd);
        ZCOROUTINE_LOG_DEBUG("FdManager::get created new FdContext for fd={}", fd);
    }
    
    return fd_contexts_[fd];
}

void FdManager::del(int fd) {
    if (fd < 0) {
        ZCOROUTINE_LOG_WARN("FdManager::del invalid fd={}", fd);
        return;
    }
    
    RWMutex::WriteLock lock(mutex_);
    
    if (static_cast<size_t>(fd) < fd_contexts_.size()) {
        if (fd_contexts_[fd]) {
            fd_contexts_[fd].reset();
            ZCOROUTINE_LOG_DEBUG("FdManager::del removed FdContext for fd={}", fd);
        }
    }
}

FdManager::ptr FdManager::GetInstance() {
    static FdManager::ptr instance = std::make_shared<FdManager>();
    return instance;
}

}  // namespace zcoroutine
