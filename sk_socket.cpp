#include "sk_socket.h"
#include "asio/awaitable.hpp"
#include "asio/buffer.hpp"
#include "asio/co_spawn.hpp"
#include "asio/connect.hpp"
#include "asio/detached.hpp"
#include "asio/error.hpp"
#include "asio/error_code.hpp"
#include "asio/impl/connect.hpp"
#include "asio/ip/address.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/socket_base.hpp"
#include "asio/system_error.hpp"
#include "asio/use_awaitable.hpp"
#include "cpp_sk.h"
#include "utils.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <variant>

using namespace cpp_sk;

int server::new_fd() {
  auto t = socket_type::invalid;
  auto size = slots.size();
  for(int i = 0; i < size; i++) {
    auto id = alloc++;
    auto idx = id % slots.size();
    if (slots[idx].type.compare_exchange_strong(t, socket_type::reserve)) {
      return id;
    }
  }
  assert(false);
  return -1;
}

auto& server::get_slot(int id) {
  return slots[id % slots.size()];
}

int server::socket_listen(context_t* ctx, const char* host, int port, int backlog) {
  try {
    namespace ip = asio::ip;

    auto addr = ip::make_address(host);
    auto ep = ip::tcp::endpoint{addr, static_cast<uint16_t>(port)};
    socket_t::tcp_ns::acceptor acceptor{poll};
    acceptor.open(ep.protocol());
    acceptor.bind(ep);
    acceptor.listen(backlog);
    acceptor.set_option(asio::socket_base::reuse_address{true});
    auto id = new_fd();

    auto& slot = get_slot(id);
    slot.sock = std::move(acceptor);
    slot.id = id;
    slot.opaque = ctx->handle;

    poll.post([this, id](){
      auto& slot = get_slot(id);

      socket_message result;
      result.id = id;
      result.opaque = slot.opaque;

      forward_message(skynet_socket_type::connect, true, result);
    });
    return id;
  } catch(std::exception& e) {
    skynet_app::error(nullptr, "listen error {}", e.what());
    return -1;
  }
}

int server::socket_connect(context_t* ctx, const char* host, int port) {
  try {
    namespace ip = asio::ip;
    using tcp = ip::tcp;
    auto id = new_fd();
    auto& slot = get_slot(id);
    slot.id = id;
    slot.opaque = ctx->handle;
    slot.sock = tcp::socket(poll);

    tcp::resolver::query query(host, std::to_string(port));
    asio::co_spawn(poll, [this, &slot, id = id, query=std::move(query)]()->asio::awaitable<void>{
      auto& slot = get_slot(id);
      if (slot.type != socket_type::reserve || slot.id != id) {
        co_return;
      }
      socket_message result;
      result.id = id;
      result.opaque = slot.opaque;
      try {
        
        slot.type.store(socket_type::connecting);

        tcp::resolver resolver(poll);
        auto it = co_await resolver.async_resolve(query, asio::use_awaitable);

        auto ep = co_await asio::async_connect(*slot.tcp(), it, asio::use_awaitable);

        slot.type.store(socket_type::connected);
        forward_message(skynet_socket_type::connect, true, result);
        
      } catch(std::exception& e) {
        result.data = e.what();
        forward_message(skynet_socket_type::error, true, result);
      }
    },asio::detached);
    return id;
  } catch(std::exception& e) {
    skynet_app::error(nullptr, "connect error {}", e.what());
    return -1;
  }
}

void server::socket_start(context_t* ctx, int id) {
  poll.post([this, handle = ctx->handle, id = id](){
    socket_message result;
    result.id = id;
    result.opaque = handle;

    auto& slot = get_slot(id);
    if (slot.id != id || slot.type == socket_type::invalid) {
      result.data = "invalid socket";
      forward_message(skynet_socket_type::error, true, result);
      return;
    }

    // read close
    if (slot.type == socket_type::halfclose_read) {
      result.data = "socket closed";
      forward_message(skynet_socket_type::error, true, result);
      return;
    }

    if (auto* l = slot.acceptor(); l) {
      asio::co_spawn(poll, [this, id, result]() mutable ->asio::awaitable<void>{
        try {
          auto& slot = get_slot(id);
          for(;;) {
            if (slot.id != id || slot.type == socket_type::invalid) {
              result.data = "invalid socket";
              forward_message(skynet_socket_type::error, true, result);
              co_return;
            }

            auto sock = co_await slot.acceptor()->async_accept(asio::use_awaitable);
            sock.set_option(asio::socket_base::keep_alive(true));
            sock.non_blocking(true);

            int newid = new_fd();
            auto& new_slot = get_slot(newid);
            new_slot.id = newid;
            new_slot.opaque = slot.opaque;
            new_slot.type.store(socket_type::paccept);
            result.ud = newid;
            forward_message(skynet_socket_type::accept, true, result);
          }
        } catch(std::exception& e) {
          // TODO report
        }
      },asio::detached);
      forward_message(skynet_socket_type::connect, true, result);
    } else if(auto* t = slot.tcp(); t) {

    }
  });
}

void server::forward_message(skynet_socket_type type, bool padding, socket_message& result) {
  skynet_message smsg;
  auto* m = smsg.alloc<skynet_socket_message>();
  m->type = type;
  m->id = result.id;
  m->ud = result.ud;
  m->buffer = nullptr;

  if (padding && result.data) {
    m->buffer = algo::str_malloc(result.data);
  } else {
    m->buffer = result.data;
  }
  
  smsg.source = 0;
  smsg.session = 0;
  smsg.sz |= ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);

  if(!skynet_app::context_push(result.opaque, smsg)) {
    algo::safe_free(m->buffer);
  }
}