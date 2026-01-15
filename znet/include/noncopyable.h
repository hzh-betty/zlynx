#ifndef ZNET_NONCOPYABLE_H_
#define ZNET_NONCOPYABLE_H_

/**
 * @brief 非拷贝类基类
 * 通过继承该类，可以禁止派生类的拷贝构造和赋值操作
 * 使用C++11 delete关键字实现
 */
namespace znet {

class NonCopyable {
public:
  NonCopyable() = default;
  ~NonCopyable() = default;

  // 禁止拷贝构造
  NonCopyable(const NonCopyable &) = delete;
  // 禁止拷贝赋值
  NonCopyable &operator=(const NonCopyable &) = delete;
};

} // namespace znet

#endif // ZNET_NONCOPYABLE_H_
