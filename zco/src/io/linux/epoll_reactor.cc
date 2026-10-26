#include "io/linux/epoll_reactor.h"
#include <atomic>
#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace zco {
namespace detail {
namespace {
std::atomic<uint64_t> next_registration{0};

uint32_t native_events(unsigned mask) {
    return ((mask & 1) ? static_cast<uint32_t>(EPOLLIN) : 0u) |
           ((mask & 2) ? static_cast<uint32_t>(EPOLLOUT) : 0u);
}
} // namespace

EpollReactor::EpollReactor() {
    epoll_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_ < 0)
        throw std::system_error(errno, std::generic_category(),
                                "epoll_create1");
    event_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = 0;
    if (event_ < 0 || ::epoll_ctl(epoll_, EPOLL_CTL_ADD, event_, &event) != 0) {
        int error = errno;
        if (event_ >= 0)
            ::close(event_);
        ::close(epoll_);
        throw std::system_error(error, std::generic_category(),
                                "eventfd registration");
    }
}

EpollReactor::~EpollReactor() {
    ::close(event_);
    ::close(epoll_);
}

Result<RegistrationId> EpollReactor::add(io::ResourceId resource, int fd,
                                         io::Interest interest) {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned requested = static_cast<unsigned>(interest);
    if (!resource.value || !requested || requested > 3)
        return Result<RegistrationId>(
            std::make_error_code(std::errc::invalid_argument));
    
    
    auto found = resources_.find(resource.value);
    Bucket next{fd, {}};
    unsigned old_mask = 0;

    // 检查是否有兴趣冲突；如果没有，分配一个新的注册 ID 并登记到 epoll。
    if (found != resources_.end()) {
        if (found->second.fd != fd)
            throw std::logic_error("Resource identity changed its fd");

        // 拷贝桶内容，避免在 epoll_ctl 失败时留下半完成注册。
        next = found->second; 
        for (auto &item : next.registrations)
            old_mask |= item.second;
    }
    if (old_mask & requested)
        return Result<RegistrationId>(
            std::make_error_code(std::errc::device_or_resource_busy));
    auto id = ++next_registration;
    next.registrations.emplace(id, requested);

    // 先分配用户态条目；epoll_ctl 失败时撤销条目，避免留下半完成注册。
    registrations_.emplace(id, resource.value);
    try {
        if (found == resources_.end())
            found = resources_.emplace(resource.value, Bucket{fd, {}}).first;
    } catch (...) {
        registrations_.erase(id);
        throw;
    }
    epoll_event event{};
    event.events = native_events(old_mask | requested);

    //这里把传给内核的 event.data.u64 设置为 ResourceId，
    //而不是 fd，也不是 RegistrationId。
    //因为内核监听的是合并后的资源事件，用户态才负责把事件拆给具体登记
    event.data.u64 = resource.value;
    int op = old_mask ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(epoll_, op, fd, &event) != 0) {
        int error = errno;
        registrations_.erase(id);
        if (!old_mask)
            resources_.erase(found);
        return Result<RegistrationId>(
            std::error_code(error, std::generic_category()));
    }

    // 交换桶内容，避免在 epoll_ctl 失败时留下半完成注册。
    found->second.registrations.swap(next.registrations);
    return Result<RegistrationId>(id);
}

Result<void> EpollReactor::remove(RegistrationId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto index = registrations_.find(id);
    if (index == registrations_.end())
        return {};
    auto bucket = resources_.find(index->second);
    bucket->second.registrations.erase(id); //删除对应的注册

    unsigned mask = 0;
    for (auto &item : bucket->second.registrations)
        mask |= item.second;
    epoll_event event{};
    event.events = native_events(mask);
    event.data.u64 = bucket->first;
    int result = ::epoll_ctl(epoll_, mask ? EPOLL_CTL_MOD : EPOLL_CTL_DEL,
                             bucket->second.fd, &event);
    const int error = errno;
    if (!mask)
        resources_.erase(bucket);
    registrations_.erase(index);
    return Result<void>(result < 0
                            ? std::error_code(error, std::generic_category())
                            : std::error_code{});
}

std::vector<ReadyEvent> EpollReactor::poll(int timeout_ms) {
    epoll_event events[64];

    // 调用 epoll_wait() 时没有持有 mutex_。其他线程仍能调用 add() 或 remove()；
    // 锁只保护返回事件与用户态映射之间的转换
    int count = ::epoll_wait(epoll_, events, 64, timeout_ms);
    if (count < 0) {
        if (errno == EINTR)
            return {};
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
    }
    std::vector<ReadyEvent> ready;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < count; ++i) {
        auto &event = events[i];

        // epoll_event 使用紧凑布局；先复制字段，避免给映射键绑定未对齐引用。
        const uint64_t resource = event.data.u64;
        if (!resource) {
            uint64_t value;
            while (::read(event_, &value, sizeof(value)) == sizeof(value)) {
            }
            continue;
        }
        auto bucket = resources_.find(resource);
        if (bucket == resources_.end())
            continue;
        unsigned mask = 0;
        if (event.events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            mask |= 1;
        if (event.events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
            mask |= 2;
        for (auto &entry : bucket->second.registrations)
            if (entry.second & mask)
                ready.push_back({entry.first, static_cast<io::Interest>(
                                                  entry.second & mask)});
    }
    return ready;
}

void EpollReactor::wake() {
    uint64_t value = 1;
    while (::write(event_, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
}
} // namespace detail
} // namespace zco
