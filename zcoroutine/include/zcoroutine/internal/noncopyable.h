#ifndef ZCOROUTINE_INTERNAL_NONCOPYABLE_H_
#define ZCOROUTINE_INTERNAL_NONCOPYABLE_H_

namespace zcoroutine {

/**
 * @brief 禁止拷贝基类。
 * @details 继承该类后，派生类自动禁用拷贝构造与拷贝赋值。
 */
class NonCopyable {
  public:
    /**
     * @brief 构造函数。
     * @param 无参数。
     * @return 无返回值。
     */
    NonCopyable() = default;

    /**
     * @brief 析构函数。
     * @param 无参数。
     * @return 无返回值。
     */
    ~NonCopyable() = default;

    NonCopyable(const NonCopyable &) = delete;
    NonCopyable &operator=(const NonCopyable &) = delete;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_INTERNAL_NONCOPYABLE_H_
