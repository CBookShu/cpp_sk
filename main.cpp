#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <iguana/iguana.hpp>
#include <iguana/prettify.hpp>
#include <string_view>
#include <utility>

#include "cpp_sk.h"
#include "iguana/json_reader.hpp"
#include "ylt/reflection/template_string.hpp"

int main() {
  cpp_sk::config_t config;
  config.read("config.json");
  cpp_sk::skynet_server::start(config);
  return 0;
}