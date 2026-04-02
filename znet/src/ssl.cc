#include "znet/ssl.h"

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#include "zcoroutine/io_event.h"
#include "zcoroutine/sched.h"

#ifdef ZNET_WITH_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace znet {
namespace ssl {
namespace {

thread_local std::string g_ssl_error;

uint32_t to_timeout_ms(int ms) {
  return ms < 0 ? zcoroutine::kInfiniteTimeoutMs : static_cast<uint32_t>(ms);
}

void set_ssl_error_message(S* s = 0, int r = 0) {
  g_ssl_error.clear();

  unsigned long e = 0;
  while ((e = ERR_get_error()) != 0) {
    char buf[256] = {0};
    ERR_error_string_n(e, buf, sizeof(buf));
    if (!g_ssl_error.empty()) {
      g_ssl_error.append(". ");
    }
    g_ssl_error.append(buf);
  }

  if (!g_ssl_error.empty()) {
    return;
  }

  if (errno != 0) {
    g_ssl_error = std::strerror(errno);
    return;
  }

  if (s != 0) {
    const int ssl_err = SSL_get_error(reinterpret_cast<SSL*>(s), r);
    g_ssl_error = "ssl error: " + std::to_string(ssl_err);
    return;
  }

  g_ssl_error = "success";
}

bool wait_ssl_event(S* s, int ssl_error, int ms) {
  const int fd = SSL_get_fd(reinterpret_cast<SSL*>(s));
  if (fd < 0) {
    errno = EBADF;
    return false;
  }

  zcoroutine::IoEventType event_type;
  if (ssl_error == SSL_ERROR_WANT_READ) {
    event_type = zcoroutine::IoEventType::kRead;
  } else if (ssl_error == SSL_ERROR_WANT_WRITE) {
    event_type = zcoroutine::IoEventType::kWrite;
  } else {
    errno = EIO;
    return false;
  }

  zcoroutine::IoEvent ev(fd, event_type);
  return ev.wait(to_timeout_ms(ms));
}

bool require_coroutine_context() {
  if (zcoroutine::in_coroutine()) {
    return true;
  }

  errno = EPERM;
  set_ssl_error_message();
  return false;
}

}  // namespace

const char* strerror(S* s) {
  set_ssl_error_message(s, 0);
  return g_ssl_error.c_str();
}

C* new_ctx(char c) {
  static std::once_flag once;
  std::call_once(once, []() {
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
  });

  ERR_clear_error();
  SSL_CTX* ctx = SSL_CTX_new(c == 's' ? TLS_server_method() : TLS_client_method());
  if (ctx == nullptr) {
    set_ssl_error_message();
  }
  return reinterpret_cast<C*>(ctx);
}

void free_ctx(C* c) {
  if (c != nullptr) {
    SSL_CTX_free(reinterpret_cast<SSL_CTX*>(c));
  }
}

S* new_ssl(C* c) {
  ERR_clear_error();
  SSL* s = SSL_new(reinterpret_cast<SSL_CTX*>(c));
  if (s == nullptr) {
    set_ssl_error_message();
  }
  return reinterpret_cast<S*>(s);
}

void free_ssl(S* s) {
  if (s != nullptr) {
    SSL_free(reinterpret_cast<SSL*>(s));
  }
}

int set_fd(S* s, int fd) {
  ERR_clear_error();
  const int r = SSL_set_fd(reinterpret_cast<SSL*>(s), fd);
  if (r != 1) {
    set_ssl_error_message(s, r);
  }
  return r;
}

int get_fd(const S* s) {
  if (s == nullptr) {
    return -1;
  }
  return SSL_get_fd(reinterpret_cast<const SSL*>(s));
}

int use_private_key_file(C* c, const char* path) {
  ERR_clear_error();
  const int r = SSL_CTX_use_PrivateKey_file(reinterpret_cast<SSL_CTX*>(c), path, SSL_FILETYPE_PEM);
  if (r != 1) {
    set_ssl_error_message();
  }
  return r;
}

int use_certificate_file(C* c, const char* path) {
  ERR_clear_error();
  const int r = SSL_CTX_use_certificate_file(reinterpret_cast<SSL_CTX*>(c), path, SSL_FILETYPE_PEM);
  if (r != 1) {
    set_ssl_error_message();
  }
  return r;
}

int check_private_key(const C* c) {
  ERR_clear_error();
  const int r = SSL_CTX_check_private_key(reinterpret_cast<const SSL_CTX*>(c));
  if (r != 1) {
    set_ssl_error_message();
  }
  return r;
}

int shutdown(S* s, int ms) {
  if (s == nullptr) {
    errno = EINVAL;
    set_ssl_error_message();
    return -1;
  }

  if (!require_coroutine_context()) {
    return -1;
  }

  const int e = SSL_get_error(reinterpret_cast<SSL*>(s), 0);
  if (e == SSL_ERROR_SYSCALL || e == SSL_ERROR_SSL) {
    set_ssl_error_message(s, 0);
    return -1;
  }

  while (true) {
    ERR_clear_error();
    const int r = SSL_shutdown(reinterpret_cast<SSL*>(s));
    if (r == 1) {
      return 1;
    }

    if (r == 0) {
      continue;
    }

    const int ssl_error = SSL_get_error(reinterpret_cast<SSL*>(s), r);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      if (!wait_ssl_event(s, ssl_error, ms)) {
        set_ssl_error_message(s, r);
        return -1;
      }
      continue;
    }

    set_ssl_error_message(s, r);
    return r;
  }
}

int accept(S* s, int ms) {
  if (s == nullptr) {
    errno = EINVAL;
    set_ssl_error_message();
    return -1;
  }

  if (!require_coroutine_context()) {
    return -1;
  }

  while (true) {
    ERR_clear_error();
    const int r = SSL_accept(reinterpret_cast<SSL*>(s));
    if (r == 1) {
      return 1;
    }

    if (r == 0) {
      return 0;
    }

    const int ssl_error = SSL_get_error(reinterpret_cast<SSL*>(s), r);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      if (!wait_ssl_event(s, ssl_error, ms)) {
        set_ssl_error_message(s, r);
        return -1;
      }
      continue;
    }

    set_ssl_error_message(s, r);
    return r;
  }
}

int connect(S* s, int ms) {
  if (s == nullptr) {
    errno = EINVAL;
    set_ssl_error_message();
    return -1;
  }

  if (!require_coroutine_context()) {
    return -1;
  }

  while (true) {
    ERR_clear_error();
    const int r = SSL_connect(reinterpret_cast<SSL*>(s));
    if (r == 1) {
      return 1;
    }

    if (r == 0) {
      return 0;
    }

    const int ssl_error = SSL_get_error(reinterpret_cast<SSL*>(s), r);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      if (!wait_ssl_event(s, ssl_error, ms)) {
        set_ssl_error_message(s, r);
        return -1;
      }
      continue;
    }

    set_ssl_error_message(s, r);
    return r;
  }
}

int recv(S* s, void* buf, int n, int ms) {
  if (s == nullptr || buf == nullptr || n < 0) {
    errno = EINVAL;
    set_ssl_error_message();
    return -1;
  }

  if (!require_coroutine_context()) {
    return -1;
  }

  while (true) {
    ERR_clear_error();
    const int r = SSL_read(reinterpret_cast<SSL*>(s), buf, n);
    if (r > 0) {
      return r;
    }

    if (r == 0) {
      return 0;
    }

    const int ssl_error = SSL_get_error(reinterpret_cast<SSL*>(s), r);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      if (!wait_ssl_event(s, ssl_error, ms)) {
        set_ssl_error_message(s, r);
        return -1;
      }
      continue;
    }

    set_ssl_error_message(s, r);
    return r;
  }
}

int recvn(S* s, void* buf, int n, int ms) {
  if (s == nullptr || buf == nullptr || n < 0) {
    errno = EINVAL;
    set_ssl_error_message();
    return -1;
  }

  if (!require_coroutine_context()) {
    return -1;
  }

  char* p = static_cast<char*>(buf);
  int remain = n;

  while (true) {
    ERR_clear_error();
    const int r = SSL_read(reinterpret_cast<SSL*>(s), p, remain);
    if (r == remain) {
      return n;
    }

    if (r == 0) {
      return 0;
    }

    if (r > 0) {
      remain -= r;
      p += r;
      continue;
    }

    const int ssl_error = SSL_get_error(reinterpret_cast<SSL*>(s), r);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      if (!wait_ssl_event(s, ssl_error, ms)) {
        set_ssl_error_message(s, r);
        return -1;
      }
      continue;
    }

    set_ssl_error_message(s, r);
    return r;
  }
}

int send(S* s, const void* buf, int n, int ms) {
  if (s == nullptr || buf == nullptr || n < 0) {
    errno = EINVAL;
    set_ssl_error_message();
    return -1;
  }

  if (!require_coroutine_context()) {
    return -1;
  }

  const char* p = static_cast<const char*>(buf);
  int remain = n;

  while (true) {
    ERR_clear_error();
    const int r = SSL_write(reinterpret_cast<SSL*>(s), p, remain);
    if (r == remain) {
      return n;
    }

    if (r == 0) {
      return 0;
    }

    if (r > 0) {
      remain -= r;
      p += r;
      continue;
    }

    const int ssl_error = SSL_get_error(reinterpret_cast<SSL*>(s), r);
    if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
      if (!wait_ssl_event(s, ssl_error, ms)) {
        set_ssl_error_message(s, r);
        return -1;
      }
      continue;
    }

    set_ssl_error_message(s, r);
    return r;
  }
}

bool timeout() {
  return zcoroutine::timeout();
}

}  // namespace ssl
}  // namespace znet

#else

namespace znet {
namespace ssl {
namespace {
thread_local std::string g_ssl_error = "openssl support is disabled";
}

const char* strerror(S*) {
  return g_ssl_error.c_str();
}

C* new_ctx(char) {
  errno = ENOTSUP;
  return 0;
}

void free_ctx(C*) {}
S* new_ssl(C*) { return 0; }
void free_ssl(S*) {}
int set_fd(S*, int) { errno = ENOTSUP; return 0; }
int get_fd(const S*) { errno = ENOTSUP; return -1; }
int use_private_key_file(C*, const char*) { errno = ENOTSUP; return 0; }
int use_certificate_file(C*, const char*) { errno = ENOTSUP; return 0; }
int check_private_key(const C*) { errno = ENOTSUP; return 0; }
int shutdown(S*, int) { errno = ENOTSUP; return -1; }
int accept(S*, int) { errno = ENOTSUP; return -1; }
int connect(S*, int) { errno = ENOTSUP; return -1; }
int recv(S*, void*, int, int) { errno = ENOTSUP; return -1; }
int recvn(S*, void*, int, int) { errno = ENOTSUP; return -1; }
int send(S*, const void*, int, int) { errno = ENOTSUP; return -1; }
bool timeout() { return false; }

}  // namespace ssl
}  // namespace znet

#endif
