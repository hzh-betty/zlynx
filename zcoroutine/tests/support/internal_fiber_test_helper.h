#ifndef ZCOROUTINE_TESTS_SUPPORT_INTERNAL_FIBER_TEST_HELPER_H_
#define ZCOROUTINE_TESTS_SUPPORT_INTERNAL_FIBER_TEST_HELPER_H_

#include <memory>
#include <utility>

#include "zcoroutine/internal/fiber.h"
#include "zcoroutine/internal/processor.h"

namespace zcoroutine {
namespace test {

inline std::shared_ptr<Fiber> MakeFiberForTest(Processor* owner,
                                               int id,
                                               size_t stack_slot,
                                               Task task = Task(),
                                               bool use_shared_stack = true,
                                               size_t stack_size = 64 * 1024) {
  if (!task) {
    task = []() {};
  }

  const size_t effective_stack_size =
      use_shared_stack ? owner->shared_stack_size(stack_slot) : stack_size;

  return std::make_shared<Fiber>(id, owner, std::move(task), effective_stack_size, stack_slot,
                                 use_shared_stack);
}

}  // namespace test
}  // namespace zcoroutine

#endif  // ZCOROUTINE_TESTS_SUPPORT_INTERNAL_FIBER_TEST_HELPER_H_
