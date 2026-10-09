#include "znet/error.h"

namespace znet {
std::string Error::message() const {
    if (!*this)
        return {};
    return operation + ": " + code.message() +
           (detail.empty() ? "" : " (" + detail + ")");
}

Error make_error(ErrorKind kind, std::errc code, std::string operation,
                 std::string detail) {
    return {kind, std::make_error_code(code), std::move(operation),
            std::move(detail)};
}

Error io_error(std::string operation, int code) {
    return {ErrorKind::io,
            std::error_code(code, std::generic_category()),
            std::move(operation),
            {}};
}

Error runtime_error(std::string operation, std::error_code code) {
    return {ErrorKind::runtime, code, std::move(operation), {}};
}
} // namespace znet
