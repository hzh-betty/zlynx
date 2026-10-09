#include "znet/transport/byte_stream.h"
#include "znet/transport/socket.h"

namespace znet {
namespace {
class TcpStream final : public ByteStream {
  public:
    explicit TcpStream(Socket socket) : socket_(std::move(socket)) {}

    Result<void> start(zco::Deadline) override {
        if (socket_.native_handle() < 0)
            return io_error("start TCP stream", EBADF);
        return {};
    }

    Transfer read_some(void *bytes, size_t size,
                       zco::Deadline deadline) override {
        return socket_.read_some(bytes, size, deadline);
    }

    Transfer write_some(const void *bytes, size_t size,
                        zco::Deadline deadline) override {
        return socket_.write_some(bytes, size, deadline);
    }

    Result<void> shutdown_write(zco::Deadline) override {
        return socket_.shutdown_write();
    }

    Result<void> close() override { return socket_.close(); }

    Result<Endpoint> local_endpoint() const override {
        return socket_.local_endpoint();
    }

    Result<Endpoint> remote_endpoint() const override {
        return socket_.remote_endpoint();
    }

    int native_handle() const override { return socket_.native_handle(); }

  private:
    Socket socket_;
};
} // namespace

Result<std::unique_ptr<ByteStream>> make_tcp_stream(Socket socket) {
    auto type = socket.option<int>(SOL_SOCKET, SO_TYPE);
    if (!type)
        return type.error();
    if (type.value() != SOCK_STREAM)
        return make_error(ErrorKind::validation, std::errc::invalid_argument,
                          "TCP stream", "requires a stream socket");
    return std::unique_ptr<ByteStream>(new TcpStream(std::move(socket)));
}
} // namespace znet
