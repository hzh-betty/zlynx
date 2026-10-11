#pragma once

#include "znet/server/tcp_server.h"
#include <atomic>

namespace znet::detail {
void serve_session(const Connection::ptr &connection,
                   const SessionFactory &factory, const ServerOptions &options,
                   const std::atomic<bool> &running);
} // namespace znet::detail
