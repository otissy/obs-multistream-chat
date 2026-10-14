#include "http_server.hpp"

#include "chat_targets.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET native_sock;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int native_sock;
#endif

namespace oms {

namespace {

const int64_t kNoSock = -1;

std::string lower(const std::string &s)
{
	std::string o = s;
	for (size_t i = 0; i < o.size(); ++i)
		o[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(o[i])));
	return o;
}

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t'))
		++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
		--b;
	return s.substr(a, b - a);
}

bool is_token_char(char c)
{
	if (std::isalnum(static_cast<unsigned char>(c)))
		return true;
	return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != '\0';
}

const char *reason_for(int status)
{
	switch (status) {
	case 200: return "OK";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 401: return "Unauthorized";
	case 405: return "Method Not Allowed";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 415: return "Unsupported Media Type";
	case 431: return "Request Header Fields Too Large";
	case 503: return "Service Unavailable";
	default: return "Error";
	}
}

void add_common_headers(HttpResponse &r)
{
	r.headers.push_back(std::make_pair("Cache-Control", "no-store"));
	r.headers.push_back(std::make_pair("X-Content-Type-Options", "nosniff"));
	r.headers.push_back(std::make_pair("Referrer-Policy", "no-referrer"));
	r.headers.push_back(std::make_pair("Cross-Origin-Resource-Policy", "same-origin"));
	r.headers.push_back(std::make_pair(
		"Content-Security-Policy",
		"default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
		"connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"));
}

HttpResponse make_error(int status, const std::string &text)
{
	HttpResponse r;
	r.status = status;
	r.reason = reason_for(status);
	add_common_headers(r);
	r.headers.push_back(std::make_pair("Content-Type", "text/plain; charset=utf-8"));
	r.body = text + "\n";
	return r;
}

} // namespace

ParseStatus parse_request(const std::string &buf, HttpRequest &out, size_t max_header_bytes)
{
	size_t end = buf.find("\r\n\r\n");
	if (end == std::string::npos) {
		return buf.size() > max_header_bytes ? ParseStatus::TooLarge : ParseStatus::Incomplete;
	}
	if (end + 4 > max_header_bytes)
		return ParseStatus::TooLarge;
	if (buf.find('\0') != std::string::npos && buf.find('\0') < end)
		return ParseStatus::Malformed;

	std::string block = buf.substr(0, end);
	// walk CRLF-separated lines; any stray CR or LF inside a line is malformed
	std::vector<std::string> lines;
	size_t pos = 0;
	while (true) {
		size_t nl = block.find("\r\n", pos);
		std::string line = nl == std::string::npos ? block.substr(pos) : block.substr(pos, nl - pos);
		if (line.find('\r') != std::string::npos || line.find('\n') != std::string::npos)
			return ParseStatus::Malformed;
		lines.push_back(line);
		if (nl == std::string::npos)
			break;
		pos = nl + 2;
	}
	if (lines.empty() || lines[0].empty())
		return ParseStatus::Malformed;

	// request line: METHOD SP target SP HTTP/1.x
	const std::string &rl = lines[0];
	size_t s1 = rl.find(' ');
	size_t s2 = s1 == std::string::npos ? std::string::npos : rl.find(' ', s1 + 1);
	if (s1 == std::string::npos || s2 == std::string::npos || rl.find(' ', s2 + 1) != std::string::npos)
		return ParseStatus::Malformed;
	HttpRequest req;
	req.method = rl.substr(0, s1);
	req.target = rl.substr(s1 + 1, s2 - s1 - 1);
	req.version = rl.substr(s2 + 1);
	if (req.method.empty() || req.method.size() > 16)
		return ParseStatus::Malformed;
	for (size_t i = 0; i < req.method.size(); ++i)
		if (!std::isupper(static_cast<unsigned char>(req.method[i])))
			return ParseStatus::Malformed;
	if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0")
		return ParseStatus::Malformed;
	if (req.target.empty() || req.target.size() > 1024 || req.target[0] != '/')
		return ParseStatus::Malformed;
	for (size_t i = 0; i < req.target.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(req.target[i]);
		if (c <= 0x20 || c == 0x7F || c == '\\')
			return ParseStatus::Malformed;
	}
	size_t q = req.target.find('?');
	req.path = q == std::string::npos ? req.target : req.target.substr(0, q);
	req.query = q == std::string::npos ? std::string() : req.target.substr(q + 1);

	for (size_t i = 1; i < lines.size(); ++i) {
		const std::string &ln = lines[i];
		if (ln.empty())
			return ParseStatus::Malformed;
		if (ln[0] == ' ' || ln[0] == '\t')
			return ParseStatus::Malformed; // obsolete line folding
		size_t colon = ln.find(':');
		if (colon == std::string::npos || colon == 0)
			return ParseStatus::Malformed;
		std::string name = ln.substr(0, colon);
		for (size_t k = 0; k < name.size(); ++k)
			if (!is_token_char(name[k]))
				return ParseStatus::Malformed;
		std::string lname = lower(name);
		std::string value = trim(ln.substr(colon + 1));
		if (lname == "host" && req.headers.find("host") != req.headers.end())
			return ParseStatus::Malformed; // two Host headers: ambiguous, refuse
		req.headers[lname] = value;
	}
	req.header_bytes = end + 4;
	out = req;
	return ParseStatus::Ok;
}

bool is_loopback_ipv4(const std::string &dotted)
{
	int parts[4] = {0, 0, 0, 0};
	int idx = 0;
	size_t i = 0;
	if (dotted.empty())
		return false;
	while (idx < 4) {
		size_t start = i;
		int v = 0;
		while (i < dotted.size() && dotted[i] >= '0' && dotted[i] <= '9') {
			v = v * 10 + (dotted[i] - '0');
			if (v > 255)
				return false;
			++i;
		}
		if (i == start || i - start > 3)
			return false;
		parts[idx++] = v;
		if (idx < 4) {
			if (i >= dotted.size() || dotted[i] != '.')
				return false;
			++i;
		}
	}
	if (i != dotted.size())
		return false;
	return parts[0] == 127;
}

bool host_allowed(const std::string &host_header, int port)
{
	if (host_header.empty() || port <= 0)
		return false;
	std::string h = lower(host_header);
	std::string p = std::to_string(port);
	return h == "127.0.0.1:" + p || h == "localhost:" + p;
}

bool origin_allowed(const std::string &origin, int port)
{
	std::string o = lower(origin);
	std::string p = std::to_string(port);
	return o == "http://127.0.0.1:" + p || o == "http://localhost:" + p;
}

namespace {

bool starts_with_ci(const std::string &s, const char *prefix)
{
	size_t n = std::strlen(prefix);
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; ++i)
		if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
			return false;
	return true;
}

// parses a Content-Length value: digits only, no sign, no spaces, at most 10 digits
bool parse_length(const std::string &v, size_t &out)
{
	if (v.empty() || v.size() > 10)
		return false;
	size_t n = 0;
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i] < '0' || v[i] > '9')
			return false;
		n = n * 10 + static_cast<size_t>(v[i] - '0');
	}
	out = n;
	return true;
}

HttpResponse json_response(int status, const std::string &body)
{
	HttpResponse r;
	r.status = status;
	r.reason = reason_for(status);
	add_common_headers(r);
	r.headers.push_back(std::make_pair("Content-Type", "application/json; charset=utf-8"));
	r.body = body;
	return r;
}

} // namespace

bool ingest_precheck(const HttpRequest &req, int port, ChatHub &chat, HttpResponse &early)
{
	if (req.method != "POST") {
		early = make_error(405, "method not allowed");
		early.headers.push_back(std::make_pair("Allow", "POST"));
		return false;
	}
	if (!req.has_header("host") || !host_allowed(req.header("host"), port)) {
		early = make_error(403, "forbidden host");
		return false;
	}
	// The reader is a local program, not a web page: a browser always sends Origin and/or
	// Sec-Fetch-Site on a cross-site or POST request, Node's fetch sends neither (only Sec-Fetch-Mode).
	if (req.has_header("origin") || req.has_header("sec-fetch-site")) {
		early = make_error(403, "forbidden origin");
		return false;
	}
	std::string auth = req.header("authorization");
	if (!starts_with_ci(auth, "bearer ") || !chat.token_ok(auth.substr(7))) {
		early = make_error(401, "unauthorized");
		early.headers.push_back(std::make_pair("WWW-Authenticate", "Bearer"));
		return false;
	}
	if (!starts_with_ci(req.header("content-type"), "application/json")) {
		early = make_error(415, "content type must be application/json");
		return false;
	}
	if (req.has_header("transfer-encoding") || req.has_header("expect")) {
		early = make_error(400, "chunked or expect requests are not accepted");
		return false;
	}
	size_t len = 0;
	if (!req.has_header("content-length")) {
		early = make_error(411, "content-length required");
		return false;
	}
	if (!parse_length(req.header("content-length"), len)) {
		early = make_error(400, "bad content-length");
		return false;
	}
	if (len > chat.limits().max_body) {
		early = make_error(413, "body too large");
		return false;
	}
	return true;
}

HttpResponse handle_request(const HttpRequest &req, int port, const AssetMap &assets, StateHub &hub)
{
	return handle_request(req, port, assets, hub, nullptr);
}

HttpResponse handle_request(const HttpRequest &req, int port, const AssetMap &assets, StateHub &hub, ChatHub *chat)
{
	if (chat && req.path == "/chat/ingest") {
		HttpResponse early;
		if (!ingest_precheck(req, port, *chat, early))
			return early;
		size_t len = 0;
		parse_length(req.header("content-length"), len);
		if (req.body.size() != len)
			return make_error(400, "body length does not match content-length");
		IngestResult ir = chat->ingest(req.body);
		json::Value o = json::Value::object();
		o.set("ok", json::Value::boolean(ir.http_status == 200));
		if (ir.http_status != 200) {
			o.set("error", json::Value::string(ir.error));
		} else {
			o.set("accepted", json::Value::number(ir.accepted));
			o.set("duplicates", json::Value::number(ir.duplicates));
			o.set("blocked", json::Value::number(ir.blocked));
			o.set("rate_limited", json::Value::number(ir.rate_limited));
			o.set("invalid", json::Value::number(ir.invalid));
		}
		return json_response(ir.http_status, json::dump(o, false));
	}
	if (req.method != "GET" && req.method != "HEAD") {
		HttpResponse r = make_error(405, "method not allowed");
		r.headers.push_back(std::make_pair("Allow", "GET, HEAD"));
		return r;
	}
	if (!req.has_header("host") || !host_allowed(req.header("host"), port))
		return make_error(403, "forbidden host");
	if (req.has_header("origin") && !origin_allowed(req.header("origin"), port))
		return make_error(403, "forbidden origin");

	std::string path = req.path;
	if (path == "/")
		path = "/overlay.html";

	bool is_chat_data = chat && (path == "/chat/events" || path == "/chat/status.json");
	bool is_data = (path == "/state.json" || path == "/events" || is_chat_data);
	if (is_data && req.has_header("sec-fetch-site")) {
		std::string site = lower(req.header("sec-fetch-site"));
		if (site != "same-origin" && site != "none")
			return make_error(403, "forbidden fetch site");
	}

	if (path == "/state.json") {
		HttpResponse r;
		add_common_headers(r);
		r.headers.push_back(std::make_pair("Content-Type", "application/json; charset=utf-8"));
		r.body = hub.snapshot();
		return r;
	}
	if (path == "/events") {
		HttpResponse r;
		add_common_headers(r);
		r.headers.push_back(std::make_pair("Content-Type", "text/event-stream; charset=utf-8"));
		r.headers.push_back(std::make_pair("X-Accel-Buffering", "no"));
		r.sse = true;
		return r;
	}
	if (is_chat_data && path == "/chat/status.json") {
		HttpResponse r;
		add_common_headers(r);
		r.headers.push_back(std::make_pair("Content-Type", "application/json; charset=utf-8"));
		r.body = chat->status_json();
		return r;
	}
	if (is_chat_data && path == "/chat/events") {
		HttpResponse r;
		add_common_headers(r);
		r.headers.push_back(std::make_pair("Content-Type", "text/event-stream; charset=utf-8"));
		r.headers.push_back(std::make_pair("X-Accel-Buffering", "no"));
		r.sse = true;
		r.sse_kind = 1;
		std::string last = req.header("last-event-id");
		size_t n = 0;
		if (parse_length(last, n) && n > 0) {
			r.chat_resume = true;
			r.chat_after = n;
		}
		size_t q = req.query.find("backlog=");
		if (q != std::string::npos && (q == 0 || req.query[q - 1] == '&')) {
			size_t v = 0;
			size_t end = req.query.find('&', q);
			if (parse_length(req.query.substr(q + 8, end == std::string::npos ? std::string::npos : end - q - 8), v))
				r.chat_backlog = std::min<size_t>(v, 200);
		}
		return r;
	}
	AssetMap::const_iterator it = assets.find(path);
	if (it == assets.end())
		return make_error(404, "not found");
	HttpResponse r;
	add_common_headers(r);
	if (chat && path == "/chat.html") {
		// emote images may come from the platforms' own CDNs and nowhere else
		for (size_t i = 0; i < r.headers.size(); ++i) {
			if (r.headers[i].first == "Content-Security-Policy")
				r.headers[i].second = "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src " +
						      chat_img_src_csp() +
						      "; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
		}
	}
	r.headers.push_back(std::make_pair("Content-Type", it->second.content_type));
	r.body = it->second.body;
	return r;
}

std::string serialize_response(const HttpResponse &r, bool head_only)
{
	std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + r.reason + "\r\n";
	for (size_t i = 0; i < r.headers.size(); ++i)
		out += r.headers[i].first + ": " + r.headers[i].second + "\r\n";
	if (!r.sse)
		out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
	out += "Connection: close\r\n\r\n";
	if (!head_only && !r.sse)
		out += r.body;
	return out;
}

// ---------------------------------------------------------------------------------------------
// socket layer
// ---------------------------------------------------------------------------------------------

namespace {

std::mutex g_wsa_m;
int g_wsa_refs = 0;

bool wsa_acquire()
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_wsa_m);
	if (g_wsa_refs == 0) {
		WSADATA d;
		if (WSAStartup(MAKEWORD(2, 2), &d) != 0)
			return false;
	}
	++g_wsa_refs;
#endif
	return true;
}

void wsa_release()
{
#ifdef _WIN32
	std::lock_guard<std::mutex> lk(g_wsa_m);
	if (g_wsa_refs > 0 && --g_wsa_refs == 0)
		WSACleanup();
#endif
}

native_sock to_native(int64_t s)
{
	return static_cast<native_sock>(s);
}

void close_native(int64_t s)
{
	if (s == kNoSock)
		return;
#ifdef _WIN32
	closesocket(to_native(s));
#else
	::close(to_native(s));
#endif
}

void shutdown_native(int64_t s)
{
#ifdef _WIN32
	shutdown(to_native(s), SD_BOTH);
#else
	::shutdown(to_native(s), SHUT_RDWR);
#endif
}

void set_timeouts(int64_t s, int ms)
{
#ifdef _WIN32
	DWORD t = static_cast<DWORD>(ms);
	setsockopt(to_native(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&t), sizeof t);
	setsockopt(to_native(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&t), sizeof t);
#else
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(to_native(s), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(to_native(s), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

bool send_all(int64_t s, const std::string &data)
{
	size_t off = 0;
	while (off < data.size()) {
#ifdef MSG_NOSIGNAL
		int flags = MSG_NOSIGNAL;
#else
		int flags = 0;
#endif
		int n = static_cast<int>(
			send(to_native(s), data.data() + off, static_cast<int>(std::min<size_t>(data.size() - off, 65536)), flags));
		if (n <= 0)
			return false;
		off += static_cast<size_t>(n);
	}
	return true;
}

// Lets the peer finish sending (so an early error response is not lost to a TCP reset when we
// close with unread request bytes). Bounded in time and size.
void drain_peer(int64_t s)
{
#ifdef _WIN32
	shutdown(to_native(s), SD_SEND);
#else
	::shutdown(to_native(s), SHUT_WR);
#endif
	set_timeouts(s, 200);
	char junk[2048];
	size_t total = 0;
	while (total < 65536) {
		int n = static_cast<int>(recv(to_native(s), junk, sizeof junk, 0));
		if (n <= 0)
			break;
		total += static_cast<size_t>(n);
	}
}

// Binds a loopback listener on `port` (0 = any). Returns the socket or kNoSock.
int64_t bind_loopback(int port, int &actual_port)
{
	native_sock s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
	if (s == INVALID_SOCKET)
		return kNoSock;
	BOOL excl = TRUE; // nobody else may share or hijack this port
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&excl), sizeof excl);
#else
	if (s < 0)
		return kNoSock;
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1, never INADDR_ANY
	a.sin_port = htons(static_cast<unsigned short>(port));
	if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(s, 16) != 0) {
		close_native(static_cast<int64_t>(s));
		return kNoSock;
	}
	sockaddr_in got;
	socklen_t len = sizeof got;
	std::memset(&got, 0, sizeof got);
	if (getsockname(s, reinterpret_cast<sockaddr *>(&got), &len) != 0) {
		close_native(static_cast<int64_t>(s));
		return kNoSock;
	}
	actual_port = ntohs(got.sin_port);
	return static_cast<int64_t>(s);
}

} // namespace

HttpServer::HttpServer(StateHub &hub, const AssetMap &assets, const ServerOptions &opts, ChatHub *chat)
	: hub_(hub),
	  chat_(chat),
	  assets_(assets),
	  opts_(opts),
	  listen_sock_(kNoSock),
	  port_(0),
	  fell_back_(false),
	  running_(false),
	  active_conns_(0),
	  sse_clients_(0),
	  refused_peers_(0),
	  wsa_started_(false)
{
}

HttpServer::~HttpServer()
{
	stop();
}

bool HttpServer::start(std::string &err)
{
	if (running_)
		return true;
	if (!wsa_acquire()) {
		err = "network stack unavailable";
		return false;
	}
	wsa_started_ = true;
	int actual = 0;
	int64_t s = kNoSock;
	fell_back_ = false;
	if (opts_.preferred_port > 0 && opts_.preferred_port < 65536)
		s = bind_loopback(opts_.preferred_port, actual);
	if (s == kNoSock) {
		s = bind_loopback(0, actual);
		fell_back_ = opts_.preferred_port > 0;
	}
	if (s == kNoSock) {
		err = "could not open a local port on 127.0.0.1";
		wsa_release();
		wsa_started_ = false;
		return false;
	}
	listen_sock_ = s;
	port_ = actual;
	{
		sockaddr_in got;
		socklen_t gl = sizeof got;
		std::memset(&got, 0, sizeof got);
		char ipbuf[64] = {0};
		if (getsockname(to_native(s), reinterpret_cast<sockaddr *>(&got), &gl) == 0)
			inet_ntop(AF_INET, &got.sin_addr, ipbuf, sizeof ipbuf);
		bound_address_ = ipbuf;
	}
	running_ = true;
	accept_thread_ = std::thread(&HttpServer::accept_loop, this);
	return true;
}

void HttpServer::stop()
{
	if (!running_.exchange(false)) {
		if (wsa_started_) {
			wsa_release();
			wsa_started_ = false;
		}
		return;
	}
	if (accept_thread_.joinable())
		accept_thread_.join();
	close_native(listen_sock_);
	listen_sock_ = kNoSock;
	{
		std::unique_lock<std::mutex> lk(conn_m_);
		for (std::set<int64_t>::iterator it = clients_.begin(); it != clients_.end(); ++it)
			shutdown_native(*it);
		conn_cv_.wait_for(lk, std::chrono::seconds(5), [this] { return active_conns_ == 0; });
	}
	if (wsa_started_) {
		wsa_release();
		wsa_started_ = false;
	}
}

void HttpServer::accept_loop()
{
	while (running_) {
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(to_native(listen_sock_), &rf);
		struct timeval tv;
		tv.tv_sec = 0;
		tv.tv_usec = 200000;
		int n = select(static_cast<int>(listen_sock_) + 1, &rf, nullptr, nullptr, &tv);
		if (chat_)
			chat_->flush(); // releases messages whose reordering window has passed
		if (n <= 0)
			continue;
		sockaddr_in peer;
		socklen_t plen = sizeof peer;
		std::memset(&peer, 0, sizeof peer);
		native_sock c = accept(to_native(listen_sock_), reinterpret_cast<sockaddr *>(&peer), &plen);
#ifdef _WIN32
		if (c == INVALID_SOCKET)
			continue;
#else
		if (c < 0)
			continue;
#endif
		int64_t cs = static_cast<int64_t>(c);
		char ip[64] = {0};
		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
		std::string peer_ip = peer_override_.empty() ? std::string(ip) : peer_override_;
		if (!is_loopback_ipv4(peer_ip)) {
			++refused_peers_;
			close_native(cs);
			continue;
		}
		bool accepted = false;
		{
			std::lock_guard<std::mutex> lk(conn_m_);
			if (active_conns_ < opts_.max_connections) {
				++active_conns_;
				clients_.insert(cs);
				accepted = true;
			}
		}
		if (!accepted) {
			HttpResponse r = make_error(503, "too many connections");
			send_all(cs, serialize_response(r, false));
			close_native(cs);
			continue;
		}
		std::thread(&HttpServer::serve_connection, this, cs).detach();
	}
}

void HttpServer::serve_connection(int64_t sock)
{
	set_timeouts(sock, opts_.read_timeout_ms);
	std::string buf;
	HttpRequest req;
	ParseStatus st = ParseStatus::Incomplete;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts_.read_timeout_ms);
	char chunk[1024];
	while (st == ParseStatus::Incomplete && running_) {
		if (std::chrono::steady_clock::now() > deadline)
			break;
		int n = static_cast<int>(recv(to_native(sock), chunk, sizeof chunk, 0));
		if (n <= 0)
			break;
		buf.append(chunk, static_cast<size_t>(n));
		st = parse_request(buf, req);
	}
	if (st == ParseStatus::Ok) {
		HttpResponse r;
		bool have_response = false;
		if (chat_ && req.path == "/chat/ingest") {
			if (!ingest_precheck(req, port_, *chat_, r)) {
				have_response = true; // refused before reading any body
			} else {
				size_t need = 0;
				need = static_cast<size_t>(std::strtoull(req.header("content-length").c_str(), nullptr, 10));
				std::string body = buf.size() > req.header_bytes ? buf.substr(req.header_bytes) : std::string();
				while (body.size() < need && running_ && std::chrono::steady_clock::now() <= deadline) {
					int n = static_cast<int>(recv(to_native(sock), chunk, sizeof chunk, 0));
					if (n <= 0)
						break;
					body.append(chunk, static_cast<size_t>(n));
				}
				if (body.size() > need)
					body.resize(need); // pipelined extra bytes are ignored; the connection closes after one reply
				req.body = body;
			}
		}
		if (!have_response)
			r = handle_request(req, port_, assets_, hub_, chat_);
		if (r.sse) {
			if (sse_clients_.load() >= opts_.max_sse) {
				HttpResponse busy = make_error(503, "too many event streams");
				send_all(sock, serialize_response(busy, false));
			} else if (send_all(sock, serialize_response(r, false))) {
				++sse_clients_;
				if (r.sse_kind == 1)
					serve_chat_sse(sock, r);
				else
					serve_sse(sock);
				--sse_clients_;
			}
		} else {
			send_all(sock, serialize_response(r, req.method == "HEAD"));
		}
	} else if (st == ParseStatus::Malformed) {
		send_all(sock, serialize_response(make_error(400, "bad request"), false));
	} else if (st == ParseStatus::TooLarge) {
		send_all(sock, serialize_response(make_error(431, "headers too large"), false));
	}
	if (st == ParseStatus::Malformed || st == ParseStatus::TooLarge || (st == ParseStatus::Ok && !req.has_header("upgrade")))
		drain_peer(sock);
	// Incomplete here means timeout or peer gone: just close.
	shutdown_native(sock);
	close_native(sock);
	{
		std::lock_guard<std::mutex> lk(conn_m_);
		clients_.erase(sock);
		--active_conns_;
		conn_cv_.notify_all();
	}
}

void HttpServer::serve_chat_sse(int64_t sock, const HttpResponse &r)
{
	if (!chat_ || !send_all(sock, "retry: 2000\n\n"))
		return;
	std::vector<ChatMessage> first = chat_->backlog(r.chat_resume ? chat_->limits().ring : r.chat_backlog);
	uint64_t cursor = 0;
	bool sent_any = false;
	for (size_t i = 0; i < first.size(); ++i) {
		if (r.chat_resume && first[i].seq <= r.chat_after)
			continue;
		if (!send_all(sock, "id: " + std::to_string(first[i].seq) + "\ndata: " + chat_message_json(first[i]) + "\n\n"))
			return;
		sent_any = true;
	}
	if (!first.empty())
		cursor = first.back().seq;
	else
		cursor = chat_->last_seq();
	if (r.chat_resume && !sent_any && cursor < r.chat_after)
		cursor = r.chat_after;
	if (r.chat_backlog == 0 && !r.chat_resume)
		cursor = chat_->last_seq();
	int waited = 0;
	while (running_ && !chat_->is_shutdown()) {
		std::vector<ChatMessage> next;
		if (chat_->wait_for(cursor, 250, next, 100)) {
			waited = 0;
			for (size_t i = 0; i < next.size(); ++i) {
				cursor = next[i].seq;
				if (!send_all(sock, "id: " + std::to_string(next[i].seq) + "\ndata: " + chat_message_json(next[i]) + "\n\n"))
					return;
			}
		} else {
			waited += 250;
			if (waited >= opts_.heartbeat_ms) {
				waited = 0;
				if (!send_all(sock, ": hb\n\n"))
					return;
			}
		}
	}
}

void HttpServer::serve_sse(int64_t sock)
{
	if (!send_all(sock, "retry: 2000\n\n"))
		return;
	uint64_t version = 0;
	std::string json = hub_.snapshot(&version);
	if (!send_all(sock, "data: " + json + "\n\n"))
		return;
	int waited = 0;
	while (running_ && !hub_.is_shutdown()) {
		std::string next;
		uint64_t nv = 0;
		if (hub_.wait_for_change(version, 250, next, nv)) {
			version = nv;
			waited = 0;
			if (!send_all(sock, "data: " + next + "\n\n"))
				return;
		} else {
			waited += 250;
			if (waited >= opts_.heartbeat_ms) {
				waited = 0;
				if (!send_all(sock, ": hb\n\n"))
					return;
			}
		}
	}
}

} // namespace oms
