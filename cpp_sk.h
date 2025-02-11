#pragma once
#include "iguana/json_reader.hpp"
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <format>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <filesystem>


namespace cpp_sk {
template <typename T>
class mutex_type {
public:
    template <typename...Args>
    mutex_type(Args&&...args):
        data_(std::forward<Args>(args)...) 
    {}

    class Guard {
        std::lock_guard<T> guard_;
        mutex_type* p_;
    public:
        Guard(mutex_type* m):guard_(m->mux_),p_(m) {}

        T* operator -> () {
            return &(p_->data_);
        }

        T* get() {
            return &(p_->data_);
        }
    };

    [[nodiscard]]
    Guard write() {
        return Guard(this);
    }

private:
    std::mutex mux_;
    T data_;
};

template <typename T>
class shared_mutex_type {
public:
    template <typename...Args>
    shared_mutex_type(Args&&...args):
        data_(std::forward<Args>(args)...) 
    {}

    class WriteGuard {
        std::lock_guard<std::shared_mutex> guard_;
        shared_mutex_type* p_;
    public:
        WriteGuard(shared_mutex_type* m):guard_(m->smux_),p_(m) {}

        T* operator -> () {
            return &(p_->data_);
        }

        T* get() {
            return &(p_->data_);
        }
    };

    class ReadGuard {
        std::shared_lock<std::shared_mutex> guard_;
        shared_mutex_type* p_;
    public:
        ReadGuard(shared_mutex_type* m):guard_(m->smux_),p_(m) {}

        const T* operator -> () {
            return &(p_->data_);
        }

        const T* get() {
            return &(p_->data_);
        }
    };

    WriteGuard write() {
        return WriteGuard(this);
    }

    ReadGuard read() {
        return ReadGuard(this);
    }

private:
    std::shared_mutex smux_;
    T data_;
};

struct string_hash {
  using is_transparent = void;
  [[nodiscard]] size_t operator()(const char *txt) const {
    return std::hash<std::string_view>{}(txt);
  }
  [[nodiscard]] size_t operator()(std::string_view txt) const {
    return std::hash<std::string_view>{}(txt);
  }
  [[nodiscard]] size_t operator()(const std::string &txt) const {
    return std::hash<std::string>{}(txt);
  }
};

class spinlock_mutex
{
  std::atomic_flag flag;
public:
  spinlock_mutex():
  flag{false}
  {}
  void lock()
  {
    while(flag.test_and_set(std::memory_order_acquire));
  }
  void unlock()
  {
    flag.clear(std::memory_order_release);
  }
};

struct config_t {
  int thread = std::thread::hardware_concurrency();
  int harbor = 1;
  bool profile = 1;
  std::string daemon;
  std::string module_path;
  std::string bootstrap;
  std::string logger;
  std::string logservice;

  void read(const char* config_path) {
    try {
      auto path = std::filesystem::current_path().append("config.json");
      iguana::from_json_file(*this, path);
    } catch(std::exception& e) {
      std::cerr << e.what() << std::endl;
    }
  }
};

struct node_t {
  std::atomic_int total{0};
  bool init{false};
  uint32_t monitor_exit;
  bool profile = true;

  static node_t& ins() {
    static node_t N;
    return N;
  }
};

struct handle_t {
  static constexpr uint32_t HANDLE_MASK = 0xffffff;
  static constexpr uint32_t HANDLE_REMOTE_SHIFT = 24;

  uint32_t handle;
  handle_t(uint32_t h = 0):handle(h) {}
  handle_t(uint32_t h, uint32_t habor) {handle = h | habor;}

  void check_range() {
    if (handle > HANDLE_MASK) {
      handle = 1;
    }
  }

  operator uint32_t() {
    return handle;
  }

  operator uint32_t() const {
    return handle;
  }

  handle_t operator |= (uint32_t harbor) {
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
constexpr size_t MESSAGE_TYPE_SHIFT = ((sizeof(size_t)-1) * 8);

constexpr size_t TAG_DONTCOPY = 0x10000;
constexpr size_t TAG_ALLOCSESSION = 0x20000;
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

struct skynet_data_t {
  std::span<std::byte> buf;

  skynet_data_t() = default;
  skynet_data_t(skynet_data_t& other) = delete;
  skynet_data_t(skynet_data_t&& other) {
    buf = std::exchange(other.buf, {});
  }
  skynet_data_t& operator =( skynet_data_t& other) = delete;
  skynet_data_t& operator =(skynet_data_t&& other) {
    buf = std::exchange(other.buf, {});
    return *this;
  }

};

struct skynet_message {
  uint32_t source = 0;
	int session = 0;
  char* data = nullptr;
  // [sizeof(size_t)-8, sizeof(size_t)) type
  // [0, sizeof(size_t)-8) data len
  size_t size = 0;

  skynet_message() = default;
  ~skynet_message() {
    if (data) {
      std::free(data);
    }
  }

  skynet_message(skynet_message&& other) {
    data = std::exchange(other.data, nullptr);
    size = std::exchange(other.size, 0);
    source = std::exchange(other.source, 0);
    session = std::exchange(other.session, 0);
  }

  skynet_message& operator=(skynet_message&& other) {
    data = std::exchange(other.data, data);
    size = std::exchange(other.size, size);
    source = std::exchange(other.source, source);
    session = std::exchange(other.session, session);
    return *this;
  }

  skynet_message(skynet_message& other) = delete;
  skynet_message& operator = (skynet_message& other) = delete;
};

struct message_queue;
struct global_queue {
private:
	struct message_queue *head;
	struct message_queue *tail;
	struct spinlock_mutex lock;
public:
  global_queue();

  static global_queue& ins() {
    static global_queue Q;
    return Q;
  }

  void push(message_queue* queue);

  message_queue* pop();
};


struct message_queue {
private:
  friend class global_queue;
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
  template<typename F>
  requires requires(F&& f, skynet_message* m){f(m);}
  void _drop_queue(F&& f) {
    while(auto msg = mq_pop()) {
      std::forward<F>(f)(std::addressof(msg.value()));
    }
  }
public:
  message_queue(handle_t handle_);
  ~message_queue();

  size_t length();
  int mq_overload();
  void mq_push(skynet_message& message);
  std::optional<skynet_message> mq_pop();

  void mark_release();

  template<typename F>
  requires requires(F f, skynet_message* m){f(m);}
  void mq_release(F&& f) {
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

template<typename _Type>
struct module_register_t{
    inline static auto place = []{ // [1]
        std::cout << ylt::reflection::type_string<_Type>() << std::endl;
        return 0;
    }();
};

template<typename _Type>
using module_register_base = decltype([](auto...){return module_register_t<_Type>::place;}(), module_register_t<_Type>{});

struct module_t {
public:
  
};

struct module_base_t {
  virtual ~module_base_t() {};
  virtual std::string_view name() const = 0;
  virtual bool init(context_ptr_t& , const char*) = 0;
  virtual void signal(int) = 0;

  virtual std::string_view type_name() {
    return typeid(*this).name();
  }
};

struct context_t {
  std::unique_ptr<module_base_t> instance;
  std::unique_ptr<message_queue> queue;

  u_int64_t cpu_cost;
  u_int64_t cpu_start;
  std::string result;
  handle_t handle;
  int session_id;
  size_t message_count;
  bool init = false;
  bool endless = false;
  bool profile = true;

  context_t();
  ~context_t();

  static context_ptr_t 
    create(std::unique_ptr<module_base_t> instance_, const char* param);

  template <typename C>
  decltype(auto) call(C&& cb) {
    return std::forward<C>(cb)(this);
  }
};

struct handle_storage_t {
public:
  handle_storage_t(int harbor_);

  handle_t handle_register(context_ptr_t& ctx);

  bool handle_retire(handle_t handle);

  void handle_retireall();

  context_ptr_t handle_grab(handle_t handle);

  handle_t handle_finename(const char* name_);

  bool handle_namehandle(handle_t handle, const char* name_);

  static void init(int harbor_) {
    static std::atomic_bool flag{false};
    bool ok = false;
    if (!flag.compare_exchange_strong(ok, true)) {
      return;
    }
    gH = std::make_unique<handle_storage_t>(harbor_);
  }
  static handle_storage_t& ins() {
    return *gH;
  }
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
  static harbor_t& ins() {
    static harbor_t H;
    return H;
  }

  void init(int harbor_) {
    harbor = (unsigned int)harbor_ << handle_t::HANDLE_REMOTE_SHIFT;
  }

  void start(context_ptr_t& ctx) {
    remote = ctx;
  }

  void exit() {
    remote.reset();
  }
protected:
  constexpr bool invalid_type(PTYPE type) {
    return type != PTYPE_SYSTEM && type != PTYPE_HARBOR;  
  }
private:
  int harbor = ~0;
  context_ptr_t remote;
};

struct skynet_server {
  template<typename...Args>
  static void error(context_t* context, std::string_view msg) {
    static handle_t logger = handle_storage_t::ins().handle_finename("logger");
    if (logger == 0) {
      logger = handle_storage_t::ins().handle_finename("logger");
    }
    if (logger == 0) {
      return;
    }
    skynet_message smsg;
    if (context == NULL) {
      smsg.source = 0;
    } else {
      smsg.source = context->handle;
    }
    smsg.session = 0;
    smsg.data = (char*)std::malloc(msg.size() + 1);
    smsg.data[msg.size()] = 0;
    std::copy(msg.begin(), msg.end(), smsg.data);
    smsg.size = msg.size() | ((uint32_t)PTYPE_TEXT << MESSAGE_TYPE_SHIFT);
    context_push(logger, smsg);
  }

  static bool context_push(handle_t handle, skynet_message& msg) {
    auto ctx = handle_storage_t::ins().handle_grab(handle);
    if (!ctx) {
      return false;
    }
    ctx->queue->mq_push(msg);
    return true;
  }

  static int send(context_t* context, uint32_t source, uint32_t destination, PTYPE type, int session, std::unique_ptr<std::string> msg) {
    return 0;
  }

  static int context_total() {
    return node_t::ins().total;
  }

  static void context_endless(handle_t handle) {
    auto ctx = handle_storage_t::ins().handle_grab(handle);
    if (!ctx) {
      return;
    }
    ctx->endless = true;
  }

  static void start(config_t& config) {
    harbor_t::ins().init(config.harbor);
    handle_storage_t::ins().init(config.harbor);
    global_queue::ins();
    
  }
};

} // namespace cpp_sk