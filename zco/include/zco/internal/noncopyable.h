#ifndef ZCO_INTERNAL_NONCOPYABLE_H_
#define ZCO_INTERNAL_NONCOPYABLE_H_

namespace zco {

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

} // namespace zco

#endif // ZCO_INTERNAL_NONCOPYABLE_H_
