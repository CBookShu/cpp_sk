#pragma once
#include "asio/any_io_executor.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio/ip/udp.hpp"
#include "utils.h"
#include <asio.hpp>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <list>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cpp_sk {
/*
* 

skynet socket process
ctx: worker thread
server single thread poll

socket::type state transition
invalid
	get-> reserve
	
listen->plisten
	start->listen

paccept[listen accept new fd]:
	start-> connected

connect->
	at once call: connect
		ok:	connected
		err[in process]: connecting
			poll ok:
				connected
	start:
		type is keep

	

skynet_socket_connect
	'O'

poll:
	open_socket
		getaddrinfo-> socket -> keepalive,noblock-> connect
			connect ok: SOCKET_TYPE_CONNECTED,SOCKET_OPEN
			connect err: SOCKET_TYPE_CONNECTING,continue
	SOCKET_OPEN
		data = ip
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		
		
skynet_socket_listen
	do_listen
	'L'
poll:
	listen_socket
		SOCKET_TYPE_PLISTEN
		data = error
			SOCKET_OPEN or SOCKET_ERR
		
		forward_message(SKYNET_SOCKET_TYPE_CONNECT, true, &result);
		

skynet_socket_close
	'K'
poll:
	close_socket
		-1	when write buff not empty; shutdown read first and close later
		SOCKET_CLOSE
	
	SOCKET_CLOSE:
		SKYNET_SOCKET_TYPE_CLOSE
	

skynet_socket_start
	socket_server_start
	'R'
poll:
	paccept -> connected
	plisten->listen
	connected->connected
	
	all fd-> enable read
	
	
here ignore it
socket_server_shutdown
	'K'
poll:
	close_socket
		must call force_close
		
		

skynet_socket_sendbuffer
	write at once		'W'
	async write 		'D'
	
poll:
	'W'		

*/
struct context_t;

enum class skynet_socket_type {
  data = 1,
  connect = 2,
  close = 3,
  accept = 4,
  error = 5,
  udp = 6,
  warning = 7
};

struct complie_str {
  std::string_view str;
  consteval complie_str(std::string_view s) : str(s) {}

  consteval complie_str(const char *s) : str(s) {}
};

struct string_buf {
private:
  std::string str;
  std::string_view str_view;

public:
  void const_assgin(complie_str s) { str_view = s.str; }
  void assgin(std::string s) { str = std::move(s); }

  size_t size() {
    if (!str.empty()) {
      return str.size();
    }
    if (!str_view.empty()) {
      return str_view.size();
    }
    return 0;
  }
  const char *ptr() {
    if (!str.empty()) {
      return str.data();
    }
    if (!str_view.empty()) {
      return str_view.data();
    }
    return nullptr;
  }
};

struct socket_message {
  int id = 0;
  uintptr_t opaque = 0;
  int ud =
      0; // for accept, ud is new connection id ; for data, ud is size of data
  string_buf data;
};

struct skynet_socket_message {
  skynet_socket_type type;
  int id;
  int ud;
  string_buf buffer;
};

enum class socket_type : int {
  invalid = 0,
  reserve = 1,
  plisten = 2,
  listen = 3,
  connecting = 4,
  connected = 5,
  halfclose_read = 6,
  halfclose_write = 7,
  paccept = 8,
  bind = 9,
};

struct socket_t {
  uintptr_t opaque;
  int id = -1;

  using tcp_ns = asio::ip::tcp;
  using udp_ns = asio::ip::udp;
  std::variant<std::monostate, tcp_ns::socket, tcp_ns::acceptor, udp_ns::socket>
      sock;

  tcp_ns::socket *tcp() { return std::get_if<tcp_ns::socket>(&sock); }

  tcp_ns::acceptor *acceptor() { return std::get_if<tcp_ns::acceptor>(&sock); }

  udp_ns::socket *udp() { return std::get_if<udp_ns::socket>(&sock); }

  std::atomic<socket_type> type{socket_type::invalid};

  // this list just read and write in poll thread
  std::deque<std::string> wlist;
  spinlock_mutex wlist_lock;

  std::atomic_bool close{false};

  udp_ns::endpoint ep;

  bool check_pstart(int id);
  bool check_can_start(int id);
  bool check_can_write(int id);

  void clear() {
    wlist.clear();
    opaque = 0;
    id = -1;

    close = false;

    ep = {};

    sock = std::monostate{};
  }
};

struct server {
  asio::io_context poll;
  std::vector<socket_t> slots;
  std::atomic_size_t alloc;

  server() : poll{1}, alloc{1}, slots{65535} {}

  int new_fd();
  auto &get_slot(int id);

  int socket_listen(context_t *ctx, const char *host, int port, int backlog);
  int socket_connect(context_t *ctx, const char *host, int port);
  void socket_start(context_t *ctx, int id);
  int socket_send(context_t *ctx, int id, std::string buf);
  void close_socket(context_t *ctx, int id);

  void forward_message(skynet_socket_type type, socket_message &result);

  auto start(int id) -> asio::awaitable<void>;

  static server &ins() {
    static server s;
    return s;
  }
};

} // namespace cpp_sk