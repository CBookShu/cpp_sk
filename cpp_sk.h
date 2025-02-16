#pragma once
#include "iguana/json_reader.hpp"
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
  size_t sz = 0;

  ~skynet_message() { int a; }

  void reserved_data() {
    data = nullptr;
    sz = 0;
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
                           uint32_t source, std::any &a, size_t sz);

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
    smsg.sz = msg.size() + 1;
    smsg.sz |= (uint32_t)((size_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);
    context_push(logger, smsg);
  }

  static bool context_push(handle_t handle, skynet_message &msg);

  static int send(context_t *ctx, uint32_t source, uint32_t destination,
                  int type, int session, void *msg, size_t sz);
  // static int context_send(context_t *context, void * msg, size_t sz, uint32_t
  // source, int type, int session);

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
} // namespace cpp_sk