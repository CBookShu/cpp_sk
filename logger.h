#pragma once
#include "cpp_sk.h"
#include <string_view>

namespace cpp_sk {

struct logger : public module_mid_t {
  FILE *fp = nullptr;

  ~logger() {
    if (fp) {
      fclose(fp);
    }
  }

  virtual bool init(context_ptr_t &ctx, std::string_view param) override {
    module_mid_t::init(ctx, param);
    if (param.size() > 0) {
      fp = fopen(param.data(), "a");
    }
    return true;
  };

  virtual void on_text(cpp_sk::context_t *context, int session, uint32_t source,
                       std::string &str) override {
    auto now = timer::ins().now();
    auto zone = std::chrono::current_zone();
    auto current_point = std::chrono::system_clock::now();
    auto timefmt =
        std::format("{}", std::chrono::zoned_time{zone, current_point});
    auto output =
        std::format("[{}][{:08x}]{}\n",
                    std::chrono::zoned_time{zone, current_point}, source, str);
    if (fp) {
      fwrite(output.c_str(), output.size(), 1, fp);
      fflush(fp);
    }
    fwrite(output.c_str(), output.size(), 1, stdout);
    fflush(stdout);
  }
};

} // namespace cpp_sk