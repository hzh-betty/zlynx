#ifndef ZCOROUTINE_SHARED_STACK_POOL_H_
#define ZCOROUTINE_SHARED_STACK_POOL_H_

#include <vector>
#include <atomic>
#include <cstddef>

namespace zcoroutine {

// 前向声明
class Fiber;

/**
 * @brief 共享栈内存结构
 * 参考libco的StackMem设计
 */
struct StackMem {
    Fiber* occupy_fiber = nullptr;  // 当前占用的协程
    size_t stack_size = 0;          // 栈大小
    char* stack_bp = nullptr;       // 栈顶指针（高地址）
    char* stack_buffer = nullptr;   // 栈底指针（低地址）
};

/**
 * @brief 共享栈池类
 * 管理多个共享栈，协程在切换时保存和恢复栈数据
 * 参考libco的copy stack模式
 */
class SharedStackPool {
public:
    /**
     * @brief 构造函数
     * @param count 共享栈数量
     * @param stack_size 单个栈的大小
     */
    SharedStackPool(int count, size_t stack_size);

    /**
     * @brief 析构函数
     */
    ~SharedStackPool();

    /**
     * @brief 分配一个共享栈
     * @return 共享栈指针
     * 采用轮询策略分配
     */
    StackMem* allocate_stack();

    /**
     * @brief 保存协程栈数据
     * @param fiber 协程指针
     * 在协程切出时调用，将栈数据拷贝到save_buffer
     */
    void save_stack(Fiber* fiber);

    /**
     * @brief 恢复协程栈数据
     * @param fiber 协程指针
     * 在协程切入时调用，将save_buffer数据拷贝回共享栈
     */
    void restore_stack(Fiber* fiber);

    /**
     * @brief 获取共享栈数量
     * @return 栈数量
     */
    int get_count() const { return count_; }

    /**
     * @brief 获取单个栈大小
     * @return 栈大小（字节）
     */
    size_t get_stack_size() const { return stack_size_; }

private:
    int count_;                             // 共享栈数量
    size_t stack_size_;                     // 单个栈大小
    std::vector<StackMem*> stack_array_;    // 栈内存数组
    std::atomic<unsigned int> alloc_idx_;   // 分配索引（轮询）
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SHARED_STACK_POOL_H_
