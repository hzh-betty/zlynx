#ifndef ZCO_ZCO_H_
#define ZCO_ZCO_H_

#include "zco/channel.h"
#include "zco/event.h"
#include "zco/hook.h"
#include "zco/io_event.h"
#include "zco/zco_log.h"
#include "zco/mutex.h"
#include "zco/pool.h"
#include "zco/sched.h"
#include "zco/wait_group.h"

namespace zco {

/**
 * @brief 协程库主命名空间。
 * @details 包含核心 API 和常用类型别名，用户代码主要通过此头文件访问库功能
 */
template <typename T> using channel = Channel<T>;
using event = Event;
using wait_group = WaitGroup;
using pool = Pool;
using io_event = IoEvent;
using mutex = Mutex;
using mutex_guard = MutexGuard;

} // namespace zco
#endif // ZCO_ZCO_H_
