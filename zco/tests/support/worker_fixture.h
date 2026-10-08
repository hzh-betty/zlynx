#pragma once
#include "runtime/worker.h"
#include "support/fake_reactor.h"
#include "support/runtime_fixture.h"

namespace zco {
namespace test {
class WorkerFixture {
  public:
    WorkerFixture()
        : submission(std::make_shared<detail::Submission>()),
          reactor(std::make_shared<FakeReactor>()),
          worker(0, RuntimeOptions{1}, submission, reactor) {
        submission->workers.push_back(&worker);
        worker.set_peers(submission->workers);
        auto startup = std::make_shared<detail::Startup>();
        worker.start(startup);
        std::unique_lock<std::mutex> lock(startup->mutex);
        startup->cv.wait(lock, [&] { return startup->ready == 1; });
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
