// Local, read-only HTTP server for the browser-source overlay.
//   - binds 127.0.0.1 only; the peer address of every connection is re-checked
//   - GET/HEAD only, fixed allowlist: the asset map, /state.json, /events (SSE), "/" -> overlay.html
//   - Host header must be 127.0.0.1:<port> or localhost:<port> (DNS-rebinding defence)
//   - a present Origin must be the same origin; no Access-Control-Allow-* header is ever sent
// The request handling (parse_request, handle_request, host_allowed...) is pure and unit tested
// without sockets; HttpServer is the thin socket layer around it.
#pragma once
#include "state_feed.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace oms {

struct Asset {
	std::string content_type;
	std::string body;
};
typedef std::map<std::string, Asset> AssetMap; // key: "/overlay.html"

struct HttpRequest {
	std::string method;
	std::string target;
	std::string path;
	std::string query;
	std::string version;
	std::map<std::string, std::string> headers; // lower-case names
	std::string header(const std::string &lower_name) const
	{
		std::map<std::string, std::string>::const_iterator it = headers.find(lower_name);
		return it == headers.end() ? std::string() : it->second;
	}
	bool has_header(const std::string &lower_name) const { return headers.find(lower_name) != headers.end(); }
};

enum class ParseStatus { Ok, Incomplete, TooLarge, Malformed };

// Parses the request line and headers from buf (which may hold more bytes after the blank line).
ParseStatus parse_request(const std::string &buf, HttpRequest &out, size_t max_header_bytes = 8192);

struct HttpResponse {
	int status = 200;
	std::string reason = "OK";
	std::vector<std::pair<std::string, std::string> > headers;
	std::string body;
	bool sse = false; // headers only; the socket layer then streams /events
};

bool is_loopback_ipv4(const std::string &dotted); // 127.0.0.0/8 only
bool host_allowed(const std::string &host_header, int port);
bool origin_allowed(const std::string &origin, int port);

HttpResponse handle_request(const HttpRequest &req, int port, const AssetMap &assets, StateHub &hub);
std::string serialize_response(const HttpResponse &r, bool head_only);

struct ServerOptions {
	int preferred_port = 0;       // 0 = any free port. Tried first when non-zero.
	int max_connections = 8;
	int max_sse = 4;
	int read_timeout_ms = 5000;
	int heartbeat_ms = 15000;
};

class HttpServer {
public:
	HttpServer(StateHub &hub, const AssetMap &assets, const ServerOptions &opts = ServerOptions());
	~HttpServer();

	bool start(std::string &err);
	void stop();
	bool running() const { return running_; }
	int port() const { return port_; }
	// Address the listening socket is bound to, as reported by the OS after bind ("127.0.0.1").
	const std::string &bound_address() const { return bound_address_; }
	// true when preferred_port was requested but taken, so another port was chosen
	bool port_fell_back() const { return fell_back_; }

	// Test hook: treat every accepted connection as coming from this IP (set before start()).
	void set_peer_override_for_test(const std::string &ip) { peer_override_ = ip; }
	size_t refused_peers() const { return refused_peers_; }

private:
	void accept_loop();
	void serve_connection(int64_t sock);
	void serve_sse(int64_t sock);

	StateHub &hub_;
	AssetMap assets_;
	ServerOptions opts_;
	std::string peer_override_;

	int64_t listen_sock_;
	int port_;
	std::string bound_address_;
	bool fell_back_;
	std::atomic<bool> running_;
	std::thread accept_thread_;

	std::mutex conn_m_;
	std::condition_variable conn_cv_;
	int active_conns_;
	std::set<int64_t> clients_;
	std::atomic<int> sse_clients_;
	std::atomic<size_t> refused_peers_;
	bool wsa_started_;
};

} // namespace oms
