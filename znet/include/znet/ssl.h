#ifndef ZNET_SSL_H_
#define ZNET_SSL_H_

namespace znet {
namespace ssl {

typedef void S;
typedef void C;

const char* strerror(S* s = 0);

C* new_ctx(char c);
inline C* new_server_ctx() { return new_ctx('s'); }
inline C* new_client_ctx() { return new_ctx('c'); }

void free_ctx(C* c);
S* new_ssl(C* c);
void free_ssl(S* s);

int set_fd(S* s, int fd);
int get_fd(const S* s);

int use_private_key_file(C* c, const char* path);
int use_certificate_file(C* c, const char* path);
int check_private_key(const C* c);

int shutdown(S* s, int ms = 3000);
int accept(S* s, int ms = -1);
int connect(S* s, int ms = -1);

int recv(S* s, void* buf, int n, int ms = -1);
int recvn(S* s, void* buf, int n, int ms = -1);
int send(S* s, const void* buf, int n, int ms = -1);

bool timeout();

}  // namespace ssl
}  // namespace znet

#endif  // ZNET_SSL_H_
