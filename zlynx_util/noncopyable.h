#ifndef ZLYNX_NONCOPYABLE_H_
#define ZLYNX_NONCOPYABLE_H_
/**
 * @brief 非拷贝类
 * 通过继承该类，可以禁止派生类的拷贝构造和赋值操作
 */
namespace zlynx
{
    class NonCopyable
    {
    public:
        NonCopyable() = default;
        ~NonCopyable() = default;

        NonCopyable(const NonCopyable&) = delete;
        NonCopyable& operator=(const NonCopyable&) = delete;
    };
}// namespace zlynx
#endif //ZLYNX_NONCOPYABLE_H_
