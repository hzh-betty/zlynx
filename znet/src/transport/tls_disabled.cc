#include "znet/transport/tls_credentials.h"
#include "znet/transport/socket.h"

namespace znet {
namespace {
Error tls_disabled_error(std::string operation) {
    return make_error(ErrorKind::runtime, std::errc::operation_not_supported,
                      std::move(operation), "znet built without TLS support");
}
} // namespace

struct TlsCredentials::Impl {};

TlsCredentials::TlsCredentials(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TlsCredentials::~TlsCredentials() = default;
TlsCredentials::TlsCredentials(TlsCredentials &&) noexcept = default;
TlsCredentials &TlsCredentials::operator=(TlsCredentials &&) noexcept = default;

Result<TlsCredentials> TlsCredentials::load(const std::string &certificate,
                                            const std::string &private_key) {
    (void)certificate;
    (void)private_key;
    return tls_disabled_error("TLS credentials");
}

Result<std::unique_ptr<ByteStream>>
TlsCredentials::make_stream(Socket socket) const {
    (void)socket;
    return tls_disabled_error("TLS stream");
}
} // namespace znet
