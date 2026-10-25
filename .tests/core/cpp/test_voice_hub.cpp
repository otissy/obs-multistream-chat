// L2 (pure) and L3 (real sockets) tests for the chat voice on stream: VoiceHub (bounded clip ring,
// leader choice, "also play locally"), WAV framing, and the server's voice/events + voice/clip routes.
#include "oms_test.hpp"

#include "../../../src/core/chat_targets.hpp"
#include "../../../src/core/http_server.hpp"
#include "../../../src/core/json.hpp"
#include "../../../src/core/voice_hub.hpp"

#include <chrono>
#include <cstring>
#include <functional>
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
typedef SOCKET vsock;
static const vsock kNoVSock = INVALID_SOCKET;
static void vclose(vsock s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int vsock;
static const vsock kNoVSock = -1;
static void vclose(vsock s) { ::close(s); }
#endif

using namespace oms;

namespace {

const std::string kVKey = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

std::string pcm_of(size_t samples, int16_t value = 1000)
{
	std::string s;
	for (size_t i = 0; i < samples; ++i) {
		s.push_back(static_cast<char>(value & 0xff));
		s.push_back(static_cast<char>((value >> 8) & 0xff));
	}
	return s;
}

uint32_t u32(const std::string &s, size_t at)
{
	uint32_t v = 0;
	for (int i = 3; i >= 0; --i)
		v = (v << 8) | static_cast<unsigned char>(s[at + static_cast<size_t>(i)]);
	return v;
}

uint16_t u16(const std::string &s, size_t at)
{
	return static_cast<uint16_t>(static_cast<unsigned char>(s[at]) | (static_cast<unsigned char>(s[at + 1]) << 8));
}

HttpRequest vreq(const std::string &target, const std::string &host = "127.0.0.1:5000")
{
	HttpRequest r;
	r.method = "GET";
	r.target = target;
	size_t q = target.find('?');
	r.path = q == std::string::npos ? target : target.substr(0, q);
	r.query = q == std::string::npos ? std::string() : target.substr(q + 1);
	r.version = "HTTP/1.1";
	r.headers["host"] = host;
	return r;
}

std::string hdr(const HttpResponse &r, const std::string &name)
{
	for (const auto &h : r.headers)
		if (h.first == name)
			return h.second;
	return std::string();
}

vsock vconnect(int port)
{
#ifdef _WIN32
	static bool done = false;
	if (!done) {
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
		done = true;
	}
#endif
	vsock s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == kNoVSock)
		return kNoVSock;
	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(static_cast<unsigned short>(port));
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
		vclose(s);
		return kNoVSock;
	}
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

// Reads until `until` appears after `from` bytes of `buf`, the peer closes, or the 3 s timeout hits.
bool vrecv_until(vsock s, std::string &buf, const std::string &until, size_t from = 0)
{
	char tmp[4096];
	// 4 s in total: heartbeats keep the socket busy, so a per-recv timeout alone could wait forever
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
	while (buf.find(until, from) == std::string::npos) {
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		int n = static_cast<int>(recv(s, tmp, sizeof tmp, 0));
		if (n <= 0)
			return false;
		buf.append(tmp, static_cast<size_t>(n));
	}
	return true;
}

std::string vget(int port, const std::string &path)
{
	vsock s = vconnect(port);
	if (s == kNoVSock)
		return "<connect failed>";
	std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n\r\n";
	send(s, req.data(), static_cast<int>(req.size()), 0);
	std::string out;
	vrecv_until(s, out, "\x01\x02never");
	vclose(s);
	return out;
}

vsock open_voice_stream(int port, const std::string &query)
{
	vsock s = vconnect(port);
	if (s == kNoVSock)
		return s;
	std::string req = "GET /k/" + kVKey + "/voice/events?" + query + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
	send(s, req.data(), static_cast<int>(req.size()), 0);
	return s;
}

bool wait_until(const std::function<bool()> &ok, int ms)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		if (ok())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return ok();
}

} // namespace

// ---- L2: leader choice ---------------------------------------------------------------------

TEST(L2, voice_leader_prefers_on_program_then_obs_then_newest)
{
	CHECK_EQ(choose_leader({}), uint64_t(0));
	VoiceListener browser_new{9, false, true, false};
	VoiceListener obs_old{3, true, true, false};
	VoiceListener obs_newer{5, true, true, false};
	VoiceListener obs_hidden_newest{11, true, false, false};
	CHECK_EQ(choose_leader({obs_old, browser_new}), uint64_t(3));          // OBS beats an ordinary browser
	CHECK_EQ(choose_leader({obs_old, obs_newer}), uint64_t(5));            // newest among equals (reload takes over)
	CHECK_EQ(choose_leader({obs_newer, obs_old}), uint64_t(5));            // order of the list does not matter
	CHECK_EQ(choose_leader({obs_old, obs_hidden_newest}), uint64_t(3));    // shown on program beats hidden
	CHECK_EQ(choose_leader({obs_hidden_newest}), uint64_t(11));            // a hidden one still leads when alone
	VoiceListener browser_hidden{12, false, false, false};
	CHECK_EQ(choose_leader({browser_hidden, browser_new}), uint64_t(9));
}

// ---- L2: WAV framing and clip formats ------------------------------------------------------

TEST(L2, voice_wav_header_matches_each_rate_and_length)
{
	for (uint32_t rate : {24000u, 22050u, 16000u, 48000u}) {
		std::string pcm = pcm_of(rate / 10); // 100 ms
		std::string w = make_wav(pcm, rate);
		CHECK_EQ(w.size(), size_t(44) + pcm.size());
		CHECK_EQ(w.substr(0, 4), std::string("RIFF"));
		CHECK_EQ(u32(w, 4), uint32_t(36 + pcm.size()));
		CHECK_EQ(w.substr(8, 8), std::string("WAVEfmt "));
		CHECK_EQ(u16(w, 20), uint16_t(1)); // PCM
		CHECK_EQ(u16(w, 22), uint16_t(1)); // mono
		CHECK_EQ(u32(w, 24), rate);
		CHECK_EQ(u32(w, 28), rate * 2);
		CHECK_EQ(u16(w, 34), uint16_t(16));
		CHECK_EQ(w.substr(36, 4), std::string("data"));
		CHECK_EQ(u32(w, 40), uint32_t(pcm.size()));
		CHECK(w.compare(44, std::string::npos, pcm) == 0);
	}
	std::string odd = make_wav(std::string("\x01\x02\x03", 3), 24000);
	CHECK_EQ(u32(odd, 40), uint32_t(2)); // a dangling half sample is dropped
	CHECK_EQ(odd.size(), size_t(46));
	CHECK_EQ(make_wav("", 24000).size(), size_t(44));
}

TEST(L2, voice_push_refuses_empty_bad_rate_and_oversized_clips)
{
	VoiceHub::Limits l;
	l.max_clip_bytes = 1000;
	VoiceHub h(l);
	CHECK_EQ(h.push("a", "twitch", 24000, ""), uint64_t(0));                    // zero-length frame
	CHECK_EQ(h.push("a", "twitch", 24000, std::string(1, '\x01')), uint64_t(0)); // half a sample
	CHECK_EQ(h.push("a", "twitch", 7999, pcm_of(10)), uint64_t(0));
	CHECK_EQ(h.push("a", "twitch", 192001, pcm_of(10)), uint64_t(0));
	CHECK_EQ(h.push("a", "twitch", 0, pcm_of(10)), uint64_t(0));
	CHECK_EQ(h.push("a", "twitch", 24000, pcm_of(501)), uint64_t(0)); // 1002 bytes > 1000
	CHECK_EQ(h.clip_count(), size_t(0));
	uint64_t a = h.push("a", "twitch", 24000, pcm_of(10));
	uint64_t b = h.push("b", "kick", 16000, pcm_of(10)); // a rate change between lines is fine
	uint64_t c = h.push("c", "youtube", 22050, pcm_of(500));
	CHECK(a > 0 && b == a + 1 && c == b + 1);
	std::string w;
	CHECK(h.clip_wav(b, w));
	CHECK_EQ(u32(w, 24), 16000u);
	CHECK(h.clip_wav(c, w));
	CHECK_EQ(u32(w, 24), 22050u);
	CHECK_FALSE(h.clip_wav(999, w));
	CHECK_FALSE(h.clip_wav(0, w));
}

TEST(L2, voice_flood_stays_inside_count_and_byte_limits)
{
	VoiceHub::Limits l;
	l.max_clips = 8;
	l.max_bytes = 10000;
	VoiceHub h(l);
	uint64_t last = 0;
	for (int i = 0; i < 1000; ++i) {
		last = h.push("spam", "twitch", 24000, pcm_of(1000)); // 2000 bytes each
		CHECK(h.clip_count() <= 8);
		CHECK(h.bytes() <= 10000);
	}
	CHECK_EQ(h.clip_count(), size_t(5)); // 5 x 2000 = 10000
	CHECK_EQ(h.last_id(), last);
	std::string w;
	CHECK(h.clip_wav(last, w));
	CHECK_FALSE(h.clip_wav(last - 5, w)); // trimmed: the page gets a 404 and skips it
	std::vector<VoiceClip> s = h.since(0);
	CHECK_EQ(s.size(), size_t(5));
	CHECK_EQ(s.front().id, last - 4);
	for (const VoiceClip &c : s) {
		CHECK(c.pcm16le.empty()); // metadata only
		CHECK_EQ(c.samples, size_t(1000));
	}
	// count limit alone
	VoiceHub::Limits l2;
	l2.max_clips = 3;
	VoiceHub h2(l2);
	for (int i = 0; i < 10; ++i)
		h2.push("x", "kick", 24000, pcm_of(2));
	CHECK_EQ(h2.clip_count(), size_t(3));
	CHECK_EQ(h2.since(0).front().id, uint64_t(8));
}

TEST(L2, voice_clip_json_is_one_line_and_escapes_hostile_names)
{
	VoiceClip c;
	c.id = 42;
	c.author = "Ev\"il\n</script>\xe2\x80\xa8 \xc3\xa9";
	c.platform = "twitch";
	c.rate = 24000;
	c.samples = 36000;
	std::string j = voice_clip_json(c);
	CHECK(j.find('\n') == std::string::npos);
	json::ParseResult p = json::parse(j);
	CHECK(p.ok);
	CHECK_EQ(p.value.find("id")->as_int(), 42);
	CHECK_EQ(p.value.find("author")->as_string(), c.author);
	CHECK_EQ(p.value.find("platform")->as_string(), std::string("twitch"));
	CHECK_EQ(p.value.find("rate")->as_int(), 24000);
	CHECK_EQ(p.value.find("ms")->as_int(), 1500);
}

// ---- L2: listeners, local playback, flush, waiting -----------------------------------------

TEST(L2, voice_local_playback_follows_the_leader_and_falls_back_with_no_page)
{
	VoiceHub h;
	CHECK(h.local_wanted()); // no overlay page: the plugin keeps playing locally, as before
	uint64_t a = h.add_listener(true, true, false);
	CHECK_EQ(h.leader(), a);
	CHECK_FALSE(h.local_wanted()); // default of the page: local copy off
	uint64_t b = h.add_listener(true, true, true);
	CHECK_EQ(h.leader(), b);
	CHECK(h.local_wanted());
	uint64_t c = h.add_listener(false, true, false); // a browser preview does not take over
	CHECK_EQ(h.leader(), b);
	h.remove_listener(b);
	CHECK_EQ(h.leader(), a);
	CHECK_FALSE(h.local_wanted());
	h.remove_listener(a);
	CHECK_EQ(h.leader(), c);
	h.remove_listener(c);
	h.remove_listener(c); // twice is harmless
	CHECK_EQ(h.listener_count(), size_t(0));
	CHECK(h.local_wanted());
}

TEST(L2, voice_flush_and_wait_wake_on_change_and_time_out_when_idle)
{
	VoiceHub h;
	uint64_t v = h.version();
	auto t0 = std::chrono::steady_clock::now();
	CHECK_FALSE(h.wait_for_change(v, 120)); // underrun: nothing to say, the wait just times out
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	CHECK(ms >= 100 && ms < 2000);
	std::thread t([&h] {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		h.push("a", "x", 24000, pcm_of(4));
	});
	CHECK(h.wait_for_change(v, 2000));
	t.join();
	uint64_t g = h.flush_generation();
	v = h.version();
	h.flush();
	CHECK_EQ(h.flush_generation(), g + 1);
	CHECK(h.version() != v);
	CHECK_EQ(h.clip_count(), size_t(0));
	CHECK_EQ(h.bytes(), size_t(0));
	uint64_t next = h.push("b", "x", 24000, pcm_of(4));
	CHECK(next > 1); // ids keep counting after a flush, so a page never replays an old id
	h.shutdown();
	CHECK(h.is_shutdown());
	CHECK_EQ(h.push("c", "x", 24000, pcm_of(4)), uint64_t(0));
	CHECK_FALSE(h.wait_for_change(h.version(), 50));
}

// ---- L2: the server routes -----------------------------------------------------------------

TEST(L2, voice_routes_need_the_key_and_serve_clips_as_wav)
{
	StateHub hub;
	SystemClock clock;
	ChatHub chat(clock);
	AssetMap a;
	Asset html;
	html.content_type = "text/html; charset=utf-8";
	html.body = "<!doctype html><title>chat</title>";
	a["/chat.html"] = html;
	VoiceHub voice;
	uint64_t id = voice.push("viewer", "twitch", 24000, pcm_of(2400));
	const std::string k = "/k/" + kVKey;
	const std::string clip = "/voice/clip/" + std::to_string(id) + ".wav";
	for (const std::string &p : {std::string("/voice/events"), clip}) {
		HttpResponse r = handle_request(vreq(p), 5000, a, hub, &chat, kVKey, &voice);
		CHECK_EQ(r.status, 401);
		CHECK(r.body.find("RIFF") == std::string::npos);
		CHECK(r.body.find(kVKey) == std::string::npos);
		std::string wrong = kVKey;
		wrong[5] = '0';
		CHECK_EQ(handle_request(vreq("/k/" + wrong + p), 5000, a, hub, &chat, kVKey, &voice).status, 401);
	}
	HttpResponse ev = handle_request(vreq(k + "/voice/events?obs=1&active=0&local=1"), 5000, a, hub, &chat, kVKey, &voice);
	CHECK_EQ(ev.status, 200);
	CHECK(ev.sse);
	CHECK_EQ(ev.sse_kind, 2);
	CHECK(ev.voice_obs);
	CHECK_FALSE(ev.voice_active);
	CHECK(ev.voice_local);
	CHECK_FALSE(ev.voice_resume);
	HttpResponse def = handle_request(vreq(k + "/voice/events"), 5000, a, hub, &chat, kVKey, &voice);
	CHECK_FALSE(def.voice_obs);
	CHECK(def.voice_active);
	CHECK_FALSE(def.voice_local);
	HttpRequest resume = vreq(k + "/voice/events?obs=yes&local=2");
	resume.headers["last-event-id"] = "7";
	HttpResponse rr = handle_request(resume, 5000, a, hub, &chat, kVKey, &voice);
	CHECK(rr.voice_resume);
	CHECK_EQ(rr.voice_after, uint64_t(7));
	CHECK_FALSE(rr.voice_obs);   // only "1" counts
	CHECK_FALSE(rr.voice_local);

	HttpResponse w = handle_request(vreq(k + clip), 5000, a, hub, &chat, kVKey, &voice);
	CHECK_EQ(w.status, 200);
	CHECK_EQ(hdr(w, "Content-Type"), std::string("audio/wav"));
	CHECK_EQ(hdr(w, "Cache-Control"), std::string("no-store"));
	std::string expect;
	CHECK(voice.clip_wav(id, expect));
	CHECK(w.body == expect);
	CHECK_EQ(w.body.size(), size_t(44 + 4800));
	HttpRequest head = vreq(k + clip);
	head.method = "HEAD";
	CHECK_EQ(handle_request(head, 5000, a, hub, &chat, kVKey, &voice).status, 200);

	for (const char *bad : {"/voice/clip/0.wav", "/voice/clip/abc.wav", "/voice/clip/.wav", "/voice/clip/1.wav.wav",
				"/voice/clip/../1.wav", "/voice/clip/-1.wav", "/voice/clip/99999.wav", "/voice/clip/1", "/voice/clip/1234567890123456.wav",
				"/voice/events/x"})
		CHECK_EQ(handle_request(vreq(k + bad), 5000, a, hub, &chat, kVKey, &voice).status, 404);
	// no voice hub (an older plugin wiring): the routes do not exist
	CHECK_EQ(handle_request(vreq(k + clip), 5000, a, hub, &chat, kVKey, nullptr).status, 404);
	CHECK_EQ(handle_request(vreq(k + "/voice/events"), 5000, a, hub, &chat, kVKey).status, 404);
	// the other defences still apply
	CHECK_EQ(handle_request(vreq(k + clip, "evil.example:5000"), 5000, a, hub, &chat, kVKey, &voice).status, 403);
	HttpRequest x = vreq(k + clip);
	x.headers["sec-fetch-site"] = "cross-site";
	CHECK_EQ(handle_request(x, 5000, a, hub, &chat, kVKey, &voice).status, 403);
	HttpRequest so = vreq(k + clip);
	so.headers["sec-fetch-site"] = "same-origin";
	so.headers["range"] = "bytes=0-"; // what a media element sends: a full 200 is a valid answer
	CHECK_EQ(handle_request(so, 5000, a, hub, &chat, kVKey, &voice).status, 200);
	HttpRequest o = vreq(k + "/voice/events");
	o.headers["origin"] = "http://evil.example";
	CHECK_EQ(handle_request(o, 5000, a, hub, &chat, kVKey, &voice).status, 403);
	// a trimmed clip is a plain 404
	voice.flush();
	CHECK_EQ(handle_request(vreq(k + clip), 5000, a, hub, &chat, kVKey, &voice).status, 404);
}

TEST(L2, voice_chat_page_csp_allows_own_media_and_keeps_image_rules)
{
	StateHub hub;
	SystemClock clock;
	ChatHub chat(clock);
	AssetMap a;
	Asset html;
	html.content_type = "text/html; charset=utf-8";
	html.body = "<!doctype html>";
	a["/chat.html"] = html;
	VoiceHub voice;
	HttpResponse r = handle_request(vreq("/k/" + kVKey + "/chat.html"), 5000, a, hub, &chat, kVKey, &voice);
	std::string csp = hdr(r, "Content-Security-Policy");
	CHECK(csp.find("media-src 'self';") != std::string::npos);
	CHECK(csp.find("img-src " + chat_img_src_csp() + ";") != std::string::npos); // emotes, badges, logos: unchanged
	CHECK(csp.find("default-src 'none'") != std::string::npos);
	CHECK(csp.find("connect-src 'self'") != std::string::npos);
	CHECK(csp.find("blob:") == std::string::npos);
}

// ---- L3: over the wire ---------------------------------------------------------------------

TEST(L3, voice_stream_hands_over_the_leader_and_delivers_clips)
{
	StateHub hub;
	AssetMap a;
	VoiceHub voice;
	ServerOptions o;
	o.access_key = kVKey;
	HttpServer srv(hub, a, o);
	srv.set_voice_hub(&voice);
	std::string err;
	CHECK(srv.start(err));
	int port = srv.port();

	vsock first = open_voice_stream(port, "obs=1&active=1&local=0");
	CHECK(first != kNoVSock);
	std::string b1;
	CHECK(vrecv_until(first, b1, "\"role\":\"leader\""));
	CHECK(b1.find("text/event-stream") != std::string::npos);
	CHECK(wait_until([&] { return voice.listener_count() == 1; }, 2000));
	CHECK_FALSE(voice.local_wanted());

	vsock second = open_voice_stream(port, "obs=1&active=1&local=1"); // the same URL in a second browser source
	std::string b2;
	CHECK(vrecv_until(second, b2, "\"role\":\"leader\""));
	size_t mark = b1.size();
	CHECK(vrecv_until(first, b1, "\"role\":\"standby\"", mark)); // only one speaks
	CHECK(voice.local_wanted());

	uint64_t id = voice.push("viewer", "kick", 24000, pcm_of(240));
	std::string want = "id: " + std::to_string(id) + "\nevent: clip\ndata: {";
	mark = b1.size();
	CHECK(vrecv_until(first, b1, want, mark)); // standby pages also hear about clips (they just stay quiet)
	CHECK(vrecv_until(second, b2, want));
	CHECK(b2.find("\"author\":\"viewer\"") != std::string::npos);
	std::string wav = vget(port, "/k/" + kVKey + "/voice/clip/" + std::to_string(id) + ".wav");
	CHECK(wav.find("HTTP/1.1 200 OK") == 0);
	CHECK(wav.find("Content-Type: audio/wav") != std::string::npos);
	CHECK(wav.find("\r\n\r\nRIFF") != std::string::npos);
	CHECK(vget(port, "/voice/clip/" + std::to_string(id) + ".wav").find("HTTP/1.1 401") == 0);

	mark = b1.size();
	voice.flush();
	CHECK(vrecv_until(first, b1, "event: flush", mark));

	// the leader's page goes away (reload, source removed): the other one takes over by itself
	vclose(second);
	mark = b1.size();
	CHECK(vrecv_until(first, b1, "\"role\":\"leader\"", mark));
	CHECK(wait_until([&] { return voice.listener_count() == 1; }, 2000));
	CHECK_FALSE(voice.local_wanted());

	// a fresh page does not replay what was said before it opened
	uint64_t old = voice.push("before", "kick", 24000, pcm_of(240));
	vsock third = open_voice_stream(port, "obs=0");
	std::string b3;
	CHECK(vrecv_until(third, b3, "\"role\":\"standby\""));
	CHECK(b3.find("id: " + std::to_string(old)) == std::string::npos);

	auto t0 = std::chrono::steady_clock::now();
	srv.stop();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	CHECK(ms < 3000);
	vclose(first);
	vclose(third);
	CHECK(wait_until([&] { return voice.listener_count() == 0; }, 2000));
}

TEST(L3, voice_stream_reconnect_with_last_event_id_gets_missed_clips)
{
	StateHub hub;
	AssetMap a;
	VoiceHub voice;
	ServerOptions o;
	o.access_key = kVKey;
	HttpServer srv(hub, a, o);
	srv.set_voice_hub(&voice);
	std::string err;
	CHECK(srv.start(err));
	uint64_t a1 = voice.push("a", "twitch", 24000, pcm_of(10));
	uint64_t a2 = voice.push("b", "twitch", 24000, pcm_of(10));
	vsock s = vconnect(srv.port());
	std::string req = "GET /k/" + kVKey + "/voice/events?obs=1 HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) +
			  "\r\nLast-Event-ID: " + std::to_string(a1) + "\r\n\r\n";
	send(s, req.data(), static_cast<int>(req.size()), 0);
	std::string b;
	CHECK(vrecv_until(s, b, "id: " + std::to_string(a2) + "\n"));
	CHECK(b.find("id: " + std::to_string(a1) + "\n") == std::string::npos);
	vclose(s);
	srv.stop();
}

// ---- L2: routing in the plugin and the source check ----------------------------------------

TEST(L2, voice_route_sends_previews_local_only_and_lines_to_the_page)
{
	VoiceRoute r = route_voice_clip(true, nullptr);
	CHECK(r.local && !r.page);
	VoiceHub h;
	r = route_voice_clip(true, &h); // the dock's Preview never reaches the stream
	CHECK(r.local && !r.page);
	r = route_voice_clip(false, nullptr); // no hub wired: exactly the old behaviour
	CHECK(r.local && !r.page);
	r = route_voice_clip(false, &h); // nobody listening yet: page gets it and the desktop still hears it
	CHECK(r.page && r.local);
	uint64_t a = h.add_listener(true, true, false);
	r = route_voice_clip(false, &h); // a speaking overlay with the local copy off (the default)
	CHECK(r.page && !r.local);
	h.remove_listener(a);
	h.add_listener(true, true, true);
	r = route_voice_clip(false, &h);
	CHECK(r.page && r.local);
}

TEST(L2, voice_speaking_chat_url_matches_only_this_install_with_voice_on)
{
	const std::string base = "http://127.0.0.1:51734/k/" + kVKey + "/chat.html";
	CHECK(is_speaking_chat_url(base + "?voice=1", kVKey));
	CHECK(is_speaking_chat_url(base + "?theme=dark&voice=1&max=20", kVKey));
	CHECK(is_speaking_chat_url("http://localhost:51734/k/" + kVKey + "/chat.html?voice=1#x", kVKey));
	CHECK_FALSE(is_speaking_chat_url(base, kVKey));                 // existing URLs: voice off
	CHECK_FALSE(is_speaking_chat_url(base + "?voice=0", kVKey));
	CHECK_FALSE(is_speaking_chat_url(base + "?novoice=1", kVKey));
	CHECK_FALSE(is_speaking_chat_url(base + "?voice=10", kVKey));
	CHECK_FALSE(is_speaking_chat_url(base + "#voice=1", kVKey));
	CHECK_FALSE(is_speaking_chat_url("http://127.0.0.1:51734/k/" + kVKey + "/overlay.html?voice=1", kVKey));
	CHECK_FALSE(is_speaking_chat_url("http://evil.example/k/" + kVKey + "/chat.html?voice=1", kVKey));
	CHECK_FALSE(is_speaking_chat_url("http://127.0.0.1:51734/x/k/" + kVKey + "/chat.html?voice=1", kVKey));
	std::string other = kVKey;
	other[0] = '0';
	CHECK_FALSE(is_speaking_chat_url(base + "?voice=1", other));
	CHECK_FALSE(is_speaking_chat_url(base + "?voice=1", ""));
}
