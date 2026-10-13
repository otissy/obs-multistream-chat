// L2 (pure request handling) and L3 (real sockets) tests for the local overlay server.
#include "oms_test.hpp"

#include "../../../src/core/http_server.hpp"
#include "../../../src/core/json.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET tsock;
static const tsock kBadSock = INVALID_SOCKET;
static void tclose(tsock s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
typedef int tsock;
static const tsock kBadSock = -1;
static void tclose(tsock s) { ::close(s); }
#endif

using namespace oms;

// ---- helpers -------------------------------------------------------------------------------

static void net_init()
{
#ifdef _WIN32
	static bool done = false;
	if (!done) {
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
		done = true;
	}
#endif
}

static AssetMap test_assets()
{
	AssetMap m;
	Asset html;
	html.content_type = "text/html; charset=utf-8";
	html.body = "<!doctype html><title>overlay</title>";
	m["/overlay.html"] = html;
	Asset js;
	js.content_type = "text/javascript; charset=utf-8";
	js.body = "console.log('x');";
	m["/overlay.js"] = js;
	return m;
}

static HttpRequest req(const std::string &method, const std::string &target, const std::string &host)
{
	HttpRequest r;
	r.method = method;
	r.target = target;
	size_t q = target.find('?');
	r.path = q == std::string::npos ? target : target.substr(0, q);
	r.version = "HTTP/1.1";
	if (!host.empty())
		r.headers["host"] = host;
	return r;
}

static std::string header_of(const HttpResponse &r, const std::string &name)
{
	for (size_t i = 0; i < r.headers.size(); ++i)
		if (r.headers[i].first == name)
			return r.headers[i].second;
	return std::string();
}

static bool has_header(const HttpResponse &r, const std::string &name)
{
	for (size_t i = 0; i < r.headers.size(); ++i)
		if (r.headers[i].first == name)
			return true;
	return false;
}

// ---- L2: parsing ---------------------------------------------------------------------------

TEST(L2, http_parse_valid_request)
{
	HttpRequest r;
	ParseStatus s = parse_request("GET /overlay.html?theme=dark&size=48 HTTP/1.1\r\nHost: 127.0.0.1:5000\r\n"
				      "Accept: */*\r\n\r\n",
				      r);
	CHECK(s == ParseStatus::Ok);
	CHECK_EQ(r.method, std::string("GET"));
	CHECK_EQ(r.path, std::string("/overlay.html"));
	CHECK_EQ(r.query, std::string("theme=dark&size=48"));
	CHECK_EQ(r.header("host"), std::string("127.0.0.1:5000"));
	CHECK_EQ(r.header("accept"), std::string("*/*"));
}

TEST(L2, http_parse_incomplete_and_too_large)
{
	HttpRequest r;
	CHECK(parse_request("GET / HTTP/1.1\r\nHost: x", r) == ParseStatus::Incomplete);
	CHECK(parse_request("", r) == ParseStatus::Incomplete);
	std::string huge = "GET / HTTP/1.1\r\nX: " + std::string(9000, 'a');
	CHECK(parse_request(huge, r) == ParseStatus::TooLarge);
	std::string huge_done = huge + "\r\n\r\n";
	CHECK(parse_request(huge_done, r) == ParseStatus::TooLarge);
}

TEST(L2, http_parse_rejects_malformed)
{
	HttpRequest r;
	CHECK(parse_request("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", r) == ParseStatus::Malformed); // two Hosts
	CHECK(parse_request("GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", r) == ParseStatus::Malformed);   // folding
	CHECK(parse_request("get / HTTP/1.1\r\nHost: a\r\n\r\n", r) == ParseStatus::Malformed);              // method case
	CHECK(parse_request("GET http://evil/ HTTP/1.1\r\nHost: a\r\n\r\n", r) == ParseStatus::Malformed);   // absolute-form
	CHECK(parse_request("GET / HTTP/2.0\r\nHost: a\r\n\r\n", r) == ParseStatus::Malformed);
	CHECK(parse_request("GET /a b HTTP/1.1\r\nHost: a\r\n\r\n", r) == ParseStatus::Malformed);
	CHECK(parse_request("GET /\\x HTTP/1.1\r\nHost: a\r\n\r\n", r) == ParseStatus::Malformed);
	CHECK(parse_request("GET / HTTP/1.1\r\nHost a\r\n\r\n", r) == ParseStatus::Malformed);
	CHECK(parse_request("GET / HTTP/1.1\r\nHo st: a\r\n\r\n", r) == ParseStatus::Malformed);
	CHECK(parse_request("GET / HTTP/1.1\r\nHost: a\nX: y\r\n\r\n", r) == ParseStatus::Malformed); // bare LF
	CHECK(parse_request(std::string("GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n", 31), r) == ParseStatus::Malformed);
	CHECK(parse_request("\r\n\r\n", r) == ParseStatus::Malformed);
}

// ---- L2: peer, host, origin ----------------------------------------------------------------

TEST(L2, loopback_peer_detection)
{
	CHECK(is_loopback_ipv4("127.0.0.1"));
	CHECK(is_loopback_ipv4("127.5.6.7"));
	CHECK_FALSE(is_loopback_ipv4("0.0.0.0"));
	CHECK_FALSE(is_loopback_ipv4("192.168.1.20"));
	CHECK_FALSE(is_loopback_ipv4("10.0.0.1"));
	CHECK_FALSE(is_loopback_ipv4("128.0.0.1"));
	CHECK_FALSE(is_loopback_ipv4("127.0.0.1.evil"));
	CHECK_FALSE(is_loopback_ipv4("127.0.0"));
	CHECK_FALSE(is_loopback_ipv4("127.0.0.256"));
	CHECK_FALSE(is_loopback_ipv4("::1"));
	CHECK_FALSE(is_loopback_ipv4(""));
	CHECK_FALSE(is_loopback_ipv4("127.0.0.1 "));
}

TEST(L2, host_header_allowlist_blocks_dns_rebinding)
{
	CHECK(host_allowed("127.0.0.1:5000", 5000));
	CHECK(host_allowed("localhost:5000", 5000));
	CHECK(host_allowed("LOCALHOST:5000", 5000));
	CHECK_FALSE(host_allowed("evil.example:5000", 5000));
	CHECK_FALSE(host_allowed("evil.example", 5000));
	CHECK_FALSE(host_allowed("127.0.0.1", 5000));            // port missing
	CHECK_FALSE(host_allowed("127.0.0.1:5001", 5000));       // wrong port
	CHECK_FALSE(host_allowed("127.0.0.1.evil.example:5000", 5000));
	CHECK_FALSE(host_allowed("evil.example:5000@127.0.0.1:5000", 5000));
	CHECK_FALSE(host_allowed("[::1]:5000", 5000));
	CHECK_FALSE(host_allowed("", 5000));
	CHECK_FALSE(host_allowed("localhost:5000", 0));
}

TEST(L2, origin_must_match_when_present)
{
	CHECK(origin_allowed("http://127.0.0.1:5000", 5000));
	CHECK(origin_allowed("http://localhost:5000", 5000));
	CHECK_FALSE(origin_allowed("http://evil.example", 5000));
	CHECK_FALSE(origin_allowed("null", 5000));
	CHECK_FALSE(origin_allowed("https://127.0.0.1:5000", 5000));
	CHECK_FALSE(origin_allowed("http://127.0.0.1:5001", 5000));
}

// ---- L2: routing ---------------------------------------------------------------------------

TEST(L2, route_serves_allowlisted_assets_and_state)
{
	StateHub hub;
	AssetMap a = test_assets();
	HttpResponse r = handle_request(req("GET", "/overlay.html?theme=dark", "127.0.0.1:5000"), 5000, a, hub);
	CHECK_EQ(r.status, 200);
	CHECK_EQ(r.body, a["/overlay.html"].body);
	CHECK_EQ(header_of(r, "Content-Type"), std::string("text/html; charset=utf-8"));
	r = handle_request(req("GET", "/", "localhost:5000"), 5000, a, hub);
	CHECK_EQ(r.status, 200);
	CHECK_EQ(r.body, a["/overlay.html"].body);
	r = handle_request(req("GET", "/state.json", "127.0.0.1:5000"), 5000, a, hub);
	CHECK_EQ(r.status, 200);
	CHECK(json::parse(r.body).ok);
	CHECK(header_of(r, "Content-Type").find("application/json") == 0);
	r = handle_request(req("GET", "/events", "127.0.0.1:5000"), 5000, a, hub);
	CHECK_EQ(r.status, 200);
	CHECK(r.sse);
	CHECK(header_of(r, "Content-Type").find("text/event-stream") == 0);
}

TEST(L2, route_refuses_rebinding_host_everywhere)
{
	StateHub hub;
	AssetMap a = test_assets();
	const char *paths[] = {"/", "/overlay.html", "/overlay.js", "/state.json", "/events", "/nope"};
	for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
		HttpResponse r = handle_request(req("GET", paths[i], "attacker.example:5000"), 5000, a, hub);
		CHECK_EQ(r.status, 403);
		CHECK_FALSE(r.sse);
		CHECK(r.body.find("overlay") == std::string::npos);
		CHECK(r.body.find("destinations") == std::string::npos);
	}
	HttpResponse r = handle_request(req("GET", "/state.json", ""), 5000, a, hub); // no Host at all
	CHECK_EQ(r.status, 403);
}

TEST(L2, route_cross_origin_and_fetch_site_rules)
{
	StateHub hub;
	AssetMap a = test_assets();
	HttpRequest q = req("GET", "/state.json", "127.0.0.1:5000");
	q.headers["origin"] = "http://evil.example";
	CHECK_EQ(handle_request(q, 5000, a, hub).status, 403);
	q.headers["origin"] = "http://127.0.0.1:5000";
	CHECK_EQ(handle_request(q, 5000, a, hub).status, 200);
	q.headers.erase("origin");
	q.headers["sec-fetch-site"] = "cross-site";
	CHECK_EQ(handle_request(q, 5000, a, hub).status, 403);
	q.headers["sec-fetch-site"] = "same-origin";
	CHECK_EQ(handle_request(q, 5000, a, hub).status, 200);
	q.headers["sec-fetch-site"] = "none";
	CHECK_EQ(handle_request(q, 5000, a, hub).status, 200);
	HttpRequest ev = req("GET", "/events", "127.0.0.1:5000");
	ev.headers["sec-fetch-site"] = "same-site";
	CHECK_EQ(handle_request(ev, 5000, a, hub).status, 403);
}

TEST(L2, route_is_read_only)
{
	StateHub hub;
	AssetMap a = test_assets();
	const char *methods[] = {"POST", "PUT", "DELETE", "PATCH", "OPTIONS", "CONNECT", "TRACE"};
	for (size_t i = 0; i < sizeof methods / sizeof methods[0]; ++i) {
		HttpResponse r = handle_request(req(methods[i], "/state.json", "127.0.0.1:5000"), 5000, a, hub);
		CHECK_EQ(r.status, 405);
		CHECK_EQ(header_of(r, "Allow"), std::string("GET, HEAD"));
	}
	HttpResponse head = handle_request(req("HEAD", "/overlay.html", "127.0.0.1:5000"), 5000, a, hub);
	CHECK_EQ(head.status, 200);
	std::string wire = serialize_response(head, true);
	CHECK(wire.find("Content-Length: " + std::to_string(a["/overlay.html"].body.size())) != std::string::npos);
	CHECK(wire.find("<!doctype") == std::string::npos); // no body on HEAD
}

TEST(L2, route_no_traversal_and_no_unlisted_files)
{
	StateHub hub;
	AssetMap a = test_assets();
	const char *bad[] = {"/../config.json", "/..%2fconfig.json", "/%2e%2e/config.json", "/icons/../../x",
			     "/overlay.html/", "/overlay.html/..", "/config.json", "/overlay.JS", "//overlay.js",
			     "/overlay.js%00.html"};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		HttpResponse r = handle_request(req("GET", bad[i], "127.0.0.1:5000"), 5000, a, hub);
		CHECK_EQ(r.status, 404);
	}
}

TEST(L2, route_never_sends_cors_headers_and_always_sends_hardening)
{
	StateHub hub;
	AssetMap a = test_assets();
	const char *paths[] = {"/overlay.html", "/overlay.js", "/state.json", "/events", "/missing"};
	for (size_t i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
		HttpResponse r = handle_request(req("GET", paths[i], "127.0.0.1:5000"), 5000, a, hub);
		for (size_t k = 0; k < r.headers.size(); ++k)
			CHECK(r.headers[k].first.find("Access-Control") == std::string::npos);
		CHECK_EQ(header_of(r, "X-Content-Type-Options"), std::string("nosniff"));
		CHECK_EQ(header_of(r, "Cache-Control"), std::string("no-store"));
		CHECK(has_header(r, "Content-Security-Policy"));
		CHECK(header_of(r, "Content-Security-Policy").find("default-src 'none'") == 0);
	}
	// the wildcard must not appear anywhere in a serialized response either
	HttpResponse r = handle_request(req("GET", "/state.json", "127.0.0.1:5000"), 5000, a, hub);
	CHECK(serialize_response(r, false).find("Access-Control") == std::string::npos);
}

// ---- L3: real sockets ----------------------------------------------------------------------

static tsock tconnect(int port, const char *ip = "127.0.0.1")
{
	net_init();
	tsock s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == kBadSock)
		return kBadSock;
	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(static_cast<unsigned short>(port));
	inet_pton(AF_INET, ip, &a.sin_addr);
	// non-blocking connect with a 1.5 s cap: an address that silently drops SYNs must not stall the suite
#ifdef _WIN32
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
	int rc = connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a);
	if (rc != 0) {
		fd_set wf, ef;
		FD_ZERO(&wf);
		FD_ZERO(&ef);
		FD_SET(s, &wf);
		FD_SET(s, &ef);
		struct timeval ctv;
		ctv.tv_sec = 1;
		ctv.tv_usec = 500000;
		int sel = select(static_cast<int>(s) + 1, nullptr, &wf, &ef, &ctv);
		int soerr = 1;
		socklen_t sl = sizeof soerr;
		if (sel > 0 && FD_ISSET(s, &wf))
			getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soerr), &sl);
		if (sel <= 0 || soerr != 0) {
			tclose(s);
			return kBadSock;
		}
	}
#ifdef _WIN32
	nb = 0;
	ioctlsocket(s, FIONBIO, &nb);
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
#endif
#ifdef _WIN32
	DWORD t = 3000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&t), sizeof t);
#else
	struct timeval tv;
	tv.tv_sec = 3;
	tv.tv_usec = 0;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
	return s;
}

static void tsend(tsock s, const std::string &data)
{
	send(s, data.data(), static_cast<int>(data.size()), 0);
}

// Reads until the peer closes, the timeout hits, or `until` (when non-empty) appears.
static std::string trecv(tsock s, const std::string &until = std::string())
{
	std::string out;
	char buf[4096];
	while (true) {
		int n = static_cast<int>(recv(s, buf, sizeof buf, 0));
		if (n <= 0)
			break;
		out.append(buf, static_cast<size_t>(n));
		if (!until.empty() && out.find(until) != std::string::npos)
			break;
	}
	return out;
}

static std::string http_exchange(int port, const std::string &raw)
{
	tsock s = tconnect(port);
	if (s == kBadSock)
		return "<connect failed>";
	tsend(s, raw);
	std::string r = trecv(s);
	tclose(s);
	return r;
}

static std::string get(int port, const std::string &path, const std::string &host_override = std::string())
{
	std::string host = host_override.empty() ? "127.0.0.1:" + std::to_string(port) : host_override;
	return http_exchange(port, "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");
}

static std::vector<OverlayEntry> one_entry(State st)
{
	std::vector<OverlayEntry> v;
	OverlayEntry e;
	e.id = "d1";
	e.platform = "twitch";
	e.handle = "otiyyy";
	e.state = st;
	v.push_back(e);
	return v;
}

TEST(L3, server_starts_on_loopback_and_serves_everything)
{
	StateHub hub;
	hub.publish(one_entry(State::Live), true);
	HttpServer srv(hub, test_assets());
	std::string err;
	CHECK(srv.start(err));
	CHECK(srv.port() > 0);
	std::string page = get(srv.port(), "/overlay.html");
	CHECK(page.find("HTTP/1.1 200 OK") == 0);
	CHECK(page.find("<title>overlay</title>") != std::string::npos);
	std::string st = get(srv.port(), "/state.json");
	CHECK(st.find("\"state\":\"live\"") != std::string::npos);
	CHECK(st.find("\"handle\":\"otiyyy\"") != std::string::npos);
	CHECK(get(srv.port(), "/missing").find("404") != std::string::npos);
	srv.stop();
	CHECK_FALSE(srv.running());
}

TEST(L3, server_refuses_dns_rebinding_over_the_wire)
{
	StateHub hub;
	hub.publish(one_entry(State::Live), true);
	HttpServer srv(hub, test_assets());
	std::string err;
	CHECK(srv.start(err));
	std::string r = get(srv.port(), "/state.json", "attacker.example:" + std::to_string(srv.port()));
	CHECK(r.find("HTTP/1.1 403") == 0);
	CHECK(r.find("otiyyy") == std::string::npos);
	r = get(srv.port(), "/state.json", "127.0.0.1"); // right host, no port
	CHECK(r.find("HTTP/1.1 403") == 0);
	r = http_exchange(srv.port(), "GET /state.json HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) +
					 "\r\nOrigin: http://attacker.example\r\n\r\n");
	CHECK(r.find("HTTP/1.1 403") == 0);
	r = http_exchange(srv.port(), "GET /state.json HTTP/1.1\r\n\r\n"); // no Host header
	CHECK(r.find("HTTP/1.1 403") == 0);
	srv.stop();
}

TEST(L3, server_drops_non_loopback_peers_without_a_response)
{
	StateHub hub;
	HttpServer srv(hub, test_assets());
	srv.set_peer_override_for_test("192.168.1.50"); // pretend the connection came from the LAN
	std::string err;
	CHECK(srv.start(err));
	std::string r = get(srv.port(), "/state.json");
	CHECK(r.empty() || r == "<connect failed>");
	CHECK(r.find("HTTP/") == std::string::npos);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK(srv.refused_peers() >= 1);
	srv.stop();
}

TEST(L3, server_socket_is_bound_to_loopback_only)
{
	// No network probing: ask the OS which address the listening socket is bound to (getsockname)
	// instead of connecting to any other address.
	StateHub hub;
	HttpServer srv(hub, test_assets());
	std::string err;
	CHECK(srv.start(err));
	CHECK_EQ(srv.bound_address(), std::string("127.0.0.1"));
	CHECK(srv.bound_address() != "0.0.0.0");
	CHECK(is_loopback_ipv4(srv.bound_address()));
	srv.stop();
}

TEST(L3, server_port_collision_falls_back_to_a_free_port)
{
	StateHub hub;
	HttpServer first(hub, test_assets());
	std::string err;
	CHECK(first.start(err));
	ServerOptions o;
	o.preferred_port = first.port(); // taken
	HttpServer second(hub, test_assets(), o);
	CHECK(second.start(err));
	CHECK(second.port_fell_back());
	CHECK(second.port() != first.port());
	CHECK(get(second.port(), "/state.json").find("200 OK") != std::string::npos);
	CHECK(get(first.port(), "/state.json").find("200 OK") != std::string::npos);
	second.stop();
	first.stop();
}

TEST(L3, server_uses_preferred_port_when_free)
{
	StateHub hub;
	int free_port = 0;
	{
		HttpServer probe(hub, test_assets());
		std::string err;
		CHECK(probe.start(err));
		free_port = probe.port();
		probe.stop();
	}
	ServerOptions o;
	o.preferred_port = free_port;
	HttpServer srv(hub, test_assets(), o);
	std::string err;
	CHECK(srv.start(err));
	CHECK_EQ(srv.port(), free_port);
	CHECK_FALSE(srv.port_fell_back());
	srv.stop();
}

TEST(L3, server_rejects_oversized_and_malformed_requests)
{
	StateHub hub;
	HttpServer srv(hub, test_assets());
	std::string err;
	CHECK(srv.start(err));
	std::string r = http_exchange(srv.port(), "GET / HTTP/1.1\r\nHost: x\r\nX: " + std::string(20000, 'a') + "\r\n\r\n");
	CHECK(r.find("HTTP/1.1 431") == 0);
	r = http_exchange(srv.port(), "NOT-HTTP\r\n\r\n");
	CHECK(r.find("HTTP/1.1 400") == 0);
	r = http_exchange(srv.port(), "POST /state.json HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) +
					 "\r\nContent-Length: 4\r\n\r\nabcd");
	CHECK(r.find("HTTP/1.1 405") == 0);
	srv.stop();
}

TEST(L3, server_times_out_idle_connections)
{
	StateHub hub;
	ServerOptions o;
	o.read_timeout_ms = 300;
	HttpServer srv(hub, test_assets(), o);
	std::string err;
	CHECK(srv.start(err));
	tsock s = tconnect(srv.port());
	CHECK(s != kBadSock);
	auto t0 = std::chrono::steady_clock::now();
	std::string r = trecv(s); // send nothing: the server must hang up on its own
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	CHECK(r.empty());
	CHECK(ms < 2500);
	tclose(s);
	srv.stop();
}

TEST(L3, sse_streams_updates_and_stop_is_prompt)
{
	StateHub hub;
	hub.publish(one_entry(State::Connecting), true);
	ServerOptions o;
	o.heartbeat_ms = 500;
	HttpServer srv(hub, test_assets(), o);
	std::string err;
	CHECK(srv.start(err));
	tsock s = tconnect(srv.port());
	CHECK(s != kBadSock);
	tsend(s, "GET /events HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) + "\r\n\r\n");
	std::string got = trecv(s, "\"state\":\"connecting\"");
	CHECK(got.find("text/event-stream") != std::string::npos);
	CHECK(got.find("data: {") != std::string::npos);
	CHECK(got.find("Access-Control") == std::string::npos);
	hub.publish(one_entry(State::Live), true);
	std::string more = trecv(s, "\"state\":\"live\"");
	CHECK(more.find("\"state\":\"live\"") != std::string::npos);
	// heartbeat arrives when nothing changes
	std::string hb = trecv(s, ": hb");
	CHECK(hb.find(": hb") != std::string::npos);
	auto t0 = std::chrono::steady_clock::now();
	srv.stop(); // must not hang on the open event stream
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	CHECK(ms < 3000);
	tclose(s);
}

TEST(L3, sse_client_cap_returns_503)
{
	StateHub hub;
	ServerOptions o;
	o.max_sse = 1;
	HttpServer srv(hub, test_assets(), o);
	std::string err;
	CHECK(srv.start(err));
	std::string req_text = "GET /events HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) + "\r\n\r\n";
	tsock a = tconnect(srv.port());
	tsend(a, req_text);
	CHECK(trecv(a, "data: {").find("data: {") != std::string::npos);
	std::string second = http_exchange(srv.port(), req_text);
	CHECK(second.find("HTTP/1.1 503") == 0);
	srv.stop();
	tclose(a);
}
