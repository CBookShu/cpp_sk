#include <string>
#include <iguana/iguana.hpp>
#include <iguana/prettify.hpp>
#include <string_view>
#include "cpp_sk.h"
#include "logger.h"
#include "utils.h"


class test : public cpp_sk::module_base_t {
  virtual bool init(cpp_sk::context_ptr_t &ctx, std::string_view param) override {
    ctx->cb = &test::cb;
    ctx->ud = this;
    return true;
  };

  static int cb(cpp_sk::context_t * context, int type, int session, uint32_t source , const void * msg, size_t sz) {
    if (type ==cpp_sk::PTYPE_TEXT) {
      std::cout << std::string_view((char*)msg, sz) << std::endl;
    } else if(type == cpp_sk::PTYPE_RESPONSE) {
      std::cout << "timeout" << std::endl;
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