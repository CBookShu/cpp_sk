#include "cpp_sk.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <sys/types.h>
#include <utility>

using namespace cpp_sk;

static constexpr int DEFAULT_QUEUE_SIZE = 64;
static constexpr int MQ_OVERLOAD = 1024;

message_queue::message_queue(handle_t handle_)
    : handle(handle_), head(0), tail(0), release(false), in_global(false),
      overload(0), overload_threshold(MQ_OVERLOAD), queue(DEFAULT_QUEUE_SIZE),
      next(nullptr) {}

message_queue::~message_queue() { assert(next == nullptr); }

size_t message_queue::length() {
  int head, tail, cap;
  {
    std::lock_guard guard(lock);
    head = head;
    tail = tail;
    cap = queue.size();
  }
  if (head <= tail) {
    return tail - head;
  }
  return tail + cap - head;
}

int message_queue::mq_overload() { return std::exchange(overload, 0); }

void message_queue::mq_push(skynet_message& message) {
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
  std::optional<skynet_message> ret;
  std::lock_guard guard(lock);
  if (head != tail) {
    ret = std::move(queue[head++]);
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
    if (length > overload_threshold) {
      overload = length;
      overload_threshold *= 2;
    }
  } else {
    overload_threshold = MQ_OVERLOAD;
  }

  if (ret) {
    in_global = false;
  }

  return ret;
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
    if (head->next == nullptr) {
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

context_ptr_t context_t::create(std::unique_ptr<module_base_t> instance_, const char* param) {
    auto ctx = std::make_shared<context_t>();
    ctx->instance = std::move(instance_);
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
            skynet_server::error(ctx.get(), std::format("LAUNCH {} {}", ctx->instance->type_name(), param ? param : ""));
        }
    } else {
        skynet_server::error(ctx.get(), std::format("FAILED LAUNCH {}", ctx->instance->type_name()));
        handle_storage_t::ins().handle_retire(ctx->handle);
        ctx->queue->mq_release([source = ctx->handle](skynet_message* m){
            assert(source!=0);

        });
    }

    return ctx;
}

context_t::context_t() {
    node_t::ins().total++;
}

context_t::~context_t() {
  instance.reset();
  node_t::ins().total--;
}