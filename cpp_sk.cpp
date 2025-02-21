#include "cpp_sk.h"
#include "asio/executor_work_guard.hpp"
#include "logger.h"
#include "sk_socket.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <thread>
#include <utility>

using namespace cpp_sk;

static constexpr int DEFAULT_QUEUE_SIZE = 64;
static constexpr int MQ_OVERLOAD = 1024;

message_queue::message_queue(handle_t handle_)
    : handle(handle_), head(0), tail(0), release(false), in_global(true),
      overload(0), overload_threshold(MQ_OVERLOAD), queue(DEFAULT_QUEUE_SIZE),
      next(nullptr) {}

message_queue::~message_queue() { assert(next == nullptr); }

size_t message_queue::length() {
  int head_, tail_, cap_;
  {
    std::lock_guard guard(lock);
    head_ = head;
    tail_ = tail;
    cap_ = queue.size();
  }
  if (head_ <= tail_) {
    return tail_ - head_;
  }
  return tail_ + cap_ - head_;
}

int message_queue::mq_overload() { return std::exchange(overload, 0); }

void message_queue::mq_push(skynet_message &message) {
  std::lock_guard guard(lock);
  queue[tail] = std::move(message);
  if (++tail >= queue.size()) {
    tail = 0;
  }

  if (head == tail) {
    int cap = queue.size();
    queue.resize(queue.size() * 2);
    head = 0;
    tail = cap;
  }

  if (!in_global) {
    in_global = true;
    global_queue::ins().push(this);
  }
}

std::optional<skynet_message> message_queue::mq_pop() {
  std::optional<skynet_message> res;
  bool ret = true;
  std::lock_guard guard(lock);
  if (head != tail) {
    ret = false;
    res = std::move(queue[head++]);
    int head_ = head;
    int tail_ = tail;
    int cap_ = queue.size();

    if (head_ >= cap_) {
      head = head_ = 0;
    }
    int length = tail - head;
    if (length < 0) {
      length += cap_;
    }
    while (length > overload_threshold) {
      overload = length;
      overload_threshold *= 2;
    }
  } else {
    overload_threshold = MQ_OVERLOAD;
  }

  if (ret) {
    in_global = false;
  }

  return res;
}

void message_queue::mark_release() {
  std::lock_guard guard(lock);
  assert(!release);
  release = true;
  if (!in_global) {
    global_queue::ins().push(this);
  }
}

global_queue::global_queue() : head(nullptr), tail(nullptr) {}

void global_queue::push(message_queue *queue) {
  std::lock_guard guard(lock);
  assert(queue->next == nullptr);
  if (tail) {
    tail->next = queue;
    tail = queue;
  } else {
    head = tail = queue;
  }
}

message_queue *global_queue::pop() {
  std::lock_guard guard(lock);
  auto mq = head;
  if (mq) {
    head = mq->next;
    if (head == nullptr) {
      assert(mq == tail);
      tail = nullptr;
    }
    mq->next = nullptr;
  }
  return mq;
}

std::unique_ptr<handle_storage_t> handle_storage_t::gH;
handle_storage_t::handle_storage_t(int harbor_) : slot(4) {
  harbor = (uint32_t)(harbor_ & 0xff) << handle_t::HANDLE_REMOTE_SHIFT;
}

handle_t handle_storage_t::handle_register(context_ptr_t &ctx) {
  std::unique_lock guard(lock);
  for (;;) {
    handle_t handle = handle_index;
    for (int i = 0; i < slot.size(); i++, handle++) {
      handle.check_range();
      int hash = handle & (slot.size() - 1);
      if (!slot[hash]) {
        slot[hash] = ctx;
        handle_index = handle + 1;
        guard.unlock();

        handle |= harbor;
        return handle;
      }
    }
    assert((slot.size() * 2 - 1) <= handle_t::HANDLE_MASK);
    slot.resize(slot.size() * 2);
  }
}

bool handle_storage_t::handle_retire(handle_t handle) {
  context_ptr_t ctx;
  std::unique_lock guard(lock);
  uint32_t hash = handle & (slot.size() - 1);
  auto &_ctx = slot[hash];
  if (_ctx && _ctx->handle == handle) {
    ctx = std::move(_ctx);

    for (auto it = name.begin(); it != name.end(); ++it) {
      if (it->second == handle) {
        name.erase(it);
        break;
      }
    }
  }
  guard.unlock();
  if (ctx) {
    ctx.reset();
    return true;
  }
  return false;
}

void handle_storage_t::handle_retireall() {
  for (;;) {
    int n = 0;
    for (int i = 0; i < slot.size(); ++i) {
      std::unique_lock guard(lock);
      handle_t handle = 0;
      context_ptr_t ctx = std::move(slot[i]);
      if (ctx) {
        handle = ctx->handle;
        n++;
      }
      guard.unlock();
      if (handle != 0) {
        ctx.reset();
      }
    }
    if (n == 0) {
      return;
    }
  }
}

context_ptr_t handle_storage_t::handle_grab(handle_t handle) {
  std::shared_lock guard(lock);
  uint32_t hash = handle & (slot.size() - 1);
  auto &ctx = slot[hash];
  if (ctx && ctx->handle == handle) {
    return ctx;
  }
  return nullptr;
}

handle_t handle_storage_t::handle_finename(const char *name_) {
  std::shared_lock guard(lock);
  if (auto it = name.find(name_); it != name.end()) {
    return it->second;
  }
  return 0;
}

bool handle_storage_t::handle_namehandle(handle_t handle, const char *name_) {
  std::lock_guard guard(lock);
  auto it = name.try_emplace(name_, handle);
  return it.second;
}

context_ptr_t context_t::create(std::string_view name, std::string_view param) {
  auto ctx = std::make_shared<context_t>();
  ctx->instance.reset(module_t::create(name));
  ctx->cb = nullptr;
  ctx->session_id = 0;
  ctx->init = false;
  ctx->endless = false;

  ctx->cpu_cost = 0;
  ctx->cpu_start = 0;
  ctx->message_count = 0;
  ctx->profile = node_t::ins().profile;

  ctx->handle = 0;
  ctx->handle = handle_storage_t::ins().handle_register(ctx);
  ctx->queue = std::make_unique<message_queue>(ctx->handle);

  if (ctx->instance->init(ctx, param)) {
    ctx->init = true;
    global_queue::ins().push(ctx->queue.get());
    if (ctx.use_count() != 1) {
      skynet_app::error(ctx.get(), "LAUNCH {} {}", name, param);
    }
  } else {
    skynet_app::error(ctx.get(), "FAILED LAUNCH {}", name);
    handle_storage_t::ins().handle_retire(ctx->handle);
    ctx->queue->mq_release([source = ctx->handle](skynet_message *m) {
      assert(source != 0);
      skynet_message msg;
      msg.source = source;
      msg.session = m->session;
      msg.t = PTYPE_ERROR;
      skynet_app::context_push(source, msg);
    });
  }

  return ctx;
}

context_t::context_t() { node_t::ins().total++; }

context_t::~context_t() {
  instance.reset();
  node_t::ins().total--;
}

void context_t::dispatch(skynet_message &msg) {
  int type = msg.t;
  message_count++;

  int reserve_msg = cb(this, type, msg.session, msg.source, msg.data);
  if (reserve_msg) {
    msg.reserved_data();
  }
}

std::unordered_map<std::string, module_t::creator_func_t, string_hash,
                   std::equal_to<>>
    module_t::gMap;

module_base_t *module_t::create(std::string_view name) {
  if (auto it = gMap.find(name); it != gMap.end()) {
    return it->second();
  }
  return nullptr;
}

void skynet_monitor::check() {
  if (version == check_version) {
    if (destination) {
      skynet_app::context_endless(destination);
      skynet_app::error(nullptr,
                        "A message from {:08x} to {:08x} maybe "
                        "in an endless loop (version = {})",
                        source, destination, version.load());
    }
  } else {
    check_version = version;
  }
}

void skynet_monitor::trigger(uint32_t source_, uint32_t destination_) {
  source = source_;
  destination = destination_;
  version++;
}

bool skynet_app::context_push(handle_t handle, skynet_message &msg) {
  auto ctx = handle_storage_t::ins().handle_grab(handle);
  if (!ctx) {
    return false;
  }
  ctx->queue->mq_push(msg);
  return true;
}

static void _filter_args(context_t *context, int type, int *session,
                         void **data, size_t *sz) {
  int needcopy = !(type & PTYPE_TAG_DONTCOPY);
  int allocsession = type & PTYPE_TAG_ALLOCSESSION;
  type &= 0xff;

  if (allocsession) {
    assert(*session == 0);
    *session = context->newsession();
  }
  if (needcopy && *data) {
    char *msg = (char *)std::malloc(*sz + 1);
    memcpy(msg, *data, *sz);
    msg[*sz] = '\0';
    *data = msg;
  }
  *sz |= (size_t)type << MESSAGE_TYPE_SHIFT;
}

void skynet_app::context_endless(handle_t handle) {
  auto ctx = handle_storage_t::ins().handle_grab(handle);
  if (!ctx) {
    return;
  }
  ctx->endless = true;
}

void skynet_app::start_monitor(std::thread &t, monitor &m) {
  for (auto &_m : m.m) {
    _m = std::make_unique<skynet_monitor>();
  }
  t = std::thread([&m]() {
    for (;;) {
      CHECK_ABORT
      for (auto &_m : m.m) {
        _m->check();
      }
      for (int i = 0; i < 5; i++) {
        CHECK_ABORT
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  });
}

void skynet_app::start_timer(std::thread &t, monitor &m) {
  t = std::thread([&m]() {
    int count = 0;
    for (;;) {
      timer::ins().update();
      CHECK_ABORT
      m.wakeup(m.count - 1);
      std::this_thread::sleep_for(std::chrono::microseconds(2500));
    }

    {
      std::lock_guard guard(m.mutex);
      m.quit = true;
      m.cond.notify_all();
    }
  });
}

void skynet_app::start_socket(std::thread &t, monitor &m) {
  t = std::thread([&m]() {
    auto guard = asio::make_work_guard(server::ins().poll);
    server::ins().poll.run();
  });
}

message_queue *skynet_app::context_message_dispatch(skynet_monitor *m,
                                                    message_queue *q,
                                                    int weight) {
  if (q == nullptr) {
    q = global_queue::ins().pop();
    if (q == nullptr) {
      return nullptr;
    }
  }
  handle_t handle = q->handle;
  auto ctx = handle_storage_t::ins().handle_grab(handle);
  if (!ctx) {
    q->mq_release([source = handle](skynet_message *m) {
      assert(source);
      skynet_message msg;
      msg.source = source;
      msg.session = m->session;
      msg.t = PTYPE_ERROR;
      skynet_app::context_push(source, msg);
    });
  }

  int i, n = 1;
  for (i = 0; i < n; i++) {
    auto msg = q->mq_pop();
    if (!msg) {
      return global_queue::ins().pop();
    } else if (i == 0 && weight >= 0) {
      n = q->length();
      n >>= weight;
    }

    int overload = q->mq_overload();
    if (overload) {
      skynet_app::error(ctx.get(), "May overlad, message queue length = {}",
                        overload);
    }

    m->trigger(msg.value().source, handle);

    if (ctx->cb) {
      ctx->dispatch(msg.value());
    }

    m->trigger(0, 0);
  }

  assert(q == ctx->queue.get());
  auto *nq = global_queue::ins().pop();
  if (nq) {
    global_queue::ins().push(q);
    q = nq;
  }
  return q;
}

void skynet_app::start_worker(std::thread &t, monitor &m, int id, int weight) {
  t = std::thread([id, weight, &m] {
    message_queue *q = nullptr;
    while (!m.quit) {
      q = context_message_dispatch(m.m[id].get(), q, weight);
      if (q == nullptr) {
        std::unique_lock guard(m.mutex);
        ++m.sleep;
        if (!m.quit) {
          m.cond.wait(guard);
        }
        --m.sleep;
      }
    }
  });
}

timer::timer() {
  using namespace std::chrono;
  time_ = 0;

  current_point_ = clock_t::now();
  constexpr time_point zero{};
  auto diff = current_point_ - zero;
  starttime_ = duration_cast<seconds>(diff).count();
  current_ = duration_cast<centisecond>(current_point_ -
                                        clock_t::from_time_t(starttime_))
                 .count();
}

void timer::update() {
  auto now = clock_t::now();
  auto diff = std::chrono::duration_cast<centisecond>(now - current_point_);
  if (now < current_point_) {
    auto zone = std::chrono::current_zone();
    skynet_app::error(nullptr, "time diff error: change from {} to {}",
                      std::chrono::zoned_time{zone, now},
                      std::chrono::zoned_time{zone, current_point_});
    current_point_ = now;
  } else if (diff.count() > 0) {
    current_point_ = now;
    current_ += diff.count();
    for (int i = 0; i < diff.count(); ++i) {
      timer_update();
    }
  }
}

void timer::timer_update() {
  lock_.lock();
  timer_execute();
  timer_shift();
  timer_execute();
  lock_.unlock();
}

void timer::timer_execute() {
  int idx = time_ & TIME_NEAR_MASK;
  while (near_[idx].head.next) {
    auto current = near_[idx].clear();
    lock_.unlock();
    dispatch(std::move(current));
    lock_.lock();
  }
}
void timer::timer_shift() {
  int mask = TIME_NEAR;
  auto ct = ++time_;
  if (ct == 0) {
    move_list(3, 0);
  } else {
    uint32_t time_ = ct >> TIME_NEAR_SHIFT;
    int i = 0;
    while ((ct & (mask - 1)) == 0) {
      int idx = time_ & TIME_NEAR_MASK;
      if (idx != 0) {
        move_list(i, idx);
        break;
      }
      mask <<= TIME_NEAR_SHIFT;
      time_ >>= TIME_NEAR_SHIFT;
      ++i;
    }
  }
}

int timer::timeout(handle_t handle, int32_t timeout, int session) {
  if (timeout <= 0) {
    skynet_message message;
    message.source = 0;
    message.session = session;
    message.t = PTYPE_RESPONSE;
    if (!skynet_app::context_push(handle, message)) {
      return -1;
    }
  } else {
    auto node = std::make_unique<timer_node_event>();
    node->handle = handle;
    node->session = session;
    timer_add(std::move(node), timeout);
  }
  return session;
}

void timer::timer_add(std::unique_ptr<timer_node_event> node, int32_t timeout) {
  std::lock_guard guard(lock_);
  node->expire = time_ + timeout;
  add_node(std::move(node));
}

void timer::add_node(std::unique_ptr<timer_node_event> node) {
  uint32_t time = node->expire;
  uint32_t current_time = time_;
  if ((time | TIME_NEAR_MASK) == (current_time | TIME_NEAR_MASK)) {
    near_[time & TIME_NEAR_MASK].link(std::move(node));
  } else {
    int i;
    uint32_t mask = TIME_NEAR << TIME_LEVEL_SHIFT;
    for (i = 0; i < 3; ++i) {
      if ((time | (mask - 1)) == (current_time | (mask - 1))) {
        break;
      }
      mask <<= TIME_LEVEL_SHIFT;
    }
    /*
      当 time 溢出了，只要不溢出太严重到，又跟 time_ 相差接近 TIME_NEAR
      那么最终 (time|(mask-1)) 就不会等于 (current_time|(mask-1))
      那么最终会走到 level:3 层级，并且 (time>>(TIME_NEAR_SHIFT +
      i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK) = 0 即：所有溢出都统统存入
      t_[3][0], 在shift 的时候，当 time_ = 0，即也溢出时，会重新将这些node
      投放一遍
    */
    t_[i]
      [((time >> (TIME_NEAR_SHIFT + i * TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)]
          .link(std::move(node));
  }
}

void timer::move_list(int level, int idx) {
  auto current = t_[level][idx].clear();
  while (current) {
    auto temp = std::move(current->next);
    add_node(std::move(current));
    current = std::move(temp);
  }
}

void timer::dispatch(std::unique_ptr<timer_node_event> current) {
  do {
    skynet_message message;
    message.source = 0;
    message.session = current->session;
    message.t = PTYPE_RESPONSE;

    skynet_app::context_push(current->handle, message);
    current = std::move(current->next);
  } while (current);
}
