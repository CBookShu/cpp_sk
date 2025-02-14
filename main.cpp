#include <string>
#include <iguana/iguana.hpp>
#include <iguana/prettify.hpp>
#include "cpp_sk.h"
#include "logger.h"
#include "logger.h"


int main() {
  using namespace cpp_sk;
  config_t config;
  config.read("config.json");
  module_t::register_module_func("logger", module_t::creator<logger>::create);
  skynet_app::start(config);
  return 0;
}