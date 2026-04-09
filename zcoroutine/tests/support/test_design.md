# zcoroutine Test Design Baseline

## Requirement Clarification and Assumptions

- Input validity:
  - Invalid file descriptors should fail with errno consistent with POSIX semantics.
  - Null task submission should be ignored safely and not crash.
- Timeout behavior:
  - Coroutine timeout returns false and may set timeout state via timeout().
  - Hook I/O timeout may surface as ETIMEDOUT, EAGAIN, or EWOULDBLOCK depending on kernel path.
- Concurrency constraints:
  - Tests avoid multi-accept waiter on the same listen fd due to oneshot epoll semantics.
  - Synchronization uses WaitGroup/Event to reduce flaky sleep-based waiting.
- Overflow and bounds:
  - Queue-related tests cover capacity and reserve boundaries; no intentional integer overflow stress in quick suite.
- Complexity and runtime:
  - Quick suite target is under 1 minute on normal CI nodes.
  - Stress tests are tagged extended and excluded from quick defaults.

## Branch and Condition Coverage Matrix

| Module | Function/Behavior | Branches | Atomic Conditions |
|---|---|---|---|
| Runtime | ensure_started/init/shutdown/submit/submit_to | started true/false, null task path, modulo dispatch path | task==nullptr, scheduler_count==0, count<=1 |
| Runtime | pick_secondary_index | same index correction path | second==first |
| TimerQueue | next_timeout_ms/process_due | empty queue, due now, due later, cancelled token, null callback | timers.empty(), deadline<=now, token.cancelled, callback!=null |
| Event | wait/signal/notify_all | thread vs coroutine path, manual_reset vs auto_reset, timeout vs wake | in_coroutine(), manual_reset, milliseconds==infinite, signaled |
| StealQueue | steal/append/drain_all | null ptr/max_steal zero, reserve short-circuit, 3-size policy ranges | tasks!=nullptr, max_steal>0, total>=64, total>=8, total<=reserve |
| Hook | co_read/co_write/co_dup*/metadata sync | invalid fd path, explicit timeout vs inherited timeout, dup sync and close path | requested_timeout==infinite, fd<0 |
| IoEvent | wait | invalid fd, invalid event type, timeout/error path | fd<0, event_type valid |

## Minimal Test Set (Non-redundant)

| Case ID | Inputs | Expected | Coverage Point | Redundancy |
|---|---|---|---|---|
| RM-01 | go before init | task executes and runtime starts | ensure_started false->true | Non-redundant |
| RM-02 | submit null task | no crash, normal task still runs | submit null guard | Non-redundant |
| RM-03 | submit_to large index | task runs with valid scheduler id | modulo dispatch | Non-redundant |
| RM-04 | stale handle resume | stale handle does not wake new coroutine | resume_external missing handle | Non-redundant |
| TM-01 | empty queue | next_timeout_ms=1000 | empty branch | Non-redundant |
| TM-02 | due + cancelled + null callback | only active callback fires | cancelled/callback branches | Non-redundant |
| EV-01 | auto reset signal two waiters | exactly one wakes | auto reset branch | Non-redundant |
| EV-02 | manual reset notify_all | all wake | manual reset branch | Non-redundant |
| SQ-01 | size 5/30/100 steals | 1 / total/3 / 2*total/5 logic | range branches | Non-redundant |
| HK-01 | invalid fd read/write | -1 + EBADF | invalid fd branch | Non-redundant |
| HK-02 | explicit vs inherited timeout | explicit takes precedence; inherited works | timeout resolution branch | Non-redundant |
| IO-01 | invalid fd/type | false + errno | input validation branches | Non-redundant |

## Round 2 Minimal Additions (Composite Conditions + Concurrency Boundaries)

| Case ID | Inputs | Expected | Covered Branch/Atomic Conditions | Redundancy |
|---|---|---|---|---|
| EV-03 | thread wait(kInfiniteTimeoutMs) + delayed signal | wait unblocks and returns true | thread path + milliseconds==infinite true | Non-redundant (infinite branch only) |
| EV-04 | auto_reset + notify_all without waiters | next wait times out | notify_all + manual_reset=false path, signaled cache=false | Non-redundant |
| EV-05 | moved-from Event object | wait returns false; signal/reset/notify_all no crash | impl_==nullptr defensive branches | Non-redundant |
| RM-05 | runtime.init(1) then pick_processor_index loop | always choose index 0 | count<=1 branch in pick_processor_index | Non-redundant |
| RM-06 | runtime.resume_external(nullptr) | no-op and runtime remains usable | handle==nullptr branch | Non-redundant |
| RM-07 | sequential next_fiber_id calls | strict monotonic growth | id generator monotonic path | Non-redundant |
| TM-03 | near-future timer (40ms) | timeout in (0,1000] | next_timeout_ms delta branch | Non-redundant |
| TM-04 | process_due before/after expiry | first pass no callback, later pass callback once | process_due early return + due callback path | Non-redundant |
| PR-01 | wait_fd with EPOLLOUT | returns true quickly | fallback poll write-event mapping | Non-redundant |
| PR-02 | wait_fd invalid fd | false and errno unchanged | fallback poll path for negative fd | Non-redundant |
| HK-03 | user nonblocking fd + co_read no data | immediate -1 with EAGAIN/EWOULDBLOCK | user_nonblocking=true fast-fail branch | Non-redundant |
| HK-04 | user nonblocking listen socket + co_accept4 no client | immediate -1 with EAGAIN/EWOULDBLOCK | co_accept4 user_nonblocking=true branch | Non-redundant |
| HK-05 | co_dup/co_dup2/co_close with invalid fd | -1 with EBADF | dup/close retry loop error path | Non-redundant |
| IO-02 | IoEvent kWrite on socket | wait returns true | switch(kWrite) branch | Non-redundant |
| IO-03 | IoEvent read timeout in coroutine | wait false + timeout()==true + errno in timeout family | coroutine timeout path with !ok and timeout()==true | Non-redundant |

## Round 3 Additions (High-Concurrency + Randomized Deterministic Cases)

| Case ID | Inputs | Expected | Covered Branch/Atomic Conditions | Redundancy |
|---|---|---|---|---|
| ST-01 | 8 threads x 100000 tasks | executed=100000 and id sum matches arithmetic series | high-contention submit path, scheduler enqueue/dequeue under load | Non-redundant |
| ST-02 | randomized action mix (yield/sleep/direct) with fixed seeds | all submitted tasks complete exactly once | mixed execution path with deterministic random scheduling pressure | Non-redundant |
| RM-08 | randomized submit/submit_to/go mix, multi-thread fixed seeds | all tasks complete, cross-scheduler hits >= 2 | submit path combinations + modulo dispatch in random index domain | Non-redundant |
| HI-03 | randomized payload size/content round-trip with fixed seed | receiver payload equals sender payload each round | hook read/write repeated path under varied payload boundaries | Non-redundant |

## Round 4 Additions (Connect Failure Paths + Long-Run Concurrency Stability)

| Case ID | Inputs | Expected | Covered Branch/Atomic Conditions | Redundancy |
|---|---|---|---|---|
| ST-03 | 50 rounds, each round 8 threads x 120 tasks, fixed seeds | no task loss; cumulative counter严格等于期望 | long-run scheduler stability under repeated contention and mixed task actions | Non-redundant |
| HI-04 | 20 rounds randomized closed-port connect attempts | co_connect returns -1 with refusal/timeout family errno | co_connect immediate failure/timeout error branches | Non-redundant |
| HI-05 | 20 rounds randomized clients (2..8), single acceptor loop | accepted count equals total client attempts | co_accept4/co_connect repeated success paths under randomized concurrent arrivals | Non-redundant |

## Round 5 Additions (Independent Stack Mode Deepening)

| Case ID | Inputs | Expected | Covered Branch/Atomic Conditions | Redundancy |
|---|---|---|---|---|
| FB-IND-01 | Independent fiber with out-of-range stack_slot | construction/context init succeeds; stack_slot preserved | Fiber ctor independent branch (skip shared-stack validation) | Non-redundant |
| RM-IND-01 | Independent mode, 20 coroutines block concurrently and record local variable address | all addresses non-zero and pairwise distinct | per-fiber independent stack isolation under concurrent blocking | Non-redundant |
| ST-IND-01 | Independent mode, 8 threads x 100000 tasks, each task validates stack local array after yield | all tasks complete; corruption counter is 0 | independent stack stability under high contention + context switches | Non-redundant |

## Round 6 Additions (Residual Fiber Branches + Integration)

| Case ID | Inputs | Expected | Covered Branch/Atomic Conditions | Redundancy |
|---|---|---|---|---|
| FB-SH-01 | Shared fiber ctor with out-of-range stack_slot | throw runtime_error | Fiber ctor shared branch invalid-slot guard | Non-redundant |
| FB-SH-02 | Shared fiber initialize_context called twice | context_initialized stays true and no crash | initialize_context shared-path idempotent branch | Non-redundant |
| FB-SH-03 | Shared fiber save_stack_data(non-empty) then save_stack_data(size=0) | saved stack cleared | save_stack_data size==0 branch | Non-redundant |
| HI-IND-01 | Independent mode socketpair round-trip with interleaved yield and stack-local probes | sender/receiver rounds all pass; corruption counter is 0 | independent stack + Hook IO + yield integrated path | Non-redundant |

## Round 7 Additions (Sparse-Test Modules Strengthening)

| Case ID | Inputs | Expected | Covered Branch/Atomic Conditions | Redundancy |
|---|---|---|---|---|
| TM-05 | one due timer + one far-future timer | first process_due executes only due timer, second pass executes future timer | process_due early return with non-expired head while prior due path already consumed | Non-redundant |
| TM-06 | timer callback enqueues another immediate timer | single process_due drains both callbacks | process_due loop re-entrancy via callback-triggered add_timer | Non-redundant |
| TM-07 | 4 threads concurrently add zero-delay timers | process_due executes all callbacks exactly once | add_timer mutex/thread-safety path under contention | Non-redundant |
| SC-01 | coroutine captures sched_id/coroutine_id/current_coroutine/in_coroutine | ids valid, handle non-null, in_coroutine=true | sched API observable-state branches in coroutine context | Non-redundant |
| SC-02 | thread context calls yield/current_coroutine | no crash and still reports non-coroutine state | yield thread-path + current_coroutine null behavior | Non-redundant |

## Header-To-Unit Mapping (Current)

| Header | Unit Test |
|---|---|
| zcoroutine/channel.h | tests/unit/channel_unit.cc |
| zcoroutine/event.h | tests/unit/event_unit.cc |
| zcoroutine/hook.h | tests/unit/hook_unit.cc + tests/unit/hook_metadata_timeout_unit.cc |
| zcoroutine/io_event.h | tests/unit/io_event_unit.cc |
| zcoroutine/log.h | tests/unit/log_unit.cc |
| zcoroutine/mutex.h | tests/unit/mutex_unit.cc |
| zcoroutine/pool.h | tests/unit/pool_unit.cc |
| zcoroutine/sched.h | tests/unit/sched_unit.cc + tests/unit/runtime_manager_unit.cc |
| zcoroutine/wait_group.h | tests/unit/wait_group_unit.cc |
| zcoroutine/zcoroutine.h | tests/unit/zcoroutine_unit.cc |
| zcoroutine/internal/coroutine_waiter.h | tests/unit/coroutine_waiter_unit.cc |
| zcoroutine/internal/epoller.h | tests/unit/epoller_unit.cc |
| zcoroutine/internal/fiber.h | tests/unit/fiber_unit.cc |
| zcoroutine/internal/fiber_handle_registry.h | tests/unit/fiber_handle_registry_unit.cc |
| zcoroutine/internal/fiber_pool.h | tests/unit/fiber_pool_unit.cc |
| zcoroutine/internal/noncopyable.h | tests/unit/noncopyable_unit.cc |
| zcoroutine/internal/poller.h | tests/unit/poller_unit.cc |
| zcoroutine/internal/processor.h | tests/unit/processor_unit.cc + tests/unit/processor_wait_timer_unit.cc |
| zcoroutine/internal/runtime_manager.h | tests/unit/runtime_manager_unit.cc |
| zcoroutine/internal/shared_stack_buffer.h | tests/unit/shared_stack_buffer_unit.cc |
| zcoroutine/internal/snapshot_buffer_pool.h | tests/unit/snapshot_buffer_pool_unit.cc |
| zcoroutine/internal/steal_queue.h | tests/unit/steal_queue_unit.cc |
| zcoroutine/internal/timer.h | tests/unit/timer_unit.cc + tests/unit/timer_queue_unit.cc |

Exception by design:
context.h 被视为功能特别简单（仅 ucontext 薄封装），暂不强制独立 *_unit.cc；其行为通过 fiber/processor 路径间接覆盖。

## Coverage Workflow (zcoroutine/src only)

### Build and Test

```bash
# Configure with coverage (workspace root)
cmake -S . -B build-cov -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DENABLE_TESTS=ON \
  -DENABLE_COVERAGE=ON

# Build via CMake
cmake --build build-cov -j

# Run zcoroutine unit + integration only (exclude stress)
ctest --test-dir build-cov --output-on-failure -L quick
```

### Collect and Filter Report

```bash
# Collect coverage
lcov --capture --directory build-cov --output-file coverage.info

# Keep zcoroutine/src only
lcov --extract coverage.info '*/zcoroutine/src/*' --output-file zcoroutine-src.info

# Remove system and third-party noise
lcov --remove zcoroutine-src.info '/usr/*' '*/extern/*' --output-file zcoroutine-src.info

# Generate HTML report
genhtml zcoroutine-src.info --output-directory coverage-zcoroutine-src
```

### Notes

- Metrics scope is restricted to `zcoroutine/src`.
- Coverage loop should prioritize branch/condition hotspots before adding more broad tests.
- If lcov branch details are insufficient, run a secondary clang/llvm-cov report for cross-checking.
