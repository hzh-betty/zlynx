#include "io/linux/epoll_reactor.h"
#include "io/resource.h"
#include "support/fake_reactor.h"
#include "support/runtime_fixture.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace zco;

TEST(Reactor, ConflictingCombinedRegistrationIsTransactional) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    detail::EpollReactor reactor;
    auto write = reactor.add(descriptor.id(), descriptor.native_handle(),
                             io::Interest::write);
    ASSERT_TRUE(write);
    auto both = reactor.add(descriptor.id(), descriptor.native_handle(),
                            io::Interest::read_write);
    EXPECT_FALSE(both);
    auto read = reactor.add(descriptor.id(), descriptor.native_handle(),
                            io::Interest::read);
    EXPECT_TRUE(read);
    reactor.remove(write.value());
    reactor.remove(read.value());
}

TEST(Reactor, KernelRegistrationFailureLeavesNoUserspaceEntry) {
    detail::EpollReactor reactor;
    int fd = ::open("/dev/null", O_RDONLY | O_NONBLOCK);
    ASSERT_GE(fd, 0);
    io::Descriptor descriptor(fd);
    auto failed = reactor.add(descriptor.id(), fd, io::Interest::read_write);
    EXPECT_FALSE(failed);
    auto repeated = reactor.add(descriptor.id(), fd, io::Interest::read);
    EXPECT_FALSE(repeated);
    EXPECT_NE(repeated.error(),
              std::make_error_code(std::errc::device_or_resource_busy));
}

TEST(Reactor, FakeRegistrationFailureAndStaleEventsAreIndependentOfFibers) {
    auto reactor = std::make_shared<test::FakeReactor>();
    reactor->fail_add = true;
    EXPECT_FALSE(reactor->add(io::ResourceId{1}, 4, io::Interest::read));
    reactor->fail_add = false;
    auto registration = reactor->add(io::ResourceId{2}, 4, io::Interest::read);
    ASSERT_TRUE(registration);
    reactor->remove(registration.value());
    reactor->emit(registration.value(), io::Interest::read);
    auto events = reactor->poll(0);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].registration, registration.value());
}

TEST(IO, BlockingDescriptorAndThreadWaitAreRejected) {
    int pair[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    io::Descriptor descriptor(pair[0]), peer(pair[1]);
    char data;
    EXPECT_EQ(io::read_some(descriptor, &data, 1).error,
              std::make_error_code(std::errc::operation_not_permitted));
    EXPECT_FALSE(io::wait_ready(descriptor, io::Interest::read, test::ms(0)));
}
