#ifndef ZLYNX_ZCO_INCLUDE_ZCO_IO_DESCRIPTOR_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_IO_DESCRIPTOR_H_

#include "zco/result.h"
#include <cstdint>
#include <memory>
#include <mutex>

namespace zco {
namespace io {
namespace detail {
struct Resource;
}

struct ResourceId {
    uint64_t value = 0;
};

/**
 * @brief 独占文件描述符，并管理其未完成的 I/O 等待。
 *  
 * @note 原生句柄只供借用，不得由调用方另行关闭。
 */
class Descriptor {
  public:

    /**
     * @brief 在借用期间持有资源锁，防止原生调用期间描述符被关闭。
     * @note 借用期间不得再次调用需要同一资源锁的操作。
     * 原生调用不得改变此描述符及其所有别名的 O_NONBLOCK。
     */
    class Borrow {
      public:
        explicit Borrow(std::shared_ptr<detail::Resource> resource);
        int fd() const;

      private:
        std::shared_ptr<detail::Resource> resource_;
        std::unique_lock<std::mutex> lock_;
    };

    /**
     * @brief 在借用期间持有资源锁，防止原生调用期间描述符被关闭。
     * @note 借用期间不得再次调用需要同一资源锁的操作。
     * 原生调用不得改变此描述符及其所有别名的 O_NONBLOCK。
     */
    Borrow borrow() const { return Borrow(resource()); }

    Descriptor() = default;

    /**
     * @brief 接管 fd 的独占所有权，初始化失败也会关闭非负 fd。
     * @throws std::invalid_argument fd 为负值。
     * @note 构造时记录 O_NONBLOCK；协程 I/O 要求非阻塞模式。直到关闭前，
     * 该标志必须保持不变，包括共享同一打开文件描述的所有别名。
     */
    explicit Descriptor(int fd);
    ~Descriptor();
    Descriptor(Descriptor &&) noexcept;
    Descriptor &operator=(Descriptor &&) noexcept;
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;

    /**
     * @brief 返回借用的原生句柄；空对象或已关闭时返回 -1。
     * @note 返回值不延长资源生命周期；可能并发关闭时应使用 borrow()。
     * 不得关闭句柄、通过 dup2/dup3 覆盖它或通过任意别名改变 O_NONBLOCK。
     */
    int native_handle() const;
    ResourceId id() const;
    Result<void> close();
    Result<Descriptor> duplicate() const;

    /**
     * @brief 复制 source 后关闭旧资源，撤销旧等待，再接管副本。
     * @return 复制或关闭失败时返回错误；复制失败保留旧资源，
     * 关闭失败则旧资源已进入关闭状态，不接管副本。
     */
    Result<void> replace_with_duplicate(const Descriptor &source);

    std::shared_ptr<detail::Resource> resource() const {
        return std::atomic_load(&resource_);
    }

  private:
    std::shared_ptr<detail::Resource> resource_;
};
} // namespace io
} // namespace zco

#endif
