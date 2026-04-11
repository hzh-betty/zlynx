#ifndef ZCO_TESTS_SUPPORT_INTERNAL_FIBER_TEST_HELPER_H_
#define ZCO_TESTS_SUPPORT_INTERNAL_FIBER_TEST_HELPER_H_

#include <memory>
#include <utility>

#include "zco/internal/fiber.h"
#include "zco/internal/processor.h"

namespace zco {
namespace test {

inline Fiber::ptr MakeFiberForTest(Processor *owner, int id, size_t stack_slot,
                                   Task task = Task(),
                                   bool use_shared_stack = true,
                                   size_t stack_size = 64 * 1024) {
    if (!task) {
        task = []() {};
    }

    const size_t effective_stack_size =
        use_shared_stack ? owner->shared_stack_size(stack_slot) : stack_size;

    return std::make_shared<Fiber>(id, owner, std::move(task),
                                   effective_stack_size, stack_slot,
                                   use_shared_stack);
}

} // namespace test
} // namespace zco

#endif // ZCO_TESTS_SUPPORT_INTERNAL_FIBER_TEST_HELPER_H_
