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
    if(id == 0) {
        continue;
    }
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
    slot.type.store(socket_type::plisten);

    poll.post([this, id](){
      auto& slot = get_slot(id);

      socket_message result;
      result.id = id;
      result.opaque = slot.opaque;

      forward_message(skynet_socket_type::connect, result);
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

        result.data.str = ep.address().to_string();
        forward_message(skynet_socket_type::connect, result);
        
      } catch(std::exception& e) {
        result.data.str = e.what();
        forward_message(skynet_socket_type::error, result);
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
      result.data.str_view = "invalid socket";
      forward_message(skynet_socket_type::error, result);
      return;
    }

    // read close
    if (slot.type == socket_type::halfclose_read) {
      result.data.str_view = "socket closed";
      forward_message(skynet_socket_type::error, result);
      return;
    }

    if (auto* l = slot.acceptor(); l) {
      asio::co_spawn(poll, [this, id, handle = handle]() mutable ->asio::awaitable<void>{
        socket_message result;
        result.id = id;
        result.opaque = handle;
        try {
          auto& slot = get_slot(id);
          for(;;) {
            if (slot.id != id || slot.type != socket_type::plisten) {
              result.data.str_view = "invalid socket";
              forward_message(skynet_socket_type::error, result);
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
            new_slot.sock = std::move(sock);
            result.ud = newid;
            forward_message(skynet_socket_type::accept, result);
          }
        } catch(std::exception& e) {
          // TODO report
        }
      },asio::detached);
      forward_message(skynet_socket_type::connect, result);
    } else if(auto* t = slot.tcp(); t) {
        asio::co_spawn(poll, [this, id, handle = handle]() mutable ->asio::awaitable<void> {
            socket_message result;
            result.id = id;
            result.opaque = handle;
            try {
                auto& slot = get_slot(id);
                if (slot.id != id || (slot.type != socket_type::paccept && slot.type != socket_type::connected)) 
                {
                    result.data.str_view = "invalid socket";
                    forward_message(skynet_socket_type::error, result);
                    co_return;
                }

                char buff[65535];
                for (;;) {
                    auto sz = co_await slot.tcp()->async_read_some(asio::buffer(buff), asio::use_awaitable);
                    result.data.str = std::string(buff, sz);
                    forward_message(skynet_socket_type::data, result);
                }
            }
            catch (std::exception& e) {
                // TODO report
            }
        }, asio::detached);
        forward_message(skynet_socket_type::connect, result);
    } else if(auto* u = slot.udp(); u) {
        
    } else {
        // unreach
        assert(false);
    }
  });
}

int cpp_sk::server::socket_send(context_t* ctx, int id, std::string buf)
{
    auto& slot = get_slot(id);
    auto type = slot.type.load();
    if (slot.id != id 
        || type == socket_type::invalid
        || type == socket_type::halfclose_write
        || type == socket_type::paccept) {
        return -1;
    }

    size_t offset = 0;
    do {
        if(slot.wlist_lock.trylock()) {
            defer_t _defer([&slot](){
                slot.wlist_lock.unlock();
            });
            if (!slot.wlist.empty()) {
                break;
            }
            if (auto* t = slot.tcp(); t) {
                asio::error_code ec;
                auto sz = t->write_some(asio::buffer(buf), ec);
                if (ec) {
                    break;
                }
                if (sz == buf.size()) {
                    // 写完了
                    return sz;
                }

                // 写了一部分
                offset = sz;
            } else if(auto* u = slot.udp(); u) {
                    
            }
            slot.wlist_lock.unlock();
        }
    } while(0);

    asio::co_spawn(poll, 
        [this, id = id, handle = ctx->handle, buf = std::move(buf), offset]() -> asio::awaitable<void> {
        auto& slot = get_slot(id);
        auto type = slot.type.load();
        if (slot.id != id 
            || type == socket_type::invalid
            || type == socket_type::halfclose_write
            || type == socket_type::paccept) {
            co_return ;
        }
        
        if (type == socket_type::plisten
            || type == socket_type::listen) {
            skynet_app::error(nullptr, "socket-server: write to listen fd {}.", id);
            co_return;
        }

        if (offset > 0) {
            slot.wlist.emplace_back(buf.substr(offset));
        } else {
            slot.wlist.emplace_back(std::move(buf));
        }

        if (slot.wlist_lock.trylock()) {
            defer_t _defer([&](){
                slot.wlist_lock.unlock();
            });

            if (auto* t = slot.tcp(); t) {
                // TODO: 直接 write buffers
                while(!slot.wlist.empty()) {
                    auto front = std::move(slot.wlist.front());
                    slot.wlist.pop_front();
                    auto [ec, sz] = co_await asio::async_write(
                        *t,
                        asio::buffer(front),
                        asio::as_tuple(asio::use_awaitable)
                    );
                    if (ec) {
                        auto error = asio::system_error{ec};
                        socket_message result;
                        result.id = id;
                        result.opaque = slot.opaque;
                        result.data.str_view = error.what();
                        forward_message(skynet_socket_type::error, result);
                    }
                }
            }
        }

    }, asio::detached);
    return 0;
}

void cpp_sk::server::close_socket(context_t* ctx, int id)
{
    poll.post([this, id](){
        auto& slot = get_slot(id);
        if(slot.id != id || slot.type == socket_type::invalid) {
            return;
        }

        if (slot.close) {
            slot.clear();
            return;
        }

        socket_message result;
        result.id = id;
        result.opaque = slot.opaque;

        bool shutdown_read = slot.type == socket_type::halfclose_read;
        if(slot.wlist.empty()) {
            slot.clear();
            if (!shutdown_read) {
               forward_message(skynet_socket_type::close, result);   
            }
            return;
        }
        slot.close = true;
        if(!shutdown_read) {
            asio::error_code ignore;
            slot.tcp()->shutdown(asio::socket_base::shutdown_receive, ignore);
            forward_message(skynet_socket_type::close, result);
            return;
        }
    });
}

void cpp_sk::server::shutdown_socket(context_t* ctx, int id)
{
    poll.post([this, id](){
        auto& slot = get_slot(id);
        if(slot.id != id || slot.type == socket_type::invalid) {
            return;
        }

        if (slot.close) {
            slot.clear();
            return;
        }

        socket_message result;
        result.id = id;
        result.opaque = slot.opaque;

        slot.clear();

        if(slot.type != socket_type::halfclose_read) {
            forward_message(skynet_socket_type::close, result);
        }
    });
}

void server::forward_message(skynet_socket_type type, socket_message& result) {
  skynet_message smsg;
  skynet_socket_message m;
  m.type = type;
  m.id = result.id;
  m.ud = result.ud;

  m.buffer = std::move(result.data);
  smsg.data = std::move(m);

  smsg.source = 0;
  smsg.session = 0;
  smsg.sz |= ((size_t)PTYPE_SOCKET << MESSAGE_TYPE_SHIFT);

  if(!skynet_app::context_push(result.opaque, smsg)) {
    
  }
}