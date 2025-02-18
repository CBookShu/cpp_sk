#pragma once
#include "iguana/json_reader.hpp"
#include <tuple>
#include <ylt/struct_pack.hpp>
#include <ylt/util/function_name.h>
#include <ylt/util/type_traits.h>
#include "sk_socket.h"
#include "utils.h"
#include <any>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <ratio>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpp_sk {

struct config_t {
  int thread = std::thread::hardware_concurrency();
  int harbor = 1;
  bool profile = 1;
  std::string daemon;
  std::string module_path;
  std::string bootstrap;
  std::string logger;
  std::string logservice;

  void read(const char *config_path) {
    try {
      auto path = std::filesystem::current_path().append(config_path);
      iguana::from_json_file(*this, path.string());

      if (logservice.empty()) {
        logservice = "logger";
      }
    } catch (std::exception &e) {
      std::cerr << e.what() << std::endl;
    }
  }
};

struct node_t {
  std::atomic_int total{0};
  bool init{false};
  uint32_t monitor_exit;
  bool profile = true;

  static node_t &ins() {
    static node_t N;
    return N;
  }
};

struct handle_t {
  static constexpr uint32_t HANDLE_MASK = 0xffffff;
  static constexpr uint32_t HANDLE_REMOTE_SHIFT = 24;

  uint32_t handle;
  handle_t(uint32_t h = 0) : handle(h) {}
  handle_t(uint32_t h, uint32_t habor) { handle = h | habor; }

  void check_range() {
    if (handle > HANDLE_MASK) {
      handle = 1;
    }
  }

  operator uint32_t() { return handle; }

  operator uint32_t() const { return handle; }

  handle_t operator|=(uint32_t harbor) {
    handle |= harbor;
    return *this;
  }

  uint32_t operator++() {
    if (++handle > HANDLE_MASK) {
      handle = 1;
    }
    return handle;
  }

  uint32_t operator++(int) {
    auto r = handle;
    if (++handle > HANDLE_MASK) {
      handle = 1;
    }
    return r;
  }
};

// handle: 高8位设置harbor; 低24位是实际的handle 值
// constexpr uint32_t HANDLE_MASK = 0xffffff;
// constexpr uint32_t HANDLE_REMOTE_SHIFT = 24;

// msgsize: 高8位设置type; 32位平台 size 剩余24; 64位平台剩余56
constexpr size_t MESSAGE_TYPE_MASK = std::numeric_limits<size_t>::max() >> 8;
constexpr size_t MESSAGE_TYPE_SHIFT = ((sizeof(size_t) - 1) * 8);

constexpr size_t PTYPE_TAG_DONTCOPY = 0x10000;
constexpr size_t PTYPE_TAG_ALLOCSESSION = 0x20000;
enum PTYPE {
  PTYPE_TEXT = 0,
  PTYPE_RESPONSE = 1,
  PTYPE_MULTICAST = 2,
  PTYPE_CLIENT = 3,
  PTYPE_SYSTEM = 4,
  PTYPE_HARBOR = 5,
  PTYPE_SOCKET = 6,
  PTYPE_ERROR = 7,
  PTYPE_RESERVED_QUEUE = 8,
  PTYPE_RESERVED_DEBUG = 9,
  PTYPE_RESERVED_LUA = 10,
  PTYPE_RESERVED_SNAX = 11,
  PTYPE_RESERVED_CPP = 12,

  // 不会组合到size的位
  PTYPE_DONTCOPY = 16,
  PTYPE_ALLOCSESSION = 17,
};

struct skynet_message {
  uint32_t source = 0;
  int session = 0;
  std::any data;
  // [sizeof(size_t)-8, sizeof(size_t)) type
  // [0, sizeof(size_t)-8) data len
  PTYPE t;
  

  ~skynet_message() { }

  void reserved_data() {
    t = PTYPE::PTYPE_TEXT;
  }
};

struct message_queue;
struct global_queue {
private:
  struct message_queue *head;
  struct message_queue *tail;
  struct spinlock_mutex lock;

public:
  global_queue();

  static global_queue &ins() {
    static global_queue Q;
    return Q;
  }

  void push(message_queue *queue);

  message_queue *pop();
};

struct message_queue {
  spinlock_mutex lock;
  handle_t handle;
  int head;
  int tail;
  bool release;
  bool in_global;
  int overload;
  int overload_threshold;
  std::deque<skynet_message> queue;
  struct message_queue *next;

protected:
  template <typename F>
    requires requires(F &&f, skynet_message *m) { f(m); }
  void _drop_queue(F &&f) {
    while (auto msg = mq_pop()) {
      std::forward<F>(f)(std::addressof(msg.value()));
    }
  }

public:
  message_queue(handle_t handle_);
  ~message_queue();

  size_t length();
  int mq_overload();
  void mq_push(skynet_message &message);
  std::optional<skynet_message> mq_pop();

  void mark_release();

  template <typename F>
    requires requires(F f, skynet_message *m) { f(m); }
  void mq_release(F &&f) {
    std::unique_lock guard(lock);
    if (release) {
      guard.unlock();
      _drop_queue(std::forward<F>(f));
    } else {
      global_queue::ins().push(this);
      guard.unlock();
    }
  }
};

struct context_t;
using context_ptr_t = std::shared_ptr<context_t>;

struct module_base_t {
  virtual ~module_base_t() {};
  virtual bool init(context_ptr_t &, std::string_view) { return true; };
  virtual void signal(int) {};
};

struct module_t {
  using creator_func_t = module_base_t *(*)();
  static std::unordered_map<std::string, creator_func_t, string_hash,
                            std::equal_to<>>
      gMap;

  static void register_module_func(std::string name, creator_func_t func);
  static module_base_t *create(std::string_view name);

  template <typename M> struct creator {
    static_assert(std::is_base_of_v<module_base_t, M>, "M no support");

    static module_base_t *create() {
      auto *m = new M();
      return static_cast<module_base_t *>(m);
    }
  };
};

struct context_t {
  typedef int (*skynet_cb)(struct context_t *context, int type, int session,
                           uint32_t source, std::any &a);

  std::unique_ptr<module_base_t> instance;
  std::any ud;
  skynet_cb cb;
  std::unique_ptr<message_queue> queue;
  uint64_t cpu_cost;
  uint64_t cpu_start;
  std::string result;
  handle_t handle;
  int session_id;
  size_t message_count;
  bool init = false;
  bool endless = false;
  bool profile = true;

  context_t();
  ~context_t();

  static context_ptr_t create(std::string_view name, std::string_view param);

  void dispatch(skynet_message &msg);
  int newsession() {
    int session = ++session_id;
    if (session_id <= 0) {
      session_id = 1;
      return 1;
    }
    return session;
  }
};

struct handle_storage_t {
public:
  handle_storage_t(int harbor_);

  handle_t handle_register(context_ptr_t &ctx);

  bool handle_retire(handle_t handle);

  void handle_retireall();

  context_ptr_t handle_grab(handle_t handle);

  handle_t handle_finename(const char *name_);

  bool handle_namehandle(handle_t handle, const char *name_);

  static void init(int harbor_) {
    static std::atomic_bool flag{false};
    bool ok = false;
    if (!flag.compare_exchange_strong(ok, true)) {
      return;
    }
    gH = std::make_unique<handle_storage_t>(harbor_);
  }
  static handle_storage_t &ins() { return *gH; }

private:
  static std::unique_ptr<handle_storage_t> gH;
  std::shared_mutex lock;
  uint32_t harbor;
  handle_t handle_index = 1;
  std::vector<std::shared_ptr<context_t>> slot;
  std::unordered_map<std::string, uint32_t, string_hash, std::equal_to<>> name;
};

struct harbor_t {
public:
  static harbor_t &ins() {
    static harbor_t H;
    return H;
  }

  void init(int harbor_) {
    harbor = (unsigned int)harbor_ << handle_t::HANDLE_REMOTE_SHIFT;
  }

  void start(context_ptr_t &ctx) { remote = ctx; }

  void exit() { remote.reset(); }

  bool message_isremote(handle_t handle) {
    assert(harbor != ~0);
    int h = (handle & ~handle_t::HANDLE_MASK);
    return h != harbor && h != 0;
  }

protected:
  constexpr bool invalid_type(PTYPE type) {
    return type != PTYPE_SYSTEM && type != PTYPE_HARBOR;
  }

private:
  int harbor = ~0;
  context_ptr_t remote;
};

struct skynet_monitor {
  std::atomic_int version = 0;
  int check_version = 0;
  uint32_t source = 0;
  uint32_t destination = 0;

  void check();
  void trigger(uint32_t source, uint32_t destination);
};

struct monitor {
  std::vector<std::unique_ptr<skynet_monitor>> m;
  std::condition_variable cond;
  std::mutex mutex;
  int count = 0;
  int sleep = 0;
  bool quit = false;

  void wakeup(int busy) {
    if (sleep >= (count - busy)) {
      cond.notify_one();
    }
  }
};

struct timer {
  static constexpr uint32_t TIME_NEAR_SHIFT = 8;
  static constexpr uint32_t TIME_NEAR = (1 << TIME_NEAR_SHIFT);
  static constexpr uint32_t TIME_LEVEL_SHIFT = 6;
  static constexpr uint32_t TIME_LEVEL = (1 << TIME_LEVEL_SHIFT);
  static constexpr uint32_t TIME_NEAR_MASK = (TIME_NEAR - 1);
  static constexpr uint32_t TIME_LEVEL_MASK = TIME_LEVEL - 1;

  struct timer_event {
    uint32_t handle = 0;
    int session = 0;
  };

  struct timer_node_event;
  struct timer_node {
    std::unique_ptr<timer_node_event> next;
    uint32_t expire = 0;
  };

  struct timer_node_event : public timer_node, public timer_event {};

  struct link_list {
    struct timer_node head;
    struct timer_node *tail = nullptr;

    link_list() { clear(); }

    std::unique_ptr<timer_node_event> clear() {
      auto ret = std::move(head.next);
      tail = &head;
      return ret;
    }

    void link(std::unique_ptr<timer_node_event> node) {
      auto *ptr = node.get();
      tail->next = std::move(node);
      tail = ptr;
      ptr->next = nullptr;
    }
  };

  struct link_list near_[TIME_NEAR];
  struct link_list t_[4][TIME_LEVEL];
  spinlock_mutex lock_;

  uint32_t time_; // 当前near list 的step

  using clock_t = std::chrono::system_clock;
  using time_point = clock_t::time_point;
  using req_t = std::chrono::seconds::rep;
  using centisecond = std::chrono::duration<req_t, std::centi>;

  uint32_t starttime_;
  uint64_t current_;

  time_point current_point_; // 上次刷新的时间戳
  timer();

  static timer &ins() {
    static timer T;
    return T;
  }

  void update();
  void timer_update();
  void timer_execute();
  void timer_shift();
  void timer_add(std::unique_ptr<timer_node_event> node, int32_t timeout);
  void add_node(std::unique_ptr<timer_node_event> node);
  void move_list(int level, int idx);

  int timeout(handle_t handle, int32_t time, int session);

  void dispatch(std::unique_ptr<timer_node_event> current);
  uint64_t now() { return current_; }
  uint32_t starttime() { return starttime_; }
  time_t ctime() { return starttime_ + current_ / 100; }
};

struct skynet_server {};

#define CHECK_ABORT                                                            \
  if (context_total() == 0)                                                    \
    break;

struct skynet_app {
  template <typename... Args>
  static void error(context_t *context, std::format_string<Args...> fmt,
                    Args &&...args) {
    static handle_t logger = handle_storage_t::ins().handle_finename("logger");
    if (logger == 0) {
      logger = handle_storage_t::ins().handle_finename("logger");
    }

    if (logger == 0) {
      return;
    }
    std::string msg = std::vformat(fmt.get(), std::make_format_args(args...));
    skynet_message smsg;
    if (context == NULL) {
      smsg.source = 0;
    } else {
      smsg.source = context->handle;
    }
    smsg.session = 0;
    smsg.data = std::move(msg);
    smsg.t = PTYPE_TEXT;
    context_push(logger, smsg);
  }

  static bool context_push(handle_t handle, skynet_message &msg);

  static void context_endless(handle_t handle);

  static int context_total() { return node_t::ins().total; }

  static void start_monitor(std::thread &t, monitor &m);

  static void start_timer(std::thread &t, monitor &m);

  static void start_socket(std::thread &t, monitor &m);

  static message_queue *context_message_dispatch(skynet_monitor *m,
                                                 message_queue *q, int weight);

  static void start_worker(std::thread &t, monitor &m, int id, int weight);

  static void start(config_t &config) {
    harbor_t::ins().init(config.harbor);
    handle_storage_t::ins().init(config.harbor);
    global_queue::ins();
    timer::ins();
    server::ins();

    auto ctx = context_t::create(config.logservice, config.logger);
    if (!ctx) {
      std::cerr << std::format("Cant`t launch {} service", config.logservice)
                << std::endl;
      std::terminate();
    }
    handle_storage_t::ins().handle_namehandle(ctx->handle, "logger");

    if (!config.bootstrap.empty()) {
      auto args = algo::split_one(config.bootstrap, " ");
      context_t::create(args.first, args.second);
    }

    std::vector<std::thread> threads(config.thread + 3);
    monitor m;
    m.m.resize(config.thread);
    start_monitor(threads[0], m);
    start_timer(threads[1], m);
    start_socket(threads[2], m);

    static int weight[] = {
        -1, -1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
        2,  2,  2,  2,  2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
    };
    for (int i = 0; i < config.thread; ++i) {
      int w;
      if (i < sizeof(weight) / sizeof(weight[0])) {
        w = weight[i];
      } else {
        w = 0;
      }
      start_worker(threads[3 + i], m, i, w);
    }
    error(nullptr, "wait stop");
    for (auto &t : threads) {
      t.join();
    }
  }
};

class module_mid_t : public cpp_sk::module_base_t {
public:
  std::weak_ptr<context_t> wctx;
  struct socket_node_t {
    int id = -1;
    bool connectd = false;
    std::string rbuff;

    std::function<void(int)> cb;
    std::function<size_t(int, std::string_view)> on_data;
  };

  struct cpp_rpc_param {
    uint32_t funcid;
    std::string buffer;
  };

  std::unordered_map<int, socket_node_t> socket_nodes;
  std::unordered_map<int, std::function<void()>> timeout_nodes;
  std::unordered_map<uint32_t, std::function<std::string(std::string_view)>> cpp_rpc_router;
  std::unordered_map<int, std::function<void(std::string_view)>> cpp_rpc_cbs;

  context_ptr_t self() {
    return wctx.lock();
  }

  static auto new_service(std::string_view name, std::string_view param) {
    return context_t::create(name, param);
  }

  static bool register_name(std::string_view name, handle_t handle) {
    return handle_storage_t::ins().handle_namehandle(handle, name.data());
  }

    template <typename Tp, std::size_t...Idx>
    inline decltype(auto) struck_deserialize_args_imp(std::string_view data, std::index_sequence<Idx...>) {
        return struct_pack::deserialize<std::tuple_element_t<Idx, Tp>...>(data);
    }

    template <typename Tp>
    inline decltype(auto) struck_deserialize_args(std::string_view data) {
        return struck_deserialize_args_imp<Tp>(data, std::make_index_sequence<std::tuple_size_v<Tp>>());
    }

  template <auto F, typename Self>
  void register_rpc_func(Self* self) {
    constexpr auto name = coro_rpc::get_func_name<F>();
    constexpr auto id =
        struct_pack::MD5::MD5Hash32Constexpr(name.data(), name.length());
    cpp_rpc_router[id] = [this,self](std::string_view str) mutable {
        using T = decltype(F);
        using return_type = util::function_return_type_t<T>;
        using param_type = util::function_parameters_t<T>;
        if constexpr(std::is_void_v<return_type>) {
            if constexpr(std::is_void_v<param_type>) {
              std::apply(F, std::forward_as_tuple(*this));
            } else {
              param_type args{};
              if(struct_pack::deserialize_to(args, str)) {
                std::apply(F, std::tuple_cat(std::forward_as_tuple(*this), std::tie(std::move(args))));
              }
            }
            return std::string();
        } else {
          if constexpr(std::is_void_v<param_type>) {
            auto r = std::apply(F, std::forward_as_tuple(*this));
            return struct_pack::serialize<std::string>(std::move(r));
          } else {
            auto args_tp = struck_deserialize_args<param_type>(str);
            if(args_tp) {
                return struct_pack::serialize<std::string>(
                    std::apply(F, 
                    std::tuple_cat(
                    std::forward_as_tuple(*self),
                    args_tp.value())
                ));
                return std::string();
            }
          }
        }
        return std::string();
    };
  }

  template <typename...Args, typename F>
  bool send_request(handle_t handle, std::string_view cmd, F&&f, Args&&...args) {
    auto ctx = handle_storage_t::ins().handle_grab(handle);
    if (!ctx) {
        return false;
    }

    cpp_rpc_param param;
    param.funcid = struct_pack::MD5::MD5Hash32Constexpr(cmd.data(), cmd.length());
    struct_pack::serialize_to(param.buffer, std::forward<Args>(args)...);

    auto self_ctx = wctx.lock();
    skynet_message msg;
    msg.source = self_ctx->handle;
    msg.session = self_ctx->newsession();
    msg.t = PTYPE_RESERVED_CPP;
    msg.data = std::move(param);

    cpp_rpc_cbs[msg.session] = std::move(f);

    return skynet_app::context_push(handle, msg);
  }

  template <typename...Args, typename F>
  bool send_request(std::string_view name, std::string_view cmd, F&& f, Args&&...args) {
    auto handle = handle_storage_t::ins().handle_finename(name.data());
    return send_request(handle, cmd, std::forward<F>(f), std::forward<Args>(args)...);
  }

  int timeout(cpp_sk::context_t *ctx, int timeout, std::function<void()> cb) {
    auto id = ctx->newsession();
    if (auto ret =
            cpp_sk::timer::timer::ins().timeout(ctx->handle, timeout, id);
        id == ret) {
      timeout_nodes[id] = std::move(cb);
      return id;
    }
    return -1;
  }

  template <typename F>
  int listen(cpp_sk::context_t *ctx, std::string_view ip, int port, int backlog,
             F &&f) {
    auto id = cpp_sk::server::ins().socket_listen(ctx, "0.0.0.0", 8888, 1024);
    if (id < 0) {
      return id;
    }
    socket_nodes[id] = {.cb = std::move(f)};
    return id;
  }
  template <typename F>
  void start_accept(cpp_sk::context_t *ctx, int id, F &&f) {
    if (auto it = socket_nodes.find(id); it != socket_nodes.end()) {
      if (it->second.connectd) {
        it->second.cb = std::move(f);
        cpp_sk::server::ins().socket_start(ctx, id);
      }
    }
  }

  template <typename F>
  int connect(cpp_sk::context_t *ctx, std::string_view ip, int port, F &&f) {
    int id = cpp_sk::server::ins().socket_connect(ctx, ip.data(), port);
    if (id < 0) {
      return id;
    }
    socket_nodes[id] = {.cb = std::move(f)};
    return id;
  }

  template <typename F> void start_tcp(cpp_sk::context_t *ctx, int id, F &&f) {
    if (auto it = socket_nodes.find(id); it != socket_nodes.end()) {
      if (it->second.connectd) {
        it->second.on_data = std::move(f);
        cpp_sk::server::ins().socket_start(ctx, id);
      }
    }
  }

  int send_buffer(cpp_sk::context_t *ctx, int id, std::string buf) {
    if (auto it = socket_nodes.find(id); it != socket_nodes.end()) {
      return cpp_sk::server::ins().socket_send(ctx, id, std::move(buf));
    }
    return -1;
  }

  static void close_socket(cpp_sk::context_t *ctx, int id) {
    cpp_sk::server::ins().close_socket(ctx, id);
  }

  template <typename... Args>
  static void log(std::format_string<Args...> fmt, Args &&...args) {
    cpp_sk::skynet_app::error(nullptr, std::move(fmt),
                              std::forward<Args>(args)...);
  }

  virtual bool init(cpp_sk::context_ptr_t &ctx,
                    std::string_view param) override {
    ctx->cb = &module_mid_t::cb;
    ctx->ud = this;
    wctx = ctx;
    return true;
  };

  virtual void on_text(cpp_sk::context_t *context, int session, uint32_t source, std::string& str) {
    
  }

  virtual void on_response(cpp_sk::context_t *context, int session, uint32_t source, std::any& a) {
    if (auto it = cpp_rpc_cbs.find(session); it != cpp_rpc_cbs.end()) {
        auto cb = std::move(it->second);
        cpp_rpc_cbs.erase(it);
        auto str = std::any_cast<std::string>(&a);
        cb(*str);
    } else if (auto it = timeout_nodes.find(session); it != timeout_nodes.end()) {
        auto cb = std::move(it->second);
        timeout_nodes.erase(it);
        cb();
    }
  }

  virtual void on_socket(cpp_sk::context_t *context, int session, uint32_t source, cpp_sk::skynet_socket_message& m) {
    if (auto it = socket_nodes.find(m.id); it != socket_nodes.end()) {
        if (m.type == cpp_sk::skynet_socket_type::connect) {
          if (!it->second.connectd) {
            it->second.connectd = true;
            if (it->second.cb) {
              auto cb = std::move(it->second.cb);
              it->second.id = m.id;
              cb(m.id);
            }
          } else {
          }
        } else if (m.type == cpp_sk::skynet_socket_type::close) {
          socket_nodes.erase(it);
        } else if (m.type == cpp_sk::skynet_socket_type::error) {
          close_socket(context, m.id);
        } else if (m.type == cpp_sk::skynet_socket_type::accept) {
          int newid = m.ud;
          socket_nodes[newid] = {.id = m.ud, .connectd = true};
          if (it->second.cb) {
            it->second.cb(m.ud);
          }
        } else if (m.type == cpp_sk::skynet_socket_type::data) {
          it->second.rbuff.append(m.buffer.ptr(), m.buffer.size());
          if (it->second.on_data) {
            auto sz = it->second.on_data(it->second.id, it->second.rbuff);
            if (sz > 0) {
              it->second.rbuff = it->second.rbuff.substr(sz);
            }
          }
        }
      } else {
        close_socket(context, m.id);
      }
  }

  virtual void on_rpc_cpp(cpp_sk::context_t *context, int session, uint32_t source, cpp_rpc_param& param) {
    if(auto it = cpp_rpc_router.find(param.funcid); it != cpp_rpc_router.end()) {
      auto r = it->second(param.buffer);
      skynet_message msg;
      msg.source = context->handle;
      msg.data = std::move(r);
      msg.t = PTYPE_RESPONSE;
      msg.session = session;
      skynet_app::context_push(source, msg);
    }
  }

  static int cb(cpp_sk::context_t *context, int type, int session,
                uint32_t source, std::any &a) {
    auto *t = std::any_cast<module_mid_t *>(context->ud);
    if (type == cpp_sk::PTYPE_TEXT) {
      auto *s = std::any_cast<std::string>(&a);
      t->on_text(context, session, source, *s);
    } else if (type == cpp_sk::PTYPE_RESPONSE) {
       t->on_response(context, session, source, a);
    } else if (type == cpp_sk::PTYPE_SOCKET) {
      cpp_sk::skynet_socket_message *m =
          std::any_cast<cpp_sk::skynet_socket_message>(&a);
      t->on_socket(context, session, source, *m);
    } else if(type == cpp_sk::PTYPE_RESERVED_CPP) {
      auto param = std::any_cast<cpp_rpc_param >(&a);
      if(param) {
        t->on_rpc_cpp(context, session, source, *param);
      }
    }
    return 0;
  }
};


} // namespace cpp_sk