/**
 * @file http_server.cc
 * @brief HTTP服务器示例 - 基于zcoroutine协程库
 * 
 * 参考zlynx的main.cc实现，使用zcoroutine库构建高性能HTTP服务器
 * 可使用wrk进行性能测试：wrk -t4 -c100 -d30s http://localhost:8080/
 */

#include "io/io_scheduler.h"
#include "hook/hook.h"
#include "zcoroutine_logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

using namespace zcoroutine;

static int listen_fd = -1;
static IoScheduler* g_io_scheduler = nullptr;

// HTTP响应内容
static const char* HTTP_RESPONSE = 
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

// 处理客户端连接读事件
void handle_client_read(int client_fd) {
    char buffer[4096] = {0};
    
    while (true) {
        int ret = recv(client_fd, buffer, sizeof(buffer), 0);
        
        if (ret > 0) {
            // 收到HTTP请求，发送响应
            // 可选：添加延迟模拟处理时间
            // std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            int send_ret = send(client_fd, HTTP_RESPONSE, strlen(HTTP_RESPONSE), 0);
            if (send_ret < 0) {
                ZCOROUTINE_LOG_ERROR("send failed, fd={} errno={}", client_fd, errno);
            }
            
            close(client_fd);
            break;
        } 
        else if (ret == 0) {
            // 客户端关闭连接
            close(client_fd);
            break;
        }
        else {
            // ret < 0
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 非阻塞模式下，暂无数据，等待下次读事件
                break;
            } else {
                // 其他错误
                ZCOROUTINE_LOG_ERROR("recv error, fd={} errno={}", client_fd, errno);
                close(client_fd);
                break;
            }
        }
    }
}

// 接受新连接
void accept_connection();

// 注册监听socket的读事件（用于下一次accept）
void register_accept_event() {
    if (g_io_scheduler) {
        g_io_scheduler->add_event(listen_fd, FdContext::kRead, accept_connection);
    }
}

// accept回调函数
void accept_connection() {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&client_addr, 0, sizeof(client_addr));
    
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
    
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ZCOROUTINE_LOG_ERROR("accept failed, errno={}", errno);
        }
        // 继续监听下一个连接
        register_accept_event();
        return;
    }
    
    // 设置客户端socket为非阻塞
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    ZCOROUTINE_LOG_DEBUG("Accepted connection, client_fd={}", client_fd);
    
    // 为新连接注册读事件
    if (g_io_scheduler) {
        g_io_scheduler->add_event(client_fd, FdContext::kRead, 
            [client_fd]() { handle_client_read(client_fd); });
    }
    
    // 继续监听下一个连接
    register_accept_event();
}

int main(int argc, char* argv[]) {
    // 默认端口
    int port = 8080;
    int thread_num = 4;
    
    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        thread_num = std::atoi(argv[2]);
    }
    
    std::cout << "Starting HTTP server on port " << port 
              << " with " << thread_num << " threads" << std::endl;
    
    // 创建监听socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }
    
    // 设置SO_REUSEADDR
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    // 绑定地址
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Failed to bind, errno=" << errno << std::endl;
        close(listen_fd);
        return 1;
    }
    
    // 监听
    if (listen(listen_fd, 1024) < 0) {
        std::cerr << "Failed to listen" << std::endl;
        close(listen_fd);
        return 1;
    }
    
    // 设置为非阻塞
    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    std::cout << "Server listening on 0.0.0.0:" << port << std::endl;
    std::cout << "Test with: curl http://localhost:" << port << "/" << std::endl;
    std::cout << "Benchmark: wrk -t4 -c100 -d30s http://localhost:" << port << "/" << std::endl;
    
    // 创建IoScheduler（使用指定数量的工作线程）
    auto io_scheduler = IoScheduler::CreateInstance(thread_num, true, "HttpServer");
    g_io_scheduler = io_scheduler.get();
    
    // 启用Hook（使系统调用异步化）
    set_hook_enable(true);
    
    // 注册accept事件
    io_scheduler->add_event(listen_fd, FdContext::kRead, accept_connection);
    
    // 主线程等待（服务器持续运行）
    // 可以添加信号处理来优雅关闭
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // 清理
    io_scheduler->stop();
    close(listen_fd);
    
    return 0;
}
