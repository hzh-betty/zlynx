#ifndef ZCO_POOL_H_
#define ZCO_POOL_H_

#include <cstddef>
#include <functional>
#include <memory>

namespace zco {

/**
 * @brief 协程友好的对象池。
 * @details 逻辑上按线程分桶存储，适合连接复用等场景。
 */
class Pool {
  public:
    /**
     * @brief 对象创建回调类型。
     */
    using CreateCallback = std::function<void *()>;

    /**
     * @brief 对象销毁回调类型。
     */
    using DestroyCallback = std::function<void(void *)>;

    /**
     * @brief 构造对象池。
     * @param create_cb 创建回调。
     * @param destroy_cb 销毁回调。
     * @param capacity 每线程桶容量上限。
     */
    explicit Pool(CreateCallback create_cb = nullptr,
                  DestroyCallback destroy_cb = nullptr,
                  size_t capacity = static_cast<size_t>(-1));

    /**
     * @brief 析构对象池并尝试回收元素。
     */
    ~Pool();

    /**
     * @brief 拷贝构造，内部共享同一池状态。
     * @param other 另一个对象池。
     */
    Pool(const Pool &other);

    /**
     * @brief 移动构造。
     * @param other 被移动对象。
     */
    Pool(Pool &&other) noexcept;

    /**
     * @brief 弹出一个对象。
     * @return 对象指针，若池为空且无 create 回调则返回 nullptr。
     */
    void *pop() const;

    /**
     * @brief 归还一个对象。
     * @param element 要归还的对象指针。
     * @return 无返回值。
     */
    void push(void *element) const;

    /**
     * @brief 获取当前线程桶中的对象数量。
     * @return 对象数量。
     */
    size_t size() const;

    /**
     * @brief 清空所有线程桶。
     * @return 无返回值。
     */
    void clear() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

/**
 * @brief Pool 弹出对象的 RAII 保护器。
 * @tparam T 对象类型。
 */
template <typename T> class PoolGuard {
  public:
    /**
     * @brief 构造时从池中弹出对象。
     * @param pool 对象池引用。
     */
    explicit PoolGuard(const Pool &pool)
        : pool_(pool), element_(static_cast<T *>(pool_.pop())) {}

    /**
     * @brief 析构时归还对象。
     */
    ~PoolGuard() { pool_.push(element_); }

    /**
     * @brief 指针访问运算符。
     * @return 对象指针。
     */
    T *operator->() const { return element_; }

    /**
     * @brief 解引用运算符。
     * @return 对象引用。
     */
    T &operator*() const { return *element_; }

    /**
     * @brief 布尔判断。
     * @return true 表示持有有效对象。
     */
    explicit operator bool() const { return element_ != nullptr; }

    /**
     * @brief 获取底层对象指针。
     * @return 对象指针。
     */
    T *get() const { return element_; }

  private:
    const Pool &pool_;
    T *element_;
};

} // namespace zco

#endif // ZCO_POOL_H_
