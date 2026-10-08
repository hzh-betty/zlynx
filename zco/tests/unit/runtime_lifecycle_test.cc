#include "support/runtime_fixture.h"
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
using namespace zco;

namespace {
std::atomic<int> fail_thread_after{-1};
std::atomic<int> fail_epoll_after{-1};
} // namespace

extern "C" int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                              void *(*entry)(void *), void *argument) noexcept {
    using Create =
        int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    static auto create =
        reinterpret_cast<Create>(::dlsym(RTLD_NEXT, "pthread_create"));
    int remaining = fail_thread_after.load();
    if (remaining >= 0 && fail_thread_after.fetch_sub(1) == 0)
        return EAGAIN;
    return create(thread, attr, entry, argument);
}

extern "C" int __real_epoll_create1(int);

extern "C" int __wrap_epoll_create1(int flags) {
    if (fail_epoll_after.load() >= 0 && fail_epoll_after.fetch_sub(1) == 0) {
        errno = EMFILE;
        return -1;
    }
    return __real_epoll_create1(flags);
}

namespace {
size_t fd_count() {
    auto *directory = ::opendir("/proc/self/fd");
    if (!directory)
        throw std::runtime_error("opendir");
    size_t count = 0;
    while (::readdir(directory))
        ++count;
    ::closedir(directory);
    return count;
}

TEST(Runtime, InvalidOptionsFailBeforePublication) {
    RuntimeOptions options{1};
    options.stack_size = 1;
    EXPECT_THROW(Runtime runtime(options), std::invalid_argument);
    options.stack_size = 65536;
    options.shared_stack_count = 0;
    EXPECT_THROW(Runtime runtime(options), std::invalid_argument);
}

TEST(Runtime, ThreadStartupFailureRollsBackAllResources) {
    auto count = fd_count();
    fail_thread_after = 1;
    EXPECT_THROW(Runtime runtime(RuntimeOptions{3}), std::system_error);
    fail_thread_after = -1;
    EXPECT_EQ(fd_count(), count);
    Runtime next(RuntimeOptions{1});
    EXPECT_TRUE(test::spawn(next, [] {}).join(test::soon()));
}

TEST(Runtime, StopIsIdempotentAndEndpointsExpire) {
    Executor executor;
    TaskHandle handle;
    {
        Runtime runtime(RuntimeOptions{2});
        executor = runtime.executor(1);
        handle = test::spawn(executor, [] {});
        EXPECT_TRUE(handle.join(test::soon()));
        runtime.request_stop();
        runtime.request_stop();
        runtime.join();
        runtime.join();
        EXPECT_FALSE(runtime.spawn([] {}));
        EXPECT_FALSE(executor.valid());
    }
    EXPECT_FALSE(executor.spawn([] {}));
    EXPECT_TRUE(handle.join(test::soon()));
    EXPECT_EQ(handle.status(), TaskStatus::succeeded);
}

TEST(Runtime, TaskFailureAndCaptureReleaseAreObservable) {
    Runtime runtime(RuntimeOptions{1});
    auto capture = std::make_shared<int>(1);
    std::weak_ptr<int> weak = capture;
    auto task = test::spawn(
        runtime, [capture] { throw std::runtime_error("user error"); });
    capture.reset();
    EXPECT_THROW(task.join(test::soon()), std::runtime_error);
    EXPECT_EQ(task.status(), TaskStatus::failed);
    EXPECT_TRUE(task.exception());
    EXPECT_TRUE(weak.expired());
}

TEST(Runtime, RequestStopFromWorkerAndSelfJoinContract) {
    Runtime runtime(RuntimeOptions{1});
    auto task = test::spawn(runtime, [&] {
        EXPECT_THROW(runtime.join(), std::logic_error);
        runtime.request_stop();
    });
    EXPECT_TRUE(task.join(test::soon()));
    runtime.join();
}

TEST(Runtime, ConcurrentSubmitAndStopHaveDefiniteResults) {
    Runtime runtime(RuntimeOptions{3});
    auto executor = runtime.executor(0);
    std::atomic<bool> go{false};
    std::atomic<int> admitted{0};
    std::thread submitter([&] {
        while (!go.load())
            std::this_thread::yield();
        for (int i = 0; i < 2000; ++i) {
            auto result = executor.spawn([] { yield(); });
            if (result) {
                ++admitted;
                auto joined = result.value().join(test::soon());
                EXPECT_TRUE(joined || joined.error() ==
                                          wait_error(WaitOutcome::canceled));
            } else
                EXPECT_EQ(result.error(), wait_error(WaitOutcome::canceled));
        }
    });
    go = true;
    while (admitted.load() < 10)
        std::this_thread::yield();
    runtime.request_stop();
    submitter.join();
    runtime.join();
}
} // namespace

TEST(Runtime, KernelResourceStartupFailureRollsBackEarlierWorkers) {
    auto count = fd_count();
    fail_epoll_after = 1;
    EXPECT_THROW(Runtime runtime(RuntimeOptions{3}), std::system_error);
    fail_epoll_after = -1;
    EXPECT_EQ(fd_count(), count);
}

TEST(Runtime, JoinReleasesKernelResourcesBeforeRuntimeDestruction) {
    auto before = fd_count();
    Runtime runtime(RuntimeOptions{2});
    EXPECT_GT(fd_count(), before);
    runtime.request_stop();
    runtime.join();
    EXPECT_EQ(fd_count(), before);
    EXPECT_FALSE(runtime.executor(0).valid());
}
