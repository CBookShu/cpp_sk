#pragma once
#include "cpp_sk.h"
#include <string_view>

namespace cpp_sk {

struct logger : public module_base_t {
  virtual std::string_view name() const override {
    return "logger";
  };
  virtual bool init(context_ptr_t &ctx, std::string_view) override {
    ctx->cb = &cb;
    ctx->ud = this;
    return true;
  };
  virtual void signal(int) override {

  };

  static int cb(struct context_t * context, int type, int session, uint32_t source , const void * msg, size_t sz) {
    if (type == PTYPE_TEXT) {
      std::cout << std::string_view((char*)msg, sz) << std::endl;
    } else if(type == PTYPE_RESPONSE) {
      std::cout << "timeout" << std::endl;
    }
    return 0;
  }
};


} // namespace cpp_sk