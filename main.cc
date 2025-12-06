#include "io_manager.h"
#include "hook.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <thread>
#include "zlynx_logger.h"

void test1()
{
    try
    {
        ZLYNX_LOG_INFO("main start");
        auto f = std::make_shared<zlynx::Fiber>([]
        {
            ZLYNX_LOG_INFO("fiber start");
            zlynx::Fiber::get_fiber()->yield();
            ZLYNX_LOG_INFO("fiber resume");
        });
        zlynx::Fiber::get_fiber();
        f->resume();
        ZLYNX_LOG_INFO("after first resume");
        f->resume();
        ZLYNX_LOG_INFO("main end");
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_FATAL("Unhandled exception: {}", e.what());
    }
}

void test2()
{
    try
    {
        ZLYNX_LOG_INFO("Scheduler test start");
        // zlynx::Fiber::get_fiber();

        // 创建调度器，使用2个线程
        zlynx::Scheduler scheduler(8, false, "TestScheduler");

        // 启动调度器
        scheduler.start();

        // 提交任务
        for (int i = 0; i < 10000; ++i)
        {
            scheduler.schedule([i]()
            {
                ZLYNX_LOG_INFO("Task {} running in thread {}", i, zlynx::Thread::get_name());
                zlynx::Fiber::get_fiber()->yield(); // 模拟任务挂起
                ZLYNX_LOG_INFO("Task {} resumed in thread {}", i, zlynx::Thread::get_name());
            });
        }
        // 停止调度器
        scheduler.stop();
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_ERROR("Scheduler test exception: {}", e.what());
    }

    usleep(1000); // 确保日志输出完整
    ZLYNX_LOG_INFO("Scheduler test end");
}

void test3()
{
    try
    {
        ZLYNX_LOG_INFO("Timer test start");

        // 创建调度器
        zlynx::Scheduler scheduler(4, true, "TimerScheduler");
        scheduler.start();

        // 创建定时器管理器
        class TestTimerManager : public zlynx::TimerManager
        {
        protected:
            void on_timer_inserted_at_front() override
            {
                ZLYNX_LOG_DEBUG("Timer inserted at front, tickling scheduler");
                tickle();
            }

            void tickle()
            {
                ZLYNX_LOG_DEBUG("Tickle called");
            }
        };

        auto timer_manager = std::make_shared<TestTimerManager>();

        // 测试1: 单次定时器
        auto timer1 = timer_manager->add_timer(1000, []()
        {
            ZLYNX_LOG_INFO("Timer1 executed (1000ms)");
        }, false);

        // 测试2: 循环定时器
        std::atomic<int> counter{0};
        auto timer2 = timer_manager->add_timer(500, [&counter]()
        {
            ZLYNX_LOG_INFO("Timer2 executed (500ms), count={}", ++counter);
        }, true);

        // 测试3: 条件定时器
        std::weak_ptr<int> weak_cond(std::make_shared<int>(42));
        auto timer3 = timer_manager->add_condition_timer(1500, [weak_cond]()
        {
            if (auto cond = weak_cond.lock())
            {
                ZLYNX_LOG_INFO("Timer3 executed (1500ms), cond={}", *cond);
            }
            else
            {
                ZLYNX_LOG_WARN("Timer3 condition expired");
            }
        }, weak_cond, false);

        // 测试4: 定时器取消
        auto timer4 = timer_manager->add_timer(2000, []()
        {
            ZLYNX_LOG_ERROR("Timer4 should not execute (canceled)");
        }, false);

        // 模拟定时器调度
        scheduler.schedule([timer_manager]()
        {
            ZLYNX_LOG_INFO("Timer scheduler fiber start");

            for (int i = 0; i < 10; ++i)
            {
                // 获取下一个超时时间
                auto next_time = timer_manager->get_next_expire_time();
                if (next_time > 0)
                {
                    ZLYNX_LOG_DEBUG("Next timer expires in {}ms", next_time);
                    usleep(next_time * 1000); // 模拟等待
                }
                else
                {
                    usleep(100000); // 100ms
                }

                // 收集并执行到期回调
                std::vector<zlynx::Timer::Callback> cbs;
                timer_manager->list_expired_callbacks(cbs);

                ZLYNX_LOG_DEBUG("Collected {} expired timers", cbs.size());

                for (auto &cb: cbs)
                {
                    if (cb)
                    {
                        cb();
                    }
                }
            }

            ZLYNX_LOG_INFO("Timer scheduler fiber end");
        });

        // 500ms 后取消 timer4
        usleep(500000);
        if (timer4->cancel())
        {
            ZLYNX_LOG_INFO("Timer4 canceled successfully");
        }

        // 等待定时器执行
        usleep(5000000); // 5秒

        // 停止循环定时器
        if (timer2->cancel())
        {
            ZLYNX_LOG_INFO("Timer2 canceled, executed {} times", counter.load());
        }

        scheduler.stop();
        ZLYNX_LOG_INFO("Timer test end");
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_ERROR("Timer test exception: {}", e.what());
    }
}

void test4()
{
    try
    {
        ZLYNX_LOG_INFO("IoManager test start");

        // 创建 IoManager
        auto io_manager = std::make_shared<zlynx::IoManager>(2, true, "TestIoManager");

        // 测试1: 定时器功能
        std::atomic<int> timer_count{0};
        io_manager->add_timer(500, [&timer_count]()
        {
            ZLYNX_LOG_INFO("IoManager timer executed, count={}", ++timer_count);
        }, false);

        // 测试2: 文件描述符事件
        int pipe_fds[2];
        if (pipe(pipe_fds) == -1)
        {
            ZLYNX_LOG_ERROR("pipe() failed: {}", strerror(errno));
            return;
        }

        // 读端设置非阻塞
        int flags = fcntl(pipe_fds[0], F_GETFL);
        fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);

        // 添加读事件监听
        io_manager->add_event(pipe_fds[0], zlynx::IoManager::kRead, [pipe_fds]()
        {
            char buf[256];
            for (;;)
            {
                ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
                if (n > 0)
                {
                    buf[n] = '\0';
                    ZLYNX_LOG_INFO("Read from pipe: {}", buf);
                }
                else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    break; // 没有更多数据了
                }
                else
                {
                    break; // 出错或对端关闭
                }
            }
        });

        // 延迟写入数据
        io_manager->add_timer(1000, [pipe_fds]()
        {
            const char *msg = "Hello IoManager!";
            write(pipe_fds[1], msg, strlen(msg));
            ZLYNX_LOG_INFO("Written to pipe");
        }, false);


        // 测试3: 写事件监听
        io_manager->add_timer(1500, [pipe_fds]()
        {
            ZLYNX_LOG_INFO("Pipe is writable");
            const char *msg = "Second write";
            write(pipe_fds[1], msg, strlen(msg));
        }, false);


        io_manager->add_timer(2000, [pipe_fds]()
        {
            char buf[256];
            for (;;)
            {
                ssize_t n = read(pipe_fds[0], buf, sizeof(buf) - 1);
                if (n > 0)
                {
                    buf[n] = '\0';
                    ZLYNX_LOG_INFO("Read from pipe: {}", buf);
                }
                else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                {
                    break; // 没有更多数据了
                }
                else
                {
                    break; // 出错或对端关闭
                }
            }
        }, false);


        // 运行5秒后停止
        sleep(5);

        // 清理资源
        close(pipe_fds[0]);
        close(pipe_fds[1]);

        io_manager->stop();
        ZLYNX_LOG_INFO("IoManager test end");
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_ERROR("IoManager test exception: {}", e.what());
    }
}



static int sock_listen_fd = -1;

void test_accept();
void error(const char *msg)
{
    perror(msg);
    printf("erreur...\n");
    exit(1);
}

void watch_io_read()
{
    zlynx::IoManager::get_this()->add_event(sock_listen_fd, zlynx::IoManager::kRead, test_accept);
}

void test_accept()
{
    struct sockaddr_in addr{};
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    int fd = accept(sock_listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0)
    {
        // accept 失败直接返回，等待下一次事件
    }
    else
    {
        std::cout << "accepted connection, fd = " << fd << std::endl;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        zlynx::IoManager::get_this()->add_event(fd, zlynx::IoManager::kRead, [fd]()
        {
            char buffer[1024] = {};
            while (true)
            {
                int ret = recv(fd, buffer, sizeof(buffer), 0);
                if (ret > 0)
                {
                    const char *response =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 13\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n"
                        "Hello, World!";

                    ret = send(fd, response, strlen(response), 0);
                    (void)ret;
                    close(fd);
                    break;
                }
                if (ret <= 0)
                {
                    if (ret == 0 || errno != EAGAIN)
                    {
                        close(fd);
                        break;
                    }
                    else if (errno == EAGAIN)
                    {
                        // 非阻塞情况下，当前无数据可读，等待下次可读事件
                        break;
                    }
                }
            }
        });
    }
 zlynx::IoManager::get_this()->add_event(sock_listen_fd, zlynx::IoManager::kRead, test_accept);
}

void test_http_server()
{
    int portno = 8080;
    struct sockaddr_in server_addr{}, client_addr{};
    socklen_t client_len = sizeof(client_addr);
    (void)client_len;

    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_listen_fd < 0)
    {
        error("Error creating socket..\n");
    }

    int yes = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset((char *)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portno);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_listen_fd, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
        error("Error binding socket..\n");

    if (listen(sock_listen_fd, 1024) < 0)
    {
        error("Error listening..\n");
    }

    printf("coroutine http server listening on port: %d\n", portno);
    fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);

    zlynx::IoManager iom(9);
    iom.add_event(sock_listen_fd,  zlynx::IoManager::kRead, test_accept);
}

int main()
{
    zlynx::Init(zlog::LogLevel::value::INFO);
    // test1();
    // test2();
    // test3();
    // test4();
    test_http_server();
    return 0;
}
