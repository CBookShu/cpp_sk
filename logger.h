#pragma once
#include "cpp_sk.h"
#include <string_view>

namespace cpp_sk {

struct logger : public module_base_t {
  FILE *fp = nullptr;

  ~logger() {
    if (fp) {
      fclose(fp);
    }
  }

  virtual bool init(context_ptr_t &ctx, std::string_view param) override {
    ctx->cb = &cb;
    ctx->ud = this;
    if (param.size() > 0) {
      fp = fopen(param.data(), "a");
    }
    return true;
  };

  static int cb(struct context_t *context, int type, int session,
                uint32_t source, std::any &a, size_t sz) {
    auto *log = std::any_cast<logger *>(context->ud);
    if (type == PTYPE_TEXT) {
      auto s = std::any_cast<std::string>(&a);
      FILE *fp = log->fp ? log->fp : stdout;

      auto now = timer::ins().now();
      auto zone = std::chrono::current_zone();
      auto current_point = std::chrono::system_clock::now();
      auto timefmt =
          std::format("{}", std::chrono::zoned_time{zone, current_point});
      auto output =
          std::format("[{}][{:08x}]{}\n",
                      std::chrono::zoned_time{zone, current_point}, source, *s);
      if (log->fp) {
        fwrite(output.c_str(), output.size(), 1, log->fp);
        fflush(log->fp);
      }
      fwrite(output.c_str(), output.size(), 1, stdout);
      fflush(stdout);
    } else if (type == PTYPE_RESPONSE) {
      std::cout << "timeout" << std::endl;
    }
    return 0;
  }
};

} // namespace cpp_sk