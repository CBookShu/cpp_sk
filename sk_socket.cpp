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
    slot.type.store(socket_type::connecting);

    tcp::resolver::query query(host, std::to_string(port));
    asio::co_spawn(poll, [this, &slot, id = id, query=std::move(query)]()->asio::awaitable<void>{
      auto& slot = get_slot(id);
      if (slot.type != socket_type::connecting || slot.id != id) {
        co_return;
      }
      socket_message result;
      result.id = id;
      result.opaque = slot.opaque;
      try {

        tcp::resolver resolver(poll);
        auto it = co_await resolver.async_resolve(query, asio::use_awaitable);

        auto ep = co_await asio::async_connect(*slot.tcp(), it, asio::use_awaitable);

        slot.type.store(socket_type::connected);

        result.data.assgin(ep.address().to_string());
        forward_message(skynet_socket_type::connect, result);
        
      } catch(std::exception& e) {
        result.data.assgin(e.what());
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
    if(!slot.check_pstart(id)) {
        result.data.const_assgin("invalid socket");
        forward_message(skynet_socket_type::error, result);
        return;
    }

    if (slot.type == socket_type::plisten) {
        slot.type.store(socket_type::listen);
    } else {
        slot.type.store(socket_type::connected);
    }
    slot.opaque = handle;

    asio::co_spawn(poll, [this, id=id, handle=handle]()->asio::awaitable<void>{
       co_return co_await start(id);
    }, asio::detached);

    forward_message(skynet_socket_type::connect, result);
  });
}

int cpp_sk::server::socket_send(context_t* ctx, int id, std::string buf)
{
    auto& slot = get_slot(id);
    if (!slot.check_can_write(id)) {
        return -1;
    }

    size_t offset = 0;
    [&](){
        if (slot.wlist_lock.trylock()) {
            std::lock_guard guard(slot.wlist_lock, std::adopt_lock);
            if (!slot.check_can_write(id)) {
                return;
            }
            if (!slot.wlist.empty()) {
                return;
            }

            asio::error_code ec;
            std::visit(
                overload(
                    [&](socket_t::tcp_ns::socket& s) {
                        offset = s.write_some(asio::buffer(buf), ec);
                    },
                    [&](socket_t::udp_ns::socket& s) {
                        // this function, udp socket is connect remote
                        offset = s.send(asio::buffer(buf),0, ec);
                    },
                    [](auto&&) {
                        assert(false);
                    }
                ),
                slot.sock
            );
        }
    }();

    asio::co_spawn(poll, 
        [this, id = id, handle = ctx->handle,
        offset, buf = std::move(buf)]() -> asio::awaitable<void> {
        auto& slot = get_slot(id);
        if (!slot.check_can_write(id)) {
            co_return;
        }

        socket_message result;
        result.id = slot.id;

        if (!slot.wlist_lock.trylock()) {
            // writting is doing
            // append buff
            if (offset == 0) {
                slot.wlist.emplace_back(std::move(buf));
            } else {
                slot.wlist.emplace_back(buf.substr(offset));
            }
            co_return;
        }
        asio::error_code ec;
        {
            std::lock_guard guard(slot.wlist_lock, std::adopt_lock);
            std::visit(
                overload(
                    [&](socket_t::tcp_ns::socket& s) -> asio::awaitable<void> {
                        while (!slot.wlist.empty()) {
                            auto [ec, sz] = 
                                co_await asio::async_write(s, 
                                    asio::buffer(slot.wlist.front()), 
                                    asio::as_tuple(asio::use_awaitable)
                                );
                            if (ec) {
                                asio::error_code ignore;
                                s.shutdown(asio::socket_base::shutdown_send, ignore);
                                co_return;
                            }
                            slot.wlist.pop_front();
                        }
                        co_return;
                    },
                    [&](socket_t::udp_ns::socket& s) -> asio::awaitable<void> {
                        while (!slot.wlist.empty()) {
                            auto [ec, sz] = 
                                co_await s.async_send(
                                    asio::buffer(slot.wlist.front()),
                                    asio::as_tuple(asio::use_awaitable)
                                );
                                
                            if (ec) {
                                asio::error_code ignore;
                                s.shutdown(asio::socket_base::shutdown_send, ignore);
                                co_return;
                            }
                            slot.wlist.pop_front();
                        }
                        co_return;
                    },
                    [](auto&&...args) -> asio::awaitable<void> {
                        assert(false);
                        co_return;
                    }
                ),
                slot.sock
            );
        }

        // write fail
        if (slot.type == socket_type::halfclose_read
            || slot.close) {
            slot.clear();
            slot.type.store(socket_type::invalid);
        }
        else {
            result.opaque = slot.opaque;
            result.data.assgin(ec.message());
            slot.type.store(socket_type::halfclose_write);
            forward_message(skynet_socket_type::error, result);
        }
        co_return;
    }, asio::detached);
    return 0;
}

void cpp_sk::server::close_socket(context_t* ctx, int id)
{
    poll.post([this, id](){
        auto& slot = get_slot(id);
        if(slot.id != id 
            || slot.type == socket_type::invalid
            || slot.close) {
            return;
        }

        socket_message result;
        result.id = id;
        result.opaque = slot.opaque;

        auto read_shutdown = slot.type == socket_type::halfclose_read;
        bool check_write = false;
        std::visit(
            overload(
                [&](socket_t::tcp_ns::socket& s) {
                    asio::error_code ignore;
                    if(!read_shutdown) {
                        s.shutdown(asio::socket_base::shutdown_receive, ignore);
                        slot.type.store(socket_type::halfclose_read);

                        socket_message result;
                        result.id = slot.id;
                        result.opaque = slot.opaque;
                        forward_message(skynet_socket_type::close, result);
                    }
                    if (slot.wlist.empty() && slot.wlist_lock.trylock()) {
                        std::lock_guard guard(slot.wlist_lock, std::adopt_lock);
                        s.close(ignore);
                        slot.clear();
                        slot.type.store(socket_type::invalid);
                    } else {
                        slot.close = true;
                    }
                },
                [&](socket_t::udp_ns::socket& s) {
                    asio::error_code ignore;
                    if (!read_shutdown) {
                        s.shutdown(asio::socket_base::shutdown_receive, ignore);
                        slot.type.store(socket_type::halfclose_read);

                        socket_message result;
                        result.id = slot.id;
                        result.opaque = slot.opaque;
                        forward_message(skynet_socket_type::close, result);
                    }
                    if (slot.wlist.empty() && slot.wlist_lock.trylock()) {
                        std::lock_guard guard(slot.wlist_lock, std::adopt_lock);
                        s.close(ignore);
                        slot.clear();
                        slot.type.store(socket_type::invalid);
                    } else {
                        slot.close = true;
                    }
                },
                [&](socket_t::tcp_ns::acceptor& a) {
                    asio::error_code ignore;
                    bool close_ = false;
                    if (slot.close.compare_exchange_strong(close_, true)) {
                        // close now
                        a.close(ignore);
                        if (slot.type != socket_type::listen) {
                            // listen is doing, let it clear
                        } else {
                            // clear now
                            slot.clear();
                            slot.type.store(socket_type::invalid);
                        }
                    } else {
                        // listen crash and close it
                        slot.clear();
                        slot.type.store(socket_type::invalid);
                    }
                },
                [](auto&&...args) {
                    assert(false);
                }
            ),
            slot.sock
        );
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

  skynet_app::context_push(result.opaque, smsg);
}

auto cpp_sk::server::start(int id) -> asio::awaitable<void>
{
    using tcp = asio::ip::tcp;
    using udp = asio::ip::udp;
    auto& slot = get_slot(id);
    if (slot.id != id) {
        co_return;
    }

    co_return co_await std::visit
    (
        overload
        (
            [&](tcp::acceptor& a) ->asio::awaitable<void> {
                socket_message result;
                result.id = slot.id;
                try {
                    for(;;) {
                        if (slot.id != id
                            || slot.type != socket_type::listen) {
                            result.data.const_assgin("invalid socket");
                            forward_message(skynet_socket_type::error, result);
                            co_return;
                        }
                        
                        result.opaque = slot.opaque;

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
                    bool close_ = false;
                    if (slot.close.compare_exchange_strong(close_, true)) {
                        // ok close passive
                        // send err and ctx will close
                        result.opaque = slot.opaque;
                        result.data.assgin(e.what());
                        forward_message(skynet_socket_type::error, result);
                    } else {
                        // close by ctx
                        // ok ctx is out
                        slot.clear();
                        slot.type.store(socket_type::invalid);
                    }
                }
            },
            [&](tcp::socket& s) ->asio::awaitable<void> { 
                socket_message result;
                result.id = id;
                try {
                    char buff[65535];
                    for (;;) {
                        if (slot.id != id
                            || (slot.type != socket_type::connected && slot.type != socket_type::halfclose_write)) {
                            break;
                        }

                        result.opaque = slot.opaque;

                        auto sz = co_await s.async_read_some(asio::buffer(buff), asio::use_awaitable);
                        result.data.assgin(std::string(buff, sz));
                        forward_message(skynet_socket_type::data, result);
                    }
                }
                catch (std::exception& e) {
                    result.data.assgin(e.what());
                    forward_message(skynet_socket_type::error, result);
                }
            },
            [&](udp::socket& s) ->asio::awaitable<void>{
                // TODO: udp
                co_return;
            },
            [](auto&&) ->asio::awaitable<void> {
                co_return;
            }
        ),
        slot.sock
    );
}

bool cpp_sk::socket_t::check_pstart(int id_)
{
    if (id != id_) {
        return false;
    }

    if(close) {
        return false;
    }

    if (type == socket_type::paccept
        || type == socket_type::plisten
        || type == socket_type::connected) {
        return true;
    }
    return false;
}

bool cpp_sk::socket_t::check_can_start(int id_)
{
    /*
        connected,
        halfclose_write,
        listen
    */
    if(id != id_) {
        return false;
    }

    if(close) {
        return false;
    }

    if (type == socket_type::connected
        || type == socket_type::halfclose_write
        || type == socket_type::listen) {
        return true;
    }
    return false;
}

bool cpp_sk::socket_t::check_can_write(int id_)
{
    /*
        connected,
        halfclose_read,
    */
    if (id != id_) {
        return false;
    }

    if (close) {
        return false;
    }

    if (type == socket_type::connected
        || type == socket_type::halfclose_read) {
        return true;
    }

    return false;
}
