#include <any>
#include <atomic>
#include <cassert>
#include <deque>
#include <string>
#include <iguana/iguana.hpp>
#include <iguana/prettify.hpp>
#include <string_view>
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "cpp_sk.h"
#include "logger.h"
#include "sk_socket.h"
#include "utils.h"


class test : public cpp_sk::module_base_t {
  int s_id = -1;
  int c_id = -1;

  virtual bool init(cpp_sk::context_ptr_t &ctx, std::string_view param) override {
    ctx->cb = &test::cb;
    ctx->ud = this;

    s_id = cpp_sk::server::ins().socket_listen(ctx.get(), "127.0.0.1", 8888, 1024);
    c_id = cpp_sk::server::ins().socket_connect(ctx.get(), "127.0.0.1", 8888);
    return true;
  };

  static int cb(cpp_sk::context_t * context, int type, int session, uint32_t source , const void * msg, size_t sz) {
    auto *t = std::any_cast<test*>(context->ud);
    if (type ==cpp_sk::PTYPE_TEXT) {
      std::cout << std::string_view((char*)msg, sz) << std::endl;
    } else if(type == cpp_sk::PTYPE_RESPONSE) {
      std::cout << "timeout" << std::endl;
    } else if(type == cpp_sk::PTYPE_SOCKET) {
      cpp_sk::skynet_socket_message* m = (cpp_sk::skynet_socket_message*)msg;
      std::cout << "socket" << std::endl;
      if (m->buffer) {
        std::cout << m->buffer << std::endl;
      }
      if (m->type == cpp_sk::skynet_socket_type::connect) {
        if(m->id == t->s_id) {
          cpp_sk::server::ins().socket_start(context, m->id);
        } else if(m->id == t->c_id) {
          // cpp_sk::server::ins().socket_start(context, m->id);
        }
      } else if(m->type == cpp_sk::skynet_socket_type::accept) {
        std::cout << "new id:" << m->ud << std::endl;
      }

      algo::safe_free(m->buffer);
    }
    return 0;
  }
};

int main(int argc, char** argv) {
  using namespace cpp_sk;
  config_t config;
  const char* config_path = "config.json";
  if (argc == 2) {
    config_path = argv[1];
  }
  config.read("config.json");
  module_t::register_module_func("logger", module_t::creator<logger>::create);
  module_t::register_module_func("test", module_t::creator<test>::create);
  skynet_app::start(config);
  return 0;
}