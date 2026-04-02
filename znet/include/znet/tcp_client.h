#ifndef ZNET_TCP_CLIENT_H_
#define ZNET_TCP_CLIENT_H_

namespace znet {

class TcpClient final {
 public:
  TcpClient(const char* ip, int port, bool use_ssl = false);
  TcpClient(const TcpClient& c);
  ~TcpClient();

  void operator=(const TcpClient& c) = delete;

  int recv(void* buf, int n, int ms = -1);
  int recvn(void* buf, int n, int ms = -1);
  int send(const void* buf, int n, int ms = -1);

  bool bind(const char* ip, int port = 0);
  bool connected() const noexcept;
  bool connect(int ms);

  void disconnect();
  void close() { this->disconnect(); }

  const char* strerror() const;
  int socket() const noexcept;

 private:
  void* impl_;
};

}  // namespace znet

#endif  // ZNET_TCP_CLIENT_H_
