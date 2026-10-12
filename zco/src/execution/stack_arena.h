#ifndef ZLYNX_ZCO_SRC_EXECUTION_STACK_ARENA_H_
#define ZLYNX_ZCO_SRC_EXECUTION_STACK_ARENA_H_

#include "zco/runtime.h"
#include <memory>
#include <vector>

namespace zco {
namespace detail {
/**
 * @brief 独占协程栈映射，可读写区域两端各保留一个不可访问的保护页。
 *
 * 请求大小向上按系统页对齐；data() 不含保护页，析构释放整段映射。
 */
class StackBuffer {
  public:
    explicit StackBuffer(size_t size);
    ~StackBuffer();
    StackBuffer(const StackBuffer &) = delete;
    StackBuffer &operator=(const StackBuffer &) = delete;

    char *data() const { return data_; }

    size_t size() const { return size_; }

  private:
    void *mapping_;
    char *data_;
    size_t size_, mapping_size_;
};

/**
 * @brief 工作线程独享的栈分配器，支持轮换共享栈和独立栈复用。
 *
 * 共享槽位由 Arena 持有；独立栈交给 Continuation，结束后最多保留一个
 * 空闲独立栈。Arena 必须晚于全部借用它的 Continuation 销毁。
 */
class StackArena {
  public:
    explicit StackArena(const RuntimeOptions &options);

    /**
     * @brief 轮换返回一个共享槽位，可能复用其他挂起任务先前使用的栈。
     * @pre 当前配置为共享栈；复用前须保存仍需恢复的任务栈内容。
     */
    StackBuffer &shared_slot();

    /**
     * @brief 取得缓存栈或创建独立栈，将所有权交给调用方。
     */
    std::unique_ptr<StackBuffer> acquire_independent();
    void release_independent(std::unique_ptr<StackBuffer> buffer) noexcept;

    size_t stack_size() const { return size_; }

    bool shared() const { return shared_; }

  private:
    size_t size_, next_ = 0;
    bool shared_;
    std::vector<std::unique_ptr<StackBuffer>> slots_;

    // 每个工作线程保留一个已完成任务的栈，减少连续任务的 mmap/munmap 次数。
    // 缓存只保留栈内存，不保留上下文、回调、任务身份或等待状态。
    std::unique_ptr<StackBuffer> cached_independent_;
};
} // namespace detail
} // namespace zco

#endif
