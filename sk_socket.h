#pragma once
#include "asio/any_io_executor.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/ip/udp.hpp"
#include "utils.h"
#include <asio.hpp>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <list>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cpp_sk {
/*
  skynet 代码，server 一上来就把上限的 socket 数组内存分配好
  后面，通过allocid 类似 linux 中的 fd，自己封装了另外一套fd
  ctx 中并发的获取socket 数组中的值，并把新的fd 分配给它，并把 ctx的handle 跟
  对应的 id 绑定好；后面write 到 ctrl 的一个pipe中。
  pipe 在socket thread 中在epoll 之前优先判断,取出并进行操作。随后poll
  对异步结果 进行收集，并forward 到对应的ctx queue中，随后 ctx
  就收到了对应的消息

  完成 socket 的任务
  socket:
    1. pair 读取到 fin 的时候，close 当前的 read，如果本地还有write
  buf，就写完为止

    2. pair 写失败的时候，需要强制 close 了

  整个流程代码稍微有点多，对于cpp来说，直接上asio 简化其中封装的细节。
*/
struct context_t;

enum class skynet_socket_type {
  data = 1,
  connect = 2,
  close = 3,
  accept = 4,
  error = 5,
  udp = 6,
  warning = 7
};

struct string_buf {
  std::string str;
  std::string_view str_view;

  size_t size() {
    if (!str.empty()) {
      return str.size();
    }
    if (!str_view.empty()) {
      return str_view.size();
    }
    return 0;
  }
  const char *ptr() {
    if (!str.empty()) {
      return str.data();
    }
    if (!str_view.empty()) {
      return str_view.data();
    }
    return nullptr;
  }
};

struct socket_message {
  int id = 0;
  uintptr_t opaque = 0;
  int ud =
      0; // for accept, ud is new connection id ; for data, ud is size of data
  string_buf data;
};

struct skynet_socket_message {
  skynet_socket_type type;
  int id;
  int ud;
  string_buf buffer;
};

enum class socket_type : int {
  invalid = 0,
  reserve = 1,
  plisten = 2,
  listen = 3,
  connecting = 4,
  connected = 5,
  halfclose_read = 6,
  halfclose_write = 7,
  paccept = 8,
  bind = 9,
};

struct socket_t {
  uintptr_t opaque;
  int id = -1;

  using tcp_ns = asio::ip::tcp;
  using udp_ns = asio::ip::udp;
  std::variant<std::monostate, tcp_ns::socket, tcp_ns::acceptor, udp_ns::socket>
      sock;

  tcp_ns::socket *tcp() { return std::get_if<tcp_ns::socket>(&sock); }

  tcp_ns::acceptor *acceptor() { return std::get_if<tcp_ns::acceptor>(&sock); }

  udp_ns::socket *udp() { return std::get_if<udp_ns::socket>(&sock); }

  std::atomic<socket_type> type{socket_type::invalid};

  // write list
  std::deque<std::string> wlist;
  spinlock_mutex wlist_lock;

  // read buffer
  size_t rsize;

  bool close = false;

  void clear() {
    {
      wlist_lock.lock();
      wlist.clear();
      wlist_lock.unlock();
    }

    opaque = 0;
    id = -1;

    rsize = 0;

    close = false;

    sock = std::monostate{};
  }
};

struct server {
  asio::io_context poll;
  std::vector<socket_t> slots;
  std::atomic_size_t alloc;
  spinlock_mutex lock;

  server() : poll{1}, alloc{1}, slots{65535} {}

  int new_fd();
  auto &get_slot(int id);

  int socket_listen(context_t *ctx, const char *host, int port, int backlog);
  int socket_connect(context_t *ctx, const char *host, int port);
  void socket_start(context_t *ctx, int id);
  int socket_send(context_t *ctx, int id, std::string buf);
  void close_socket(context_t *ctx, int id);
  void shutdown_socket(context_t *ctx, int id);

  void forward_message(skynet_socket_type type, socket_message &result);

  static server &ins() {
    static server s;
    return s;
  }
};

} // namespace cpp_sk