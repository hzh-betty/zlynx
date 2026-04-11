#include <type_traits>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zcoroutine/internal/noncopyable.h"

namespace zcoroutine {
namespace {

class DummyNonCopyable : public NonCopyable {
  public:
    DummyNonCopyable() = default;
};

static_assert(!std::is_copy_constructible<DummyNonCopyable>::value,
              "DummyNonCopyable must not be copy constructible");
static_assert(!std::is_copy_assignable<DummyNonCopyable>::value,
              "DummyNonCopyable must not be copy assignable");

class NonCopyableUnitTest : public test::RuntimeTestBase {};

TEST_F(NonCopyableUnitTest, TypeTraitsMatchNonCopyableIntent) {
    EXPECT_FALSE((std::is_copy_constructible<DummyNonCopyable>::value));
    EXPECT_FALSE((std::is_copy_assignable<DummyNonCopyable>::value));
    EXPECT_TRUE((std::is_default_constructible<DummyNonCopyable>::value));
}

} // namespace
} // namespace zcoroutine
