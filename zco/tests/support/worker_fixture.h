#ifndef ZLYNX_ZCO_TESTS_SUPPORT_WORKER_FIXTURE_H_
#define ZLYNX_ZCO_TESTS_SUPPORT_WORKER_FIXTURE_H_

#include "runtime/worker.h"
#include "support/fake_reactor.h"
#include "support/runtime_fixture.h"
#include <memory>
#include <mutex>

namespace zco {
namespace test {
/**
 * @brief 先中止部分启动的工作线程组，避免线程析构等待尚未释放的启动屏障。
 */
template <class Startup> class AbortStartup {
  public:
    explicit AbortStartup(std::shared_ptr<Startup> state)
        : state_(std::move(state)) {}

    ~AbortStartup() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->launch) {
            state_->abort = true;
            state_->cv.notify_all();
        }
    }

  private:
    std::shared_ptr<Startup> state_;
};

class WorkerFixture {
  public:
    WorkerFixture()
        : submission(std::make_shared<detail::Submission>()),
          reactor(std::make_shared<FakeReactor>()),
          worker(0, RuntimeOptions{1}, submission, reactor) {
        submission->workers.push_back(&worker);
        worker.set_peers(submission->workers);
        auto startup = std::make_shared<detail::Startup>();
        AbortStartup abort_startup(startup);
        worker.start(startup);
        std::unique_lock<std::mutex> lock(startup->mutex);
        if (!startup->cv.wait_for(lock, std::chrono::seconds(3),
                                  [&] { return startup->ready == 1; })) {
            startup->abort = true;
            startup->cv.notify_all();
            throw std::runtime_error("test worker startup timed out");
        }
        submission->accepting = true;
        startup->launch = true;
        startup->cv.notify_all();
    }

    ~WorkerFixture() {
        {
            std::lock_guard<std::mutex> lock(submission->mutex);
            submission->accepting = false;
        }
        worker.stop();
        worker.join();
    }

    TaskHandle spawn(Task task) {
        return std::move(detail::submit(submission, std::move(task), 0, true))
            .value();
    }

    std::shared_ptr<detail::Submission> submission;
    std::shared_ptr<FakeReactor> reactor;
    detail::Worker worker;
};
} // namespace test
} // namespace zco

#endif
