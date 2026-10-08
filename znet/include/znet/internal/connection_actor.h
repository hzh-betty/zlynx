#ifndef ZNET_INTERNAL_CONNECTION_ACTOR_H_
#define ZNET_INTERNAL_CONNECTION_ACTOR_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>

#include "zco/coroutine.h"
#include "zco/sync/event.h"

namespace znet {
namespace detail {

enum class ConnectionEventType : uint8_t {
    kRead,
    kSend,
    kFlush,
    kShutdown,
    kClose
};

struct ConnectionEvent {
    explicit ConnectionEvent(ConnectionEventType t)
        : type(t), completion(true, false) {}
    ConnectionEventType type;
    size_t max_read_bytes = 0;
    uint32_t timeout_ms = 0;
    std::string payload;
    ssize_t result = 0;
    int error = 0;
    std::exception_ptr exception;
    zco::Event completion;
};

// 在同一调度器上串行执行命令，具体传输逻辑由注入的处理函数负责。
class ConnectionActor {
  public:
    using EventPtr = std::shared_ptr<ConnectionEvent>;
    using Handler = std::function<void(const EventPtr &)>;
    using KeepAlive = std::function<std::shared_ptr<void>()>;

    ConnectionActor(zco::Executor scheduler, Handler handler,
                    KeepAlive keep_alive);
    ssize_t dispatch(const EventPtr &event);
    bool try_begin_inline();
    void finish_inline();
    int scheduler_id() const { return sched_id_; }

  private:
    void drain();
    void cancel_pending(uint64_t generation);
    Handler handler_;
    KeepAlive keep_alive_;
    std::mutex mutex_;
    std::deque<EventPtr> mailbox_;
    bool running_ = false;
    uint64_t generation_ = 0;
    zco::TaskId coroutine_{};
    zco::Executor scheduler_;
    int sched_id_ = -1;
};

} // 命名空间 detail
} // 命名空间 znet

#endif
