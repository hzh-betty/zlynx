#ifndef ZCOROUTINE_ZCOROUTINE_H_
#define ZCOROUTINE_ZCOROUTINE_H_

#include "zcoroutine/channel.h"
#include "zcoroutine/event.h"
#include "zcoroutine/hook.h"
#include "zcoroutine/io_event.h"
#include "zcoroutine/log.h"
#include "zcoroutine/mutex.h"
#include "zcoroutine/pool.h"
#include "zcoroutine/sched.h"
#include "zcoroutine/wait_group.h"

namespace zcoroutine {

/**
* @brief 协程库主命名空间。
* @details 包含核心 API 和常用类型别名，用户代码主要通过此头文件访问库功能
*/
template <typename T>
using channel = Channel<T>;
using event = Event;
using wait_group = WaitGroup;
using pool = Pool;
using io_event = IoEvent;
using mutex = Mutex;
using mutex_guard = MutexGuard;

}  // namespace zcoroutine
namespace zco = ::zcoroutine;
#endif  // ZCOROUTINE_ZCOROUTINE_H_
