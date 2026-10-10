#include "io/wait_registry.h"
#include "support/fake_reactor.h"
#include "support/runtime_fixture.h"
#include <sys/socket.h>
using namespace zco;

TEST(IoWaitRegistry, MovedRegistrationRevokesItsWaitAndIgnoresStaleEvents) {
    auto reactor = std::make_shared<test::FakeReactor>();
    detail::IoWaitRegistry registry(reactor);
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    auto resource = descriptor.resource();
    auto old_wait = std::make_shared<detail::WaitState>(WaitId{TaskId{1}, 1});
    detail::RegistrationId stale;
    {
        auto result = registry.add(old_wait, resource, io::Interest::read);
        ASSERT_TRUE(result);
        auto registration = std::move(result).value();
        stale = reactor->await_registration();
        EXPECT_EQ(reactor->registration_count(), 1u);
    }
    EXPECT_EQ(reactor->registration_count(), 0u);
    EXPECT_TRUE(resource->registrations.empty());
    registry.dispatch({stale, io::Interest::read});
    EXPECT_FALSE(old_wait->completed());

    auto next_wait = std::make_shared<detail::WaitState>(WaitId{TaskId{1}, 2});
    auto next = registry.add(next_wait, resource, io::Interest::read);
    ASSERT_TRUE(next);
    auto fresh = reactor->await_registration();
    ASSERT_NE(stale, fresh);
    registry.dispatch({stale, io::Interest::read});
    EXPECT_FALSE(next_wait->completed());
    registry.dispatch({fresh, io::Interest::read});
    EXPECT_TRUE(next_wait->completed());
    EXPECT_EQ(next_wait->outcome(), WaitOutcome::ready);
    EXPECT_TRUE(next.value().finish());
    EXPECT_TRUE(next.value().finish());
    EXPECT_EQ(reactor->registration_count(), 0u);
    EXPECT_TRUE(resource->registrations.empty());
}

TEST(IoWaitRegistry, FailedRegistrationLeavesResourceReusable) {
    auto reactor = std::make_shared<test::FakeReactor>();
    detail::IoWaitRegistry registry(reactor);
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    auto resource = descriptor.resource();
    auto wait = std::make_shared<detail::WaitState>(WaitId{TaskId{1}, 1});
    reactor->fail_add = true;
    auto failed = registry.add(wait, resource, io::Interest::read);
    EXPECT_EQ(failed.error(), std::make_error_code(std::errc::io_error));
    EXPECT_EQ(reactor->registration_count(), 0u);
    EXPECT_TRUE(resource->registrations.empty());
    reactor->fail_add = false;
    auto next = registry.add(wait, resource, io::Interest::read);
    ASSERT_TRUE(next);
    EXPECT_TRUE(next.value().finish());
    EXPECT_EQ(reactor->registration_count(), 0u);
    EXPECT_TRUE(resource->registrations.empty());
}

TEST(IoWaitRegistry, ExceptionAfterRegistrationRevokesResourceAndReactorState) {
    auto reactor = std::make_shared<test::FakeReactor>();
    detail::IoWaitRegistry registry(reactor);
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    auto resource = descriptor.resource();
    auto wait = std::make_shared<detail::WaitState>(WaitId{TaskId{1}, 1});
    EXPECT_THROW(
        {
            auto registration =
                registry.add(wait, resource, io::Interest::read);
            ASSERT_TRUE(registration);
            EXPECT_EQ(reactor->registration_count(), 1u);
            throw std::runtime_error("failure after registration");
        },
        std::runtime_error);
    EXPECT_EQ(reactor->registration_count(), 0u);
    EXPECT_TRUE(resource->registrations.empty());
    auto next = registry.add(wait, resource, io::Interest::read);
    EXPECT_TRUE(next);
}
