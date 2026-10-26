#ifndef ZLYNX_ZCO_SRC_IO_WAIT_REGISTRY_H_
#define ZLYNX_ZCO_SRC_IO_WAIT_REGISTRY_H_

#include "io/resource.h"
#include <unordered_map>

namespace zco {
namespace detail {
/**
 * @brief 将反应器登记 ID 映射到等待状态，并在事件就绪时尝试完成等待。
 *
 * 仅由所属工作线程访问；注册表必须存活至全部 Registration 失效，
 * 因为登记作用域借用该注册表。
 */
class IoWaitRegistry {
  public:
    /**
     * @brief RAII 持有资源控制状态的登记作用域；移动转移解除责任，析构时解除登记。
     *
     * 析构忽略解除登记的错误；需要获知失败时应显式调用 finish()。
     */
    class Registration {
      public:
        Registration(Registration &&) noexcept;
        Registration(const Registration &) = delete;
        Registration &operator=(const Registration &) = delete;
        ~Registration();

        /**
         * @brief 解除反应器登记并从等待映射移除；重复调用成功且无操作。
         * @return 解除登记的结果；返回失败也不恢复本作用域的登记。
         */
        Result<void> finish();

      private:
        friend class IoWaitRegistry;
        Registration(IoWaitRegistry &, std::shared_ptr<io::detail::Resource>,
                     std::shared_ptr<io::detail::Registration>);
        IoWaitRegistry *owner_;
        std::shared_ptr<io::detail::Resource> resource_; // 资源控制状态；用于撤销登记
        std::shared_ptr<io::detail::Registration> registration_; // 反应器登记；用于撤销登记
    };

    explicit IoWaitRegistry(std::shared_ptr<Reactor> reactor)
        : reactor_(std::move(reactor)) {}

    IoWaitRegistry(const IoWaitRegistry &) = delete;
    IoWaitRegistry &operator=(const IoWaitRegistry &) = delete;

    Result<Registration> add(const std::shared_ptr<WaitState> &,
                             const std::shared_ptr<io::detail::Resource> &,
                             io::Interest);
    void dispatch(ReadyEvent);

  private:
    std::shared_ptr<Reactor> reactor_;
    std::unordered_map<RegistrationId, std::weak_ptr<WaitState>> waits_;
};
} // namespace detail
} // namespace zco

#endif
