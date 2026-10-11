#pragma once

#include "znet/error.h"
#include <functional>

namespace znet::detail {
inline void report_error(const std::function<void(const Error &)> &on_error,
                         const Error &error) noexcept {
    if (on_error) {
        try {
            on_error(error);
        } catch (...) {
        }
    }
}

inline Error callback_error(std::string detail) {
    return make_error(ErrorKind::application, std::errc::io_error,
                      "session callback", std::move(detail));
}
} // namespace znet::detail
