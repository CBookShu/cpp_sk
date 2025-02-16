#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "cpp_sk.h"
#include "logger.h"
#include "sk_socket.h"
#include "utils.h"
#include <any>
#include <atomic>
#include <cassert>
#include <deque>
#include <iguana/iguana.hpp>
#include <iguana/prettify.hpp>
#include <string>
#include <string_view>

namespace cpp_sk {
class test : public cpp_sk::module_base_t {
  struct socket_node_t {
    int id = -1;
    bool connectd = false;
    std::string rbuff;

    std::function<void(int)> cb;
    std::function<size_t(int, std::string_view)> on_data;
  };

  std::unordered_map<int, socket_node_t> socket_nodes;
  std::unordered_map<int, std::function<void()>> timeout_nodes;

  int timeout(cpp_sk::context_t *ctx, int timeout, std::function<void()> cb) {
    auto id = ctx->newsession();
    if (auto ret =
            cpp_sk::timer::timer::ins().timeout(ctx->handle, timeout, id);
        id == ret) {
      timeout_nodes[id] = std::move(cb);
      return id;
    }
    return -1;
  }

  template <typename F>
  int listen(cpp_sk::context_t *ctx, std::string_view ip, int port, int backlog,
             F &&f) {
    auto id = cpp_sk::server::ins().socket_listen(ctx, "0.0.0.0", 8888, 1024);
    if (id < 0) {
      return id;
    }
    socket_nodes[id] = {.cb = std::move(f)};
    return id;
  }
  template <typename F>
  void start_accept(cpp_sk::context_t *ctx, int id, F &&f) {
    if (auto it = socket_nodes.find(id); it != socket_nodes.end()) {
      if (it->second.connectd) {
        it->second.cb = std::move(f);
        cpp_sk::server::ins().socket_start(ctx, id);
      }
    }
  }

  template <typename F>
  int connect(cpp_sk::context_t *ctx, std::string_view ip, int port, F &&f) {
    int id = cpp_sk::server::ins().socket_connect(ctx, ip.data(), port);
    if (id < 0) {
      return id;
    }
    socket_nodes[id] = {.cb = std::move(f)};
    return id;
  }

  template <typename F> void start_tcp(cpp_sk::context_t *ctx, int id, F &&f) {
    if (auto it = socket_nodes.find(id); it != socket_nodes.end()) {
      if (it->second.connectd) {
        it->second.on_data = std::move(f);
        cpp_sk::server::ins().socket_start(ctx, id);
      }
    }
  }

  int send_buffer(cpp_sk::context_t *ctx, int id, std::string buf) {
    if (auto it = socket_nodes.find(id); it != socket_nodes.end()) {
      return cpp_sk::server::ins().socket_send(ctx, id, std::move(buf));
    }
    return -1;
  }

  static void close_socket(cpp_sk::context_t *ctx, int id) {
    cpp_sk::server::ins().close_socket(ctx, id);
  }

  template <typename... Args>
  static void log(std::format_string<Args...> fmt, Args &&...args) {
    cpp_sk::skynet_app::error(nullptr, std::move(fmt),
                              std::forward<Args>(args)...);
  }

  virtual bool init(cpp_sk::context_ptr_t &ctx,
                    std::string_view param) override {
    ctx->cb = &test::cb;
    ctx->ud = this;
    listen(ctx.get(), "0.0.0.0", 8888, 1024, [this, ctx = ctx.get()](int id) {
      start_accept(ctx, id, [this, ctx](int id) {
        start_tcp(ctx, id, [](int id, std::string_view data) {
          log("id:{} data:{}", id, data);
          return data.size();
        });
      });
    });
    auto fd =
        connect(ctx.get(), "127.0.0.1", 8888, [this, ctx = ctx.get()](int id) {
          start_tcp(ctx, id, [](int id, std::string_view data) {
            log("id:{} data:{}", id, data);
            return data.size();
          });
        });
    timeout(ctx.get(), 200, [this, ctx = ctx.get(), fd]() {
      log("start send buff");
      send_buffer(ctx, fd, "hello world");
    });
    return true;
  };

  static int cb(cpp_sk::context_t *context, int type, int session,
                uint32_t source, std::any &a, size_t sz) {
    auto *t = std::any_cast<test *>(context->ud);
    if (type == cpp_sk::PTYPE_TEXT) {
      auto *s = std::any_cast<std::string>(&a);
      log("test: {}", *s);
    } else if (type == cpp_sk::PTYPE_RESPONSE) {
      if (auto it = t->timeout_nodes.find(session);
          it != t->timeout_nodes.end()) {
        std::function<void()> cb = std::move(it->second);
        t->timeout_nodes.erase(it);
        cb();
      }
    } else if (type == cpp_sk::PTYPE_SOCKET) {
      cpp_sk::skynet_socket_message *m =
          std::any_cast<cpp_sk::skynet_socket_message>(&a);
      if (auto it = t->socket_nodes.find(m->id); it != t->socket_nodes.end()) {
        if (m->type == cpp_sk::skynet_socket_type::connect) {
          if (!it->second.connectd) {
            it->second.connectd = true;
            if (it->second.cb) {
              auto cb = std::move(it->second.cb);
              cb(m->id);
            }
          } else {
          }
        } else if (m->type == cpp_sk::skynet_socket_type::close) {
          t->socket_nodes.erase(it);
        } else if (m->type == cpp_sk::skynet_socket_type::error) {
          t->close_socket(context, m->id);
        } else if (m->type == cpp_sk::skynet_socket_type::accept) {
          int newid = m->ud;
          t->socket_nodes[newid] = {.id = m->ud, .connectd = true};
          if (it->second.cb) {
            it->second.cb(m->ud);
          }
        } else if (m->type == cpp_sk::skynet_socket_type::data) {
          it->second.rbuff.append(m->buffer.ptr(), m->buffer.size());
          if (it->second.on_data) {
            auto sz = it->second.on_data(it->second.id, it->second.rbuff);
            if (sz > 0) {
              it->second.rbuff = it->second.rbuff.substr(sz);
            }
          }
        }
      } else {
        t->close_socket(context, m->id);
      }
    }
    return 0;
  }
};

} // namespace cpp_sk

int main(int argc, char **argv) {
  using namespace cpp_sk;
  config_t config;
  const char *config_path = "config.json";
  if (argc == 2) {
    config_path = argv[1];
  }

  config.read(config_path);
  module_t::register_module_func("logger", module_t::creator<logger>::create);
  module_t::register_module_func("test", module_t::creator<test>::create);
  skynet_app::start(config);
  return 0;
}