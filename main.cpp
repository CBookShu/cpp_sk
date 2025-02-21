#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "cpp_sk.h"
#include "logger.h"
#include "sk_socket.h"
#include "utils.h"
#include "ylt/coro_rpc/impl/default_config/coro_rpc_config.hpp"
#include "ylt/struct_pack.hpp"
#include "ylt/coro_rpc/coro_rpc_server.hpp"
#include <any>
#include <atomic>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <iguana/iguana.hpp>
#include <iguana/prettify.hpp>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include "cpp_rpc.h"

namespace cpp_sk {

class pingpong_server : public cpp_rpc::cpp_rpc_service {
public:
  virtual bool init(cpp_sk::context_ptr_t &ctx,
                    std::string_view param) override {
    module_mid_t::init(ctx, param);

    register_name("pingpong_server", ctx->handle);

    timeout(ctx.get(), 50, [this, ctx = ctx.get()]() {
      std::string s = "hello";
      send_request<int, std::string>(
          "pingpong_client", "cpp_sk::pingpong_client::rpc_call_1_2",
          [](cpp_rpc::rpc_result r) {
            int res = r.as<int>();
            log("rpc_call_1_2 rsp res:{}", res);
          },
          10, s);
      send_request<int>(
          "pingpong_client", "cpp_sk::pingpong_client::rpc_call_0_1",
          [](cpp_rpc::rpc_result r) {
            assert(r.result()->len == 0);
          },
          10);
      send_request<std::string>(
          "pingpong_client", "cpp_sk::pingpong_client::rpc_call_1_1",
          [](cpp_rpc::rpc_result r) {
            int res = r.as<int>();
            assert(res == 1);
            log("rpc_call_1_1 rsp res:{}", res);
          },
          "hello world");
    });

    listen(ctx.get(), "0.0.0.0", 8888, 1024, [this, ctx = ctx.get()](int id) {
      start_accept(ctx, id, [this, ctx](int id) {
        start_tcp(ctx, id, [this, ctx](int id, std::string_view data) {
          log("id:{} data:{}", id, data);
          send_buffer(ctx, id, std::string(data.data(), data.size()));
          return data.size();
        });
      });
    });

    new_service("pingpong_client", "");
    return true;
  }
};

class pingpong_client : public cpp_rpc::cpp_rpc_service {
public:
  int rpc_call_1_2(int a, std::string s) {
    log("rpc_call_1_2 a:{}, s:{}", a, s);
    return 0;
  }
  void rpc_call_0_1(int a) { log("rpc_call_0_1 a:{}", a); }
  void rpc_call_0_0() { log("rpc_call_0_0"); }
  void rpc_call_0_2(int a, std::string b) {
    log("rpc_call_0_2 a:{}, b:{}", a, b);
  }
  int rpc_call_1_0() { return 1; }
  int rpc_call_1_1(std::string s) {
    log("rpc_call_1_1 s:{}", s);
    return 1;
  }

  virtual bool init(cpp_sk::context_ptr_t &ctx,
                    std::string_view param) override {
    module_mid_t::init(ctx, param);

    register_name("pingpong_client", ctx->handle);
    register_rpc_func<&pingpong_client::rpc_call_1_2>(this);
    register_rpc_func<&pingpong_client::rpc_call_0_1>(this);
    register_rpc_func<&pingpong_client::rpc_call_0_0>(this);
    register_rpc_func<&pingpong_client::rpc_call_0_2>(this);
    register_rpc_func<&pingpong_client::rpc_call_1_0>(this);
    register_rpc_func<&pingpong_client::rpc_call_1_1>(this);

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

      timeout(ctx, 300, [this, ctx, fd]() { close_socket(ctx, fd); });
    });
    return true;
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
  module_t::register_module_func<logger>("logger");
  module_t::register_module_func<pingpong_server>("pingpong_server");
  module_t::register_module_func<pingpong_client>("pingpong_client");
  skynet_app::start(config);
  return 0;
}