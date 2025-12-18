#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include "io/io_scheduler.h"
#include "hook/hook.h"
#include "zcoroutine_logger.h"

using namespace zcoroutine;

/**
 * @brief 示例1：定时器使用
 */
void timer_example() {
    std::cout << "\n=== 定时器示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    // 添加一次性定时器
    io_scheduler->add_timer(1000, []() {
        std::cout << "1秒定时器触发" << std::endl;
    });
    
    // 添加循环定时器
    io_scheduler->add_timer(500, []() {
        std::cout << "500毫秒循环定时器触发" << std::endl;
    }, true);
    
    // 等待几秒
    sleep(3);
    
    io_scheduler->stop();
}

/**
 * @brief 示例2：Hook sleep函数
 */
void hook_sleep_example() {
    std::cout << "\n=== Hook sleep示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    // 启用Hook
    set_hook_enable(true);
    
    // 创建协程
    auto fiber = std::make_shared<Fiber>([]() {
        std::cout << "协程开始执行" << std::endl;
        
        std::cout << "sleep 1秒..." << std::endl;
        sleep(1);  // 被Hook，不会阻塞线程
        
        std::cout << "sleep 1秒完成" << std::endl;
        
        usleep(500000);  // 被Hook
        
        std::cout << "协程执行完成" << std::endl;
    }, StackAllocator::kDefaultStackSize, "hook_sleep_fiber");
    
    io_scheduler->schedule(fiber);
    
    // 等待完成
    sleep(3);
    
    io_scheduler->stop();
}

/**
 * @brief 示例3：简单的Echo服务器（演示Hook socket IO）
 */
void echo_server_example() {
    std::cout << "\n=== Echo服务器示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    // 启用Hook
    set_hook_enable(true);
    
    // 创建服务器协程
    auto server_fiber = std::make_shared<Fiber>([]() {
        // 创建socket
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            std::cerr << "socket创建失败" << std::endl;
            return;
        }
        
        // 设置地址复用
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // 绑定地址
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(8888);
        
        if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "bind失败" << std::endl;
            close(listen_fd);
            return;
        }
        
        // 监听
        if (listen(listen_fd, 128) < 0) {
            std::cerr << "listen失败" << std::endl;
            close(listen_fd);
            return;
        }
        
        std::cout << "Echo服务器启动在端口8888" << std::endl;
        
        // 接受连接（仅接受一个连接作为示例）
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "accept失败" << std::endl;
            close(listen_fd);
            return;
        }
        
        std::cout << "接受到客户端连接: " << inet_ntoa(client_addr.sin_addr) << std::endl;
        
        // Echo处理
        char buffer[1024];
        while (true) {
            ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) {
                break;
            }
            
            buffer[n] = '\0';
            std::cout << "收到: " << buffer << std::endl;
            
            // Echo回去
            write(client_fd, buffer, n);
        }
        
        close(client_fd);
        close(listen_fd);
        
        std::cout << "Echo服务器关闭" << std::endl;
    }, StackAllocator::kDefaultStackSize, "echo_server");
    
    io_scheduler->schedule(server_fiber);
    
    std::cout << "提示：可以使用 telnet localhost 8888 连接测试" << std::endl;
    std::cout << "等待10秒后自动关闭..." << std::endl;
    
    // 等待一段时间
    sleep(10);
    
    io_scheduler->stop();
}

/**
 * @brief 示例4：多协程并发
 */
void concurrent_fibers_example() {
    std::cout << "\n=== 多协程并发示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    set_hook_enable(true);
    
    // 创建10个协程，每个协程sleep不同的时间
    for (int i = 0; i < 10; ++i) {
        auto fiber = std::make_shared<Fiber>([i]() {
            auto f = Fiber::get_this();
            std::cout << "协程[" << f->name() << "] 开始, sleep " << (i + 1) << "秒" << std::endl;
            
            sleep(i + 1);
            
            std::cout << "协程[" << f->name() << "] 完成" << std::endl;
        }, StackAllocator::kDefaultStackSize, "fiber_" + std::to_string(i));
        
        io_scheduler->schedule(fiber);
    }
    
    // 等待所有协程完成
    sleep(12);
    
    io_scheduler->stop();
}

int main() {
    // 初始化日志
    InitLogger(zlog::LogLevel::value::DEBUG);
    
    std::cout << "=== zcoroutine IO和Hook示例程序 ===" << std::endl;
    std::cout << "请选择示例:" << std::endl;
    std::cout << "1. 定时器示例" << std::endl;
    std::cout << "2. Hook sleep示例" << std::endl;
    std::cout << "3. Echo服务器示例" << std::endl;
    std::cout << "4. 多协程并发示例" << std::endl;
    std::cout << "请输入选项(1-4): ";
    
    int choice;
    std::cin >> choice;
    
    switch (choice) {
        case 1:
            timer_example();
            break;
        case 2:
            hook_sleep_example();
            break;
        case 3:
            echo_server_example();
            break;
        case 4:
            concurrent_fibers_example();
            break;
        default:
            std::cout << "无效选项" << std::endl;
            break;
    }
    
    return 0;
}
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include "io/io_scheduler.h"
#include "hook/hook.h"
#include "zcoroutine_logger.h"

using namespace zcoroutine;

/**
 * @brief 示例1：定时器使用
 */
void timer_example() {
    std::cout << "\n=== 定时器示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    // 添加一次性定时器
    io_scheduler->add_timer(1000, []() {
        std::cout << "1秒定时器触发" << std::endl;
    });
    
    // 添加循环定时器
    io_scheduler->add_timer(500, []() {
        std::cout << "500毫秒循环定时器触发" << std::endl;
    }, true);
    
    // 等待几秒
    sleep(3);
    
    io_scheduler->stop();
}

/**
 * @brief 示例2：Hook sleep函数
 */
void hook_sleep_example() {
    std::cout << "\n=== Hook sleep示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    // 启用Hook
    set_hook_enable(true);
    
    // 创建协程
    auto fiber = std::make_shared<Fiber>([]() {
        std::cout << "协程开始执行" << std::endl;
        
        std::cout << "sleep 1秒..." << std::endl;
        sleep(1);  // 被Hook，不会阻塞线程
        
        std::cout << "sleep 1秒完成" << std::endl;
        
        usleep(500000);  // 被Hook
        
        std::cout << "协程执行完成" << std::endl;
    }, StackAllocator::kDefaultStackSize, "hook_sleep_fiber");
    
    io_scheduler->schedule(fiber);
    
    // 等待完成
    sleep(3);
    
    io_scheduler->stop();
}

/**
 * @brief 示例3：简单的Echo服务器（演示Hook socket IO）
 */
void echo_server_example() {
    std::cout << "\n=== Echo服务器示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    // 启用Hook
    set_hook_enable(true);
    
    // 创建服务器协程
    auto server_fiber = std::make_shared<Fiber>([]() {
        // 创建socket
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            std::cerr << "socket创建失败" << std::endl;
            return;
        }
        
        // 设置地址复用
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        // 绑定地址
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(8888);
        
        if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "bind失败" << std::endl;
            close(listen_fd);
            return;
        }
        
        // 监听
        if (listen(listen_fd, 128) < 0) {
            std::cerr << "listen失败" << std::endl;
            close(listen_fd);
            return;
        }
        
        std::cout << "Echo服务器启动在端口8888" << std::endl;
        
        // 接受连接（仅接受一个连接作为示例）
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            std::cerr << "accept失败" << std::endl;
            close(listen_fd);
            return;
        }
        
        std::cout << "接受到客户端连接: " << inet_ntoa(client_addr.sin_addr) << std::endl;
        
        // Echo处理
        char buffer[1024];
        while (true) {
            ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
            if (n <= 0) {
                break;
            }
            
            buffer[n] = '\0';
            std::cout << "收到: " << buffer << std::endl;
            
            // Echo回去
            write(client_fd, buffer, n);
        }
        
        close(client_fd);
        close(listen_fd);
        
        std::cout << "Echo服务器关闭" << std::endl;
    }, StackAllocator::kDefaultStackSize, "echo_server");
    
    io_scheduler->schedule(server_fiber);
    
    std::cout << "提示：可以使用 telnet localhost 8888 连接测试" << std::endl;
    std::cout << "等待10秒后自动关闭..." << std::endl;
    
    // 等待一段时间
    sleep(10);
    
    io_scheduler->stop();
}

/**
 * @brief 示例4：多协程并发
 */
void concurrent_fibers_example() {
    std::cout << "\n=== 多协程并发示例 ===" << std::endl;
    
    auto io_scheduler = IoScheduler::GetInstance();
    io_scheduler->start();
    
    set_hook_enable(true);
    
    // 创建10个协程，每个协程sleep不同的时间
    for (int i = 0; i < 10; ++i) {
        auto fiber = std::make_shared<Fiber>([i]() {
            auto f = Fiber::get_this();
            std::cout << "协程[" << f->name() << "] 开始, sleep " << (i + 1) << "秒" << std::endl;
            
            sleep(i + 1);
            
            std::cout << "协程[" << f->name() << "] 完成" << std::endl;
        }, StackAllocator::kDefaultStackSize, "fiber_" + std::to_string(i));
        
        io_scheduler->schedule(fiber);
    }
    
    // 等待所有协程完成
    sleep(12);
    
    io_scheduler->stop();
}

int main() {
    // 初始化日志
    InitLogger(zlog::LogLevel::value::DEBUG);
    
    std::cout << "=== zcoroutine IO和Hook示例程序 ===" << std::endl;
    std::cout << "请选择示例:" << std::endl;
    std::cout << "1. 定时器示例" << std::endl;
    std::cout << "2. Hook sleep示例" << std::endl;
    std::cout << "3. Echo服务器示例" << std::endl;
    std::cout << "4. 多协程并发示例" << std::endl;
    std::cout << "请输入选项(1-4): ";
    
    int choice;
    std::cin >> choice;
    
    switch (choice) {
        case 1:
            timer_example();
            break;
        case 2:
            hook_sleep_example();
            break;
        case 3:
            echo_server_example();
            break;
        case 4:
            concurrent_fibers_example();
            break;
        default:
            std::cout << "无效选项" << std::endl;
            break;
    }
    
    return 0;
}
