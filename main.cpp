#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "cpp_sk.h"
#include "logger.h"
#include "sk_socket.h"
#include "utils.h"
#include "ylt/struct_pack.hpp"
#include <any>
#include <atomic>
#include <cassert>
#include <cmath>
#include <deque>
#include <iguana/iguana.hpp>
#include <iguana/prettify.hpp>
#include <string>
#include <string_view>
#include <tuple>

namespace cpp_sk {

class pingpong_server : public module_mid_t {
public:
  virtual bool init(cpp_sk::context_ptr_t &ctx,
                    std::string_view param) override {
    module_mid_t::init(ctx, param);

    register_name("pingpong_server", ctx->handle);

    timeout(ctx.get(), 50, [this, ctx = ctx.get()]() {
      std::string s = "hello";
      send_request(
          "pingpong_client", "cpp_sk::pingpong_client::rpc_call_1_2",
          [](std::string_view s) {
            int res;
            if (!struct_pack::deserialize_to(res, s)) {
              log("rpc_call_1_2 rsp res:{}", res);
            }
          },
          10, s);
      send_request(
          "pingpong_client", "cpp_sk::pingpong_client::rpc_call_0_1",
          [](std::string_view s) {
            assert(s.empty());
            log("rpc_call_0_1 rsp empty");
          },
          10);
      send_request(
          "pingpong_client", "cpp_sk::pingpong_client::rpc_call_1_1",
          [](std::string_view s) {
            int res;
            if (!struct_pack::deserialize_to(res, s)) {
              assert(res == 1);
              log("rpc_call_1_1 rsp res:{}", res);
            }
          },
          std::string("hello world"));
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

class pingpong_client : public module_mid_t {
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
  module_t::register_module_func("logger", module_t::creator<logger>::create);
  module_t::register_module_func("pingpong_server",
                                 module_t::creator<pingpong_server>::create);
  module_t::register_module_func("pingpong_client",
                                 module_t::creator<pingpong_client>::create);
  skynet_app::start(config);
  return 0;
}