#pragma once

#include "cpp_sk.h"
#include "ylt/struct_pack.hpp"
#include "ylt/struct_pack/md5_constexpr.hpp"
#include "ylt/util/function_name.h"
#include "ylt/util/type_traits.h"
#include <cstddef>
#include <cstdint>
#include <forward_list>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cpp_sk::cpp_rpc {
static constexpr char MagicNum = 0x25;

enum rpc_type {
  req_res,
};

struct rpc_header_t {
  char magic;
  char type;
  uint16_t reserved;

  uint32_t funcid;
  uint32_t len;
};

struct rpc_result_t {
  char status;
  char reserved[3];
  uint32_t len;
};

struct rpc_result {
  std::string_view data;

  const rpc_result_t *result() {
    if (data.size() < sizeof(rpc_result_t)) {
      return nullptr;
    }
    return (rpc_result_t *)(data.data());
  }

  bool has_error() {
    auto r = result();
    return !r || r->status != 0;
  }

  bool has_value() {
    auto r = result();
    return r && r->len > 0;
  }

  template <typename R> R as() {
    if (!has_value()) {
      if (auto h = result(); h) {
        throw std::runtime_error(
            std::format("as type:{} error:{}", typeid(R).name(), h->status));
      } else {
        throw std::runtime_error(
            std::format("as type:{} error", typeid(R).name()));
      }
    }
    R r{};
    size_t offset = sizeof(rpc_result_t);
    auto e = struct_pack::deserialize_to_with_offset(r, data, offset);
    if (e) {
      throw std::runtime_error(e.message().data());
    }
    return r;
  }
};

template <auto func, typename Stream, typename... Args>
inline void rpc_encode(Stream &out, uint32_t session, Args &&...args) {

  constexpr auto name = coro_rpc::get_func_name<func>();
  constexpr auto id =
      struct_pack::MD5::MD5Hash32Constexpr(name.data(), name.length());
  using func_type = util::function_traits<decltype(func)>;
  using class_type = typename func_type::class_type;
  using param_type = typename func_type::parameters_type;
  using check_t = decltype(std::invoke(func, std::declval<class_type>(),
                                       std::forward<Args>(args)...));

  out.resize(sizeof(rpc_header_t));
  if constexpr (sizeof...(Args) > 0) {
    struct_pack::serialize_to(out, std::forward<Args>(args)...);
  }

  rpc_header_t *header = reinterpret_cast<rpc_header_t *>(out.data());
  header->magic = MagicNum;
  header->type = req_res;
  header->funcid = id;
  header->len = out.size() - sizeof(rpc_header_t);
}

template <typename fArg, typename Arg>
inline decltype(auto) helper_wraper_value(Arg &arg) {
  if constexpr (std::is_same_v<std::decay_t<fArg>, std::decay_t<Arg>>) {
    return (arg);
  } else {
    return fArg{arg};
  }
}

template <typename... pArgs, typename... Args, typename Stream>
inline auto helper_pack_args(Stream &out, Args &&...args) {
  return struct_pack::serialize_to(out, helper_wraper_value<pArgs>(args)...);
}

template <typename... fArgs, typename Stream, typename... pArgs>
inline void rpc_encode(Stream &out, std::string_view name, pArgs &&...args) {
  auto id = struct_pack::MD5::MD5Hash32Constexpr(name.data(), name.length());
  auto f = [](fArgs... fargs) { return 0; };
  using check_t = decltype(f(std::forward<pArgs>(args)...));

  out.resize(sizeof(rpc_header_t));
  if constexpr (sizeof...(pArgs) > 0) {
    helper_pack_args<fArgs...>(out, std::forward<pArgs>(args)...);
  }

  rpc_header_t *header = reinterpret_cast<rpc_header_t *>(out.data());
  header->magic = MagicNum;
  header->type = req_res;
  header->funcid = id;
  header->len = out.size() - sizeof(rpc_header_t);
}

template <typename Stream>
inline const rpc_header_t *rpc_decode_header(Stream &out) {
  if (out.size() < sizeof(rpc_header_t)) {
    return nullptr;
  }
  rpc_header_t *header = reinterpret_cast<rpc_header_t *>(out.data());
  if (header->magic != MagicNum) {
    return nullptr;
  }
  if (header->len < (out.size() - sizeof(rpc_header_t))) {
    return nullptr;
  }
  return header;
}

template <auto func, typename Self, typename Stream>
inline void rpc_call_func(Stream &in, std::string &out, Self *self) {
  std::string_view str(in.data() + sizeof(rpc_header_t),
                       in.size() - sizeof(rpc_header_t));
  using T = decltype(func);
  using return_type = util::function_return_type_t<T>;
  using param_type = util::function_parameters_t<T>;

  int status = 0;
  if constexpr (std::is_void_v<return_type>) {
    if constexpr (std::is_void_v<param_type>) {
      std::apply(func, std::forward_as_tuple(*self));
    } else {
      param_type args{};
      if constexpr (std::tuple_size_v<param_type> == 1) {
        if (struct_pack::deserialize_to(std::get<0>(args), str)) {
          status = -3; // param parse error
        }
      } else {
        if (struct_pack::deserialize_to(args, str)) {
          status = -3; // param parse error
        }
      }
      if (status == 0) {
        std::apply(func, std::tuple_cat(std::forward_as_tuple(*self),
                                        std::move(args)));
      }
    }
  } else {
    if constexpr (std::is_void_v<param_type>) {
      struct_pack::serialize_to(out,
                                std::apply(func, std::forward_as_tuple(*self)));
    } else {
      param_type args{};
      if constexpr (std::tuple_size_v<param_type> == 1) {
        if (struct_pack::deserialize_to(std::get<0>(args), str)) {
          status = -3;
        }
      } else {
        if (struct_pack::deserialize_to(args, str)) {
          status = -3;
        }
      }
      if (status == 0) {
        struct_pack::serialize_to(
            out, std::apply(func, std::tuple_cat(std::forward_as_tuple(*self),
                                                 std::move(args))));
      }
    }
  }

  rpc_result_t *result = reinterpret_cast<rpc_result_t *>(out.data());
  result->status = status;
  result->len = out.size() - sizeof(rpc_result_t);
}

class cpp_rpc_service : public cpp_sk::module_mid_t {
public:
  std::unordered_map<uint32_t,
                     std::function<void(std::string_view, std::string &out)>>
      cpp_rpc_router;

  template <auto F, typename Self> void register_rpc_func(Self *self) {
    constexpr auto name = coro_rpc::get_func_name<F>();
    constexpr auto id =
        struct_pack::MD5::MD5Hash32Constexpr(name.data(), name.length());

    cpp_rpc_router[id] = [self](std::string_view data, std::string &out) {
      return rpc_call_func<F>(data, out, self);
    };
  }

  // return rpc_result_t + func:ret
  std::string on_rpc_call(std::string &s) {
    std::string out;
    out.resize(sizeof(rpc_result_t));
    rpc_result_t *result = reinterpret_cast<rpc_result_t *>(out.data());

    auto *header = rpc_decode_header(s);
    if (!header) {
      result->status = -1; // header parse error
      return "";
    }

    if (auto it = cpp_rpc_router.find(header->funcid);
        it != cpp_rpc_router.end()) {
      it->second(s, out);
    } else {
      result->status = -2;
    }
    return out;
  }

  template <typename... fArgs, typename... Args>
  bool send_request(handle_t handle, std::string_view cmd, auto f,
                    Args &&...args) {
    auto ctx = handle_storage_t::ins().handle_grab(handle);
    if (!ctx) {
      return false;
    }

    std::string out;
    rpc_encode<fArgs...>(out, cmd, std::forward<Args>(args)...);

    auto self_ctx = wctx.lock();
    skynet_message msg;
    msg.source = self_ctx->handle;
    msg.session = self_ctx->newsession();
    msg.t = cpp_sk::PTYPE_RESERVED_CPP;
    msg.data = std::move(out);

    response_cbs[msg.session] = [f = std::move(f)](std::string_view data) {
      rpc_result result(data);
      f(result);
    };

    return skynet_app::context_push(handle, msg);
  }

  template <typename... fArgs, typename... Args>
  bool send_request(std::string_view name, std::string_view cmd, auto f,
                    Args &&...args) {
    auto handle = handle_storage_t::ins().handle_finename(name.data());
    return send_request<fArgs...>(handle, cmd, f, std::forward<Args>(args)...);
  }

  virtual void on_rpc_cpp(cpp_sk::context_t *context, int session,
                          uint32_t source, std::string &in) {
    auto res = on_rpc_call(in);
    skynet_message msg;
    msg.source = context->handle;
    msg.data = std::move(res);
    msg.t = PTYPE_RESPONSE;
    msg.session = session;
    skynet_app::context_push(source, msg);
  }
};

} // namespace cpp_sk::cpp_rpc