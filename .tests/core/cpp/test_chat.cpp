// Chat merge: sanitising, targets, emote allow-list, hub (dedupe, order, blocklist, rate limits),
// config schema 3, and the server routes (auth, rebinding, SSE) over real loopback sockets.
#include "oms_test.hpp"

#include "../../../src/core/chat_hub.hpp"
#include "../../../src/core/chat_targets.hpp"
#include "../../../src/core/config.hpp"
#include "../../../src/core/http_server.hpp"
#include "../../../src/core/json.hpp"
#include "../../../src/core/random.hpp"

#include <chrono>
#include <cstdlib>
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
typedef SOCKET csock;
static const csock kBadCSock = INVALID_SOCKET;
static void cclose(csock s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int csock;
static const csock kBadCSock = -1;
static void cclose(csock s) { ::close(s); }
#endif

using namespace oms;

namespace {

std::string S(const char *a) { return std::string(a); }

// Builds {"v":1,"sent":..,"messages":[...]} from message objects given as JSON text.
std::string batch(const std::string &messages_json, const std::string &extra = std::string())
{
	return "{\"v\":1" + extra + ",\"messages\":[" + messages_json + "]}";
}

std::string msg(const std::string &id, const std::string &author, const std::string &text, const std::string &platform = "twitch",
		const std::string &source = "c1", const std::string &extra = std::string())
{
	return "{\"platform\":\"" + platform + "\",\"source\":\"" + source + "\",\"id\":\"" + json::escape_string(id) +
	       "\",\"author\":\"" + json::escape_string(author) + "\",\"parts\":[{\"t\":\"text\",\"v\":\"" +
	       json::escape_string(text) + "\"}]" + extra + "}";
}

std::string plain_text(const ChatMessage &m)
{
	std::string o;
	for (size_t i = 0; i < m.parts.size(); ++i)
		o += m.parts[i].emote ? "[" + m.parts[i].text + "]" : m.parts[i].text;
	return o;
}

std::vector<ChatMessage> drain(ChatHub &h, MockClock &c, int advance_ms = 1000)
{
	c.advance(advance_ms);
	h.flush();
	return h.backlog(1000);
}

} // namespace

// ---- L2: sanitising -------------------------------------------------------------------------

TEST(L2, chat_text_strips_html_tags_but_keeps_hearts_and_comparisons)
{
	CHECK_EQ(sanitize_chat_text("<script>alert(1)</script>hello", 100), S("alert(1)hello"));
	CHECK_EQ(sanitize_chat_text("I <3 this", 100), S("I <3 this"));
	CHECK_EQ(sanitize_chat_text("2 < 3 and 5 > 4", 100), S("2 < 3 and 5 > 4"));
	CHECK_EQ(sanitize_chat_text("<img src=x onerror=alert(1)>pic", 100), S("pic"));
	CHECK_EQ(sanitize_chat_text("before <img src=x onerror=alert(1)", 100), S("before"));      // unterminated: dropped to the end
	CHECK_EQ(sanitize_chat_text("<<script>script>x", 100).find("<script"), std::string::npos);  // nesting trick
	CHECK_EQ(sanitize_chat_text("<scr<b>ipt>x", 100).find("<scr"), std::string::npos);          // reassembly trick
	CHECK_EQ(sanitize_chat_text("<!-- c --> ok", 100), S("ok"));
	CHECK_EQ(sanitize_chat_text("<?php echo 1 ?>x", 100), S("x"));
	CHECK_EQ(sanitize_chat_text("</div>x", 100), S("x"));
}

TEST(L2, chat_text_leaves_javascript_urls_inert_as_text)
{
	// The overlay only ever uses textContent, so a link is just characters. The hub does not
	// rewrite them, and it must not turn them into anything else either.
	CHECK_EQ(sanitize_chat_text("javascript:alert(1)", 100), S("javascript:alert(1)"));
	CHECK_EQ(sanitize_chat_text("&lt;script&gt;", 100), S("&lt;script&gt;")); // entities stay literal text
}

TEST(L2, chat_text_removes_bidi_overrides_zero_width_and_tag_characters)
{
	CHECK_EQ(sanitize_chat_text(S("a\xE2\x80\xAE" "b\xE2\x80\xAC"), 100), S("ab"));                 // U+202E, U+202C
	CHECK_EQ(sanitize_chat_text(S("a\xE2\x81\xA6" "b\xE2\x81\xA9"), 100), S("ab"));                 // isolates U+2066, U+2069
	CHECK_EQ(sanitize_chat_text(S("s\xE2\x80\x8B" "pam"), 100), S("spam"));                         // zero width space
	CHECK_EQ(sanitize_chat_text(S("a\xE2\x80\x8E" "b\xE2\x80\x8F"), 100), S("ab"));                 // LRM RLM
	CHECK_EQ(sanitize_chat_text(S("a\xEF\xBB\xBF" "b"), 100), S("ab"));                             // BOM
	CHECK_EQ(sanitize_chat_text(S("a\xF3\xA0\x80\x81" "b"), 100), S("ab"));                         // U+E0001 tag character
	CHECK_EQ(sanitize_chat_text(S("a\xC2\xAD" "b"), 100), S("ab"));                                  // soft hyphen
	CHECK_EQ(sanitize_chat_text(S("a\x01\x02\x7F" "b"), 100), S("ab"));                              // controls
	CHECK_EQ(sanitize_chat_text(S("a\x1B[31mred"), 100), S("a[31mred"));                             // ESC removed
	CHECK_EQ(sanitize_chat_text(S("a\xC2\x85" "b"), 100), S("a b"));                                 // NEL is a space
}

TEST(L2, chat_text_keeps_emoji_sequences_and_persian_joiners)
{
	std::string family = S("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"); // man ZWJ woman ZWJ girl
	CHECK_EQ(sanitize_chat_text(family, 100), family);
	std::string heart = S("\xE2\x9D\xA4\xEF\xB8\x8F"); // heart + VS16
	CHECK_EQ(sanitize_chat_text(heart, 100), heart);
	std::string persian = S("\xD9\x85\xDB\x8C\xE2\x80\x8C\xD8\xAE\xD9\x88\xD8\xA7\xD9\x87\xD9\x85"); // contains ZWNJ
	CHECK_EQ(sanitize_chat_text(persian, 100), persian);
}

TEST(L2, chat_text_replaces_invalid_utf8_and_overlongs)
{
	CHECK_EQ(sanitize_chat_text(S("a\xFF" "b"), 100), S("a\xEF\xBF\xBD" "b"));
	CHECK_EQ(sanitize_chat_text(S("a\xC0\xAF" "b"), 100), S("a\xEF\xBF\xBD\xEF\xBF\xBD" "b"));        // overlong "/"
	CHECK_EQ(sanitize_chat_text(S("a\xED\xA0\x80" "b"), 100), S("a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "b")); // surrogate half
	CHECK_EQ(sanitize_chat_text(S("a\xF4\x90\x80\x80"), 100), S("a\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD")); // > U+10FFFF
	CHECK_EQ(sanitize_chat_text(S("a\xE2\x82"), 100), S("a\xEF\xBF\xBD\xEF\xBF\xBD"));                  // truncated sequence
	CHECK_EQ(sanitize_chat_text(S("\xE2"), 100), S("\xEF\xBF\xBD"));
}

TEST(L2, chat_text_caps_combining_marks_whitespace_and_length)
{
	std::string zalgo = "a";
	for (int i = 0; i < 200; ++i)
		zalgo += S("\xCC\x8D"); // U+030D x200
	std::string out = sanitize_chat_text(zalgo, 1000);
	CHECK(out.size() <= 1 + 3 * 2); // base + at most three 2-byte marks
	CHECK_EQ(sanitize_chat_text("  a \t\n  b   c  ", 100), S("a b c"));
	CHECK_EQ(sanitize_chat_text("abcdefghij", 5), S("abcd\xE2\x80\xA6"));
	CHECK_EQ(sanitize_chat_text(S("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"), 3), S("\xC3\xA9\xC3\xA9\xE2\x80\xA6")); // counted in code points
	CHECK_EQ(sanitize_chat_text("", 10), S(""));
	CHECK_EQ(sanitize_chat_text("   ", 10), S(""));
}

TEST(L2, chat_text_huge_input_is_bounded)
{
	std::string huge(5 * 1024 * 1024, 'x');
	std::string out = sanitize_chat_text(huge, 500);
	CHECK(out.size() <= 504);
	std::string tags;
	for (int i = 0; i < 20000; ++i)
		tags += "<a<b<c";
	out = sanitize_chat_text(tags, 500);
	CHECK(out.size() <= 504);
}

TEST(L2, chat_color_only_accepts_hex_triplets)
{
	CHECK(valid_chat_color("#ff00AA"));
	CHECK_FALSE(valid_chat_color("red"));
	CHECK_FALSE(valid_chat_color("#fff"));
	CHECK_FALSE(valid_chat_color("#ff00AAx"));
	CHECK_FALSE(valid_chat_color("url(javascript:1)"));
	CHECK_FALSE(valid_chat_color("#ff00;AA"));
	CHECK_FALSE(valid_chat_color(""));
}

// ---- L2: targets and emote allow-list -------------------------------------------------------

static std::string vectors_path()
{
	const char *env = std::getenv("OMS_VECTORS");
	if (env && *env)
		return env;
	std::string f = __FILE__;
	for (size_t i = 0; i < f.size(); ++i)
		if (f[i] == '\\')
			f[i] = '/';
	size_t slash = f.rfind('/');
	std::string dir = slash == std::string::npos ? "." : f.substr(0, slash);
	return dir + "/../../chat/vectors/targets.json";
}

TEST(L2, chat_targets_match_the_shared_vectors)
{
	std::string text;
	CHECK(read_file(vectors_path(), text));
	json::ParseResult pr = json::parse(text);
	CHECK(pr.ok);
	CHECK(pr.value.is_array());
	CHECK(pr.value.size() >= 40);
	for (size_t i = 0; i < pr.value.size(); ++i) {
		const json::Value &v = pr.value.at(i);
		std::string platform = v.find("platform")->as_string();
		std::string input = v.find("input")->as_string();
		bool want_ok = v.find("ok")->as_bool();
		std::string target, err;
		bool ok = normalize_chat_target(platform, input, target, err);
		if (ok != want_ok) {
			oms_test::fail(__FILE__, __LINE__, "vector " + std::to_string(i) + " [" + platform + "] " + input + " expected ok=" +
									     (want_ok ? "true" : "false"));
			continue;
		}
		if (ok) {
			CHECK_EQ(target, v.find("target")->as_string());
			CHECK_EQ(chat_page_url(platform, target), v.find("url")->as_string());
		} else {
			CHECK_FALSE(err.empty());
		}
	}
}

TEST(L2, chat_page_url_refuses_unnormalized_targets)
{
	CHECK_EQ(chat_page_url("twitch", "Shroud"), S("")); // must be the normalized form
	CHECK_EQ(chat_page_url("twitch", "https://evil.example/"), S(""));
	CHECK_EQ(chat_page_url("nope", "shroud"), S(""));
	CHECK(find_chat_platform("x")->experimental);
	CHECK(find_chat_platform("tiktok")->experimental);
	CHECK_FALSE(find_chat_platform("twitch")->experimental);
}

TEST(L2, emote_urls_must_be_https_on_the_platforms_own_cdn)
{
	CHECK(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0"));
	CHECK(emote_url_allowed("twitch", "https://STATIC-CDN.JTVNW.NET/emoticons/v2/25/default/dark/1.0"));
	CHECK(emote_url_allowed("kick", "https://files.kick.com/emotes/1234/fullsize"));
	CHECK(emote_url_allowed("youtube", "https://yt3.ggpht.com/abc=w24-h24-c-k-nd"));
	CHECK(emote_url_allowed("youtube", "https://www.youtube.com/s/gaming/emoji/7ff574f2/emoji_u1f60d.svg"));
	CHECK(emote_url_allowed("x", "https://abs-0.twimg.com/emoji/v2/svg/1f600.svg"));
	CHECK(emote_url_allowed("tiktok", "https://p16-webcast.tiktokcdn.com/img/x.webp"));
	// wrong platform, wrong host
	CHECK_FALSE(emote_url_allowed("kick", "https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://evil.example/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net.evil.example/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://evil.example/static-cdn.jtvnw.net/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net@evil.example/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://evil.example@static-cdn.jtvnw.net/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net:8443/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net./a.png"));
	CHECK_FALSE(emote_url_allowed("x", "https://twimg.com/a.png"));        // the bare suffix is not a subdomain
	CHECK_FALSE(emote_url_allowed("x", "https://eviltwimg.com/a.png"));
	CHECK_FALSE(emote_url_allowed("x", "https://x.twimg.com.evil.example/a.png"));
	CHECK_FALSE(emote_url_allowed("youtube", "https://www.youtube.com/watch?v=abc"));        // only its emoji path
	// scheme and characters
	CHECK_FALSE(emote_url_allowed("twitch", "http://static-cdn.jtvnw.net/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "javascript:alert(1)"));
	CHECK_FALSE(emote_url_allowed("twitch", "data:image/svg+xml,<svg/onload=alert(1)>"));
	CHECK_FALSE(emote_url_allowed("twitch", "//static-cdn.jtvnw.net/a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net/a.png\" onerror=\"x"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net/a b.png"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net/a.png)"));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net/\\a.png"));
	CHECK_FALSE(emote_url_allowed("twitch", ""));
	CHECK_FALSE(emote_url_allowed("twitch", "https://static-cdn.jtvnw.net/" + std::string(600, 'a')));
	CHECK_FALSE(emote_url_allowed("nope", "https://static-cdn.jtvnw.net/a.png"));
	CHECK(chat_img_src_csp().find("https://*.twimg.com") != std::string::npos);
	CHECK(chat_img_src_csp().find("https://www.youtube.com/s/gaming/emoji/") != std::string::npos);
	CHECK(chat_img_src_csp().find("unsafe") == std::string::npos);
}

// ---- L2: the hub ----------------------------------------------------------------------------

TEST(L2, hub_accepts_orders_and_shapes_a_normal_message)
{
	MockClock clock(10000);
	ChatHub hub(clock);
	IngestResult r = hub.ingest(batch(msg("m1", "Alice", "hello world", "twitch", "c1", ",\"color\":\"#FF00aa\",\"badges\":[\"moderator\",\"sub\"]")));
	CHECK_EQ(r.http_status, 200);
	CHECK_EQ(r.accepted, 1);
	CHECK(hub.backlog(10).empty()); // held for the reordering window
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(all.size(), static_cast<size_t>(1));
	CHECK_EQ(all[0].author, S("Alice"));
	CHECK_EQ(plain_text(all[0]), S("hello world"));
	CHECK_EQ(all[0].color, S("#ff00aa"));
	CHECK_EQ(all[0].badges.size(), static_cast<size_t>(2));
	CHECK_EQ(all[0].platform, S("twitch"));
	CHECK_EQ(all[0].seq, static_cast<uint64_t>(1));
}

TEST(L2, hub_dedupes_by_platform_and_id_only)
{
	MockClock clock(10000);
	ChatHub hub(clock);
	hub.ingest(batch(msg("same", "A", "one", "twitch") + "," + msg("same", "A", "one again", "twitch")));
	IngestResult r = hub.ingest(batch(msg("same", "B", "other platform", "kick", "c2") + "," + msg("same", "A", "late dup", "twitch")));
	CHECK_EQ(r.accepted, 1); // kick/same is a different key
	CHECK_EQ(r.duplicates, 1);
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(all.size(), static_cast<size_t>(2));
}

TEST(L2, hub_orders_inside_the_reordering_window_by_when_the_reader_saw_them)
{
	MockClock clock(100000);
	ChatHub hub(clock); // reorder_ms 300
	// one batch: the reader saw "second" 400 ms ago... and "first" 900 ms ago, but listed them the other way round
	hub.ingest(batch(msg("b", "B", "second", "twitch", "c1", ",\"t\":99600") + "," + msg("a", "A", "first", "twitch", "c1", ",\"t\":99100"), ",\"sent\":100000"));
	// a later batch with a message seen between the two
	clock.advance(100);
	hub.ingest(batch(msg("c", "C", "third", "kick", "c2", ",\"t\":100050"), ",\"sent\":100100"));
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(all.size(), static_cast<size_t>(3));
	CHECK_EQ(plain_text(all[0]), S("first"));
	CHECK_EQ(plain_text(all[1]), S("second"));
	CHECK_EQ(plain_text(all[2]), S("third"));
	for (size_t i = 0; i < all.size(); ++i)
		CHECK_EQ(all[i].seq, static_cast<uint64_t>(i + 1));
}

TEST(L2, hub_equal_times_keep_arrival_order_and_future_times_are_clamped)
{
	MockClock clock(100000);
	ChatHub hub(clock);
	// "t" in the future (clock skew or a lie) cannot jump the queue: age is clamped to >= 0
	hub.ingest(batch(msg("1", "A", "one") + "," + msg("2", "A", "two", "twitch", "c1", ",\"t\":999999999") + "," + msg("3", "A", "three"), ",\"sent\":100000"));
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(plain_text(all[0]), S("one"));
	CHECK_EQ(plain_text(all[1]), S("two"));
	CHECK_EQ(plain_text(all[2]), S("three"));
	// a huge claimed age is capped at max_age_ms, so it cannot reorder arbitrarily far back
	MockClock c2(100000);
	ChatHub h2(c2);
	h2.ingest(batch(msg("1", "A", "old", "twitch", "c1", ",\"t\":1"), ",\"sent\":100000"));
	c2.advance(400);
	h2.ingest(batch(msg("2", "A", "newer", "twitch", "c1", ",\"t\":99000"), ",\"sent\":100400"));
	std::vector<ChatMessage> a2 = drain(h2, c2);
	CHECK_EQ(plain_text(a2[0]), S("old"));
}

TEST(L2, hub_ring_is_capped_and_seq_is_monotonic)
{
	MockClock clock(1000);
	ChatLimits lim;
	lim.ring = 5;
	lim.rate_burst = 1000;
	lim.rate_per_sec = 1000;
	ChatHub hub(clock, lim);
	for (int i = 0; i < 12; ++i) {
		hub.ingest(batch(msg("m" + std::to_string(i), "A", "text " + std::to_string(i))));
		clock.advance(400);
		hub.flush();
	}
	std::vector<ChatMessage> all = hub.backlog(100);
	CHECK_EQ(all.size(), static_cast<size_t>(5));
	CHECK_EQ(all.front().seq, static_cast<uint64_t>(8));
	CHECK_EQ(all.back().seq, static_cast<uint64_t>(12));
	std::vector<ChatMessage> out;
	CHECK(hub.wait_for(9, 10, out, 100));
	CHECK_EQ(out.size(), static_cast<size_t>(3));
	CHECK_FALSE(hub.wait_for(12, 10, out, 100));
}

TEST(L2, hub_dedupe_memory_is_bounded)
{
	MockClock clock(1000);
	ChatLimits lim;
	lim.dedupe_keep = 10;
	lim.rate_burst = 100000;
	lim.rate_per_sec = 100000;
	ChatHub hub(clock, lim);
	for (int i = 0; i < 50; ++i)
		hub.ingest(batch(msg("m" + std::to_string(i), "A", "x")));
	// the first id was forgotten: it is accepted again, an id from the last 10 is still a duplicate
	CHECK_EQ(hub.ingest(batch(msg("m0", "A", "x"))).accepted, 1);
	CHECK_EQ(hub.ingest(batch(msg("m49", "A", "x"))).duplicates, 1);
}

TEST(L2, hub_blocklist_words_and_users_beat_unicode_tricks)
{
	MockClock clock(1000);
	ChatHub hub(clock);
	ChatFilter f;
	f.words.push_back("BadWord");
	f.words.push_back("  ");
	f.users.push_back("TrollUser");
	hub.set_filter(f);
	IngestResult r = hub.ingest(batch(msg("1", "Alice", "this has a BADWORD inside") + "," + msg("2", "alice", "clean") + "," +
					   msg("3", "trolluser", "innocent") + "," + msg("4", "Bob", S("b\xE2\x80\x8B" "ad\xE2\x80\xAE" "word")) + "," +
					   msg("5", "Carol", "<b>BAD</b>word")));
	CHECK_EQ(r.accepted, 1);
	CHECK_EQ(r.blocked, 4);
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(plain_text(all[0]), S("clean"));
}

TEST(L2, hub_hide_commands_and_bots)
{
	MockClock clock(1000);
	ChatHub hub(clock);
	ChatFilter f;
	f.hide_commands = true;
	f.hide_bots = true;
	hub.set_filter(f);
	IngestResult r = hub.ingest(batch(msg("1", "Alice", "!uptime") + "," + msg("2", "Alice", "/me waves") + "," + msg("3", "Alice", "!") + "," +
					  msg("4", "Alice", "! not a command") + "," + msg("5", "Nightbot", "Follow the stream") + "," +
					  msg("6", "Alice", "hello !bang")));
	CHECK_EQ(r.blocked, 3); // !uptime, /me, Nightbot
	CHECK_EQ(r.accepted, 3);
	f.hide_commands = false;
	f.hide_bots = false;
	hub.set_filter(f);
	CHECK_EQ(hub.ingest(batch(msg("7", "Nightbot", "!uptime"))).accepted, 1);
}

TEST(L2, hub_rate_limit_is_per_source_and_refills)
{
	MockClock clock(1000);
	ChatLimits lim;
	lim.rate_burst = 5;
	lim.rate_per_sec = 2;
	ChatHub hub(clock, lim);
	std::string many;
	for (int i = 0; i < 10; ++i)
		many += (i ? "," : "") + msg("a" + std::to_string(i), "A", "x", "twitch", "c1");
	IngestResult r = hub.ingest(batch(many));
	CHECK_EQ(r.accepted, 5);
	CHECK_EQ(r.rate_limited, 5);
	// another source is not affected
	CHECK_EQ(hub.ingest(batch(msg("k1", "K", "x", "kick", "c2"))).accepted, 1);
	clock.advance(1000); // two tokens
	std::string more = msg("b1", "A", "x") + "," + msg("b2", "A", "x") + "," + msg("b3", "A", "x");
	r = hub.ingest(batch(more));
	CHECK_EQ(r.accepted, 2);
	CHECK_EQ(r.rate_limited, 1);
}

TEST(L2, hub_enforces_batch_body_and_schema_caps)
{
	MockClock clock(1000);
	ChatHub hub(clock);
	std::string many;
	for (int i = 0; i < 101; ++i)
		many += (i ? "," : "") + msg("m" + std::to_string(i), "A", "x");
	CHECK_EQ(hub.ingest(batch(many)).http_status, 413);
	CHECK_EQ(hub.ingest(std::string(300000, ' ')).http_status, 413);
	CHECK_EQ(hub.ingest("not json").http_status, 400);
	CHECK_EQ(hub.ingest("[]").http_status, 400);
	CHECK_EQ(hub.ingest("{\"messages\":[]}").http_status, 400);            // no version
	CHECK_EQ(hub.ingest("{\"v\":2,\"messages\":[]}").http_status, 400);
	CHECK_EQ(hub.ingest("{\"v\":1,\"messages\":{}}").http_status, 400);
	CHECK_EQ(hub.ingest("{\"v\":1,\"messages\":[]}").http_status, 200);
	std::string deep = "{\"v\":1,\"x\":";
	for (int i = 0; i < 200; ++i)
		deep += "[";
	CHECK_EQ(hub.ingest(deep).http_status, 400);
	CHECK_EQ(hub.ingest("{\"v\":1,\"messages\":[\"\\ud800\"]}").http_status, 400); // lone surrogate is a parse error
}

TEST(L2, hub_counts_invalid_messages_without_failing_the_batch)
{
	MockClock clock(1000);
	ChatHub hub(clock);
	std::string bad = "42,null,\"str\",{},{\"platform\":\"myspace\",\"source\":\"c1\",\"id\":\"1\",\"author\":\"A\",\"text\":\"x\"},"
			  "{\"platform\":\"twitch\",\"source\":\"bad source!\",\"id\":\"1\",\"author\":\"A\",\"text\":\"x\"},"
			  "{\"platform\":\"twitch\",\"source\":\"c1\",\"id\":\"\",\"author\":\"A\",\"text\":\"x\"},"
			  "{\"platform\":\"twitch\",\"source\":\"c1\",\"id\":\"i\",\"author\":\"\",\"text\":\"x\"},"
			  "{\"platform\":\"twitch\",\"source\":\"c1\",\"id\":\"i\",\"author\":\"A\",\"text\":\"\"},"
			  "{\"platform\":\"twitch\",\"source\":\"c1\",\"id\":\"i\",\"author\":\"A\",\"text\":\"<b></b>\"}";
	IngestResult r = hub.ingest(batch(bad + "," + msg("ok", "A", "fine")));
	CHECK_EQ(r.http_status, 200);
	CHECK_EQ(r.accepted, 1);
	CHECK_EQ(r.invalid, 10);
}

TEST(L2, hub_truncates_huge_text_author_and_limits_parts)
{
	MockClock clock(1000);
	ChatHub hub(clock);
	std::string big(5000, 'a');
	std::string author(500, 'n');
	hub.ingest(batch(msg("1", author, big)));
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(all.size(), static_cast<size_t>(1));
	CHECK(plain_text(all[0]).size() <= 504);
	CHECK(all[0].author.size() <= 43);
	// 100 parts: refused as invalid (cap is 64)
	std::string parts = "{\"platform\":\"twitch\",\"source\":\"c1\",\"id\":\"p\",\"author\":\"A\",\"parts\":[";
	for (int i = 0; i < 100; ++i)
		parts += std::string(i ? "," : "") + "{\"t\":\"text\",\"v\":\"x\"}";
	parts += "]}";
	CHECK_EQ(hub.ingest(batch(parts)).invalid, 1);
	// 40 emotes in 60 parts: only 30 survive
	std::string em = "{\"platform\":\"twitch\",\"source\":\"c1\",\"id\":\"e\",\"author\":\"A\",\"parts\":[";
	for (int i = 0; i < 40; ++i)
		em += std::string(i ? "," : "") + "{\"t\":\"emote\",\"v\":\"K\",\"u\":\"https://static-cdn.jtvnw.net/e/" + std::to_string(i) + "\"}";
	em += "]}";
	hub.ingest(batch(em));
	all = drain(hub, clock);
	size_t emotes = 0;
	for (size_t i = 0; i < all.back().parts.size(); ++i)
		emotes += all.back().parts[i].emote ? 1 : 0;
	CHECK_EQ(emotes, static_cast<size_t>(30));
}

TEST(L2, hub_emote_on_a_foreign_host_becomes_plain_text)
{
	MockClock clock(1000);
	ChatHub hub(clock);
	std::string m = "{\"platform\":\"twitch\",\"source\":\"c1\",\"id\":\"e1\",\"author\":\"A\",\"parts\":["
			"{\"t\":\"text\",\"v\":\"hi \"},{\"t\":\"emote\",\"v\":\"Kappa\",\"u\":\"https://static-cdn.jtvnw.net/emoticons/v2/25/default/dark/1.0\"},"
			"{\"t\":\"text\",\"v\":\" and \"},{\"t\":\"emote\",\"v\":\"Evil\",\"u\":\"https://evil.example/x.png\"},"
			"{\"t\":\"emote\",\"v\":\"Js\",\"u\":\"javascript:alert(1)\"},{\"t\":\"text\",\"v\":\" bye\"}]}";
	hub.ingest(batch(m));
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(all.size(), static_cast<size_t>(1));
	CHECK_EQ(plain_text(all[0]), S("hi [Kappa] and EvilJs bye"));
	size_t emotes = 0;
	for (size_t i = 0; i < all[0].parts.size(); ++i)
		if (all[0].parts[i].emote) {
			++emotes;
			CHECK(all[0].parts[i].url.find("https://static-cdn.jtvnw.net/") == 0);
		}
	CHECK_EQ(emotes, static_cast<size_t>(1));
}

TEST(L2, hub_message_json_is_valid_and_carries_no_html)
{
	MockClock clock(1000);
	ChatHub hub(clock);
	hub.ingest(batch(msg("1", "<img src=x onerror=alert(1)>Mallory", "<script>alert(1)</script> \"quoted\" \\ back"), ",\"sent\":1000"));
	std::vector<ChatMessage> all = drain(hub, clock);
	CHECK_EQ(all.size(), static_cast<size_t>(1));
	std::string j = chat_message_json(all[0]);
	json::ParseResult pr = json::parse(j);
	CHECK(pr.ok);
	CHECK_EQ(pr.value.find("author")->as_string(), S("Mallory"));
	CHECK(j.find("<script") == std::string::npos);
	CHECK(j.find("<img") == std::string::npos);
	CHECK_EQ(pr.value.find("parts")->at(0).find("v")->as_string(), S("alert(1) \"quoted\" \\ back"));
}

TEST(L2, hub_token_check_is_exact_and_empty_token_disables_ingest)
{
	MockClock clock(1);
	ChatHub hub(clock);
	CHECK_FALSE(hub.token_ok(""));
	CHECK_FALSE(hub.token_ok("anything"));
	hub.set_token("abcdef0123456789");
	CHECK(hub.token_ok("abcdef0123456789"));
	CHECK_FALSE(hub.token_ok("abcdef012345678"));
	CHECK_FALSE(hub.token_ok("abcdef01234567890"));
	CHECK_FALSE(hub.token_ok("ABCDEF0123456789"));
	CHECK_FALSE(hub.token_ok(""));
	hub.set_token("");
	CHECK_FALSE(hub.token_ok("abcdef0123456789"));
	std::string a = random_token_hex(32), b = random_token_hex(32);
	CHECK_EQ(a.size(), static_cast<size_t>(64));
	CHECK(a != b);
	CHECK_EQ(random_token_hex(0), S(""));
}

TEST(L2, hub_tracks_source_status_and_rejects_junk_states)
{
	MockClock clock(5000);
	ChatHub hub(clock);
	std::string body = "{\"v\":1,\"sources\":[{\"id\":\"c1\",\"platform\":\"twitch\",\"state\":\"layout\",\"detail\":\"chat container not found <b>x</b>\"},"
			   "{\"id\":\"c2\",\"platform\":\"kick\",\"state\":\"hacked\"},{\"id\":\"c3\",\"platform\":\"myspace\",\"state\":\"reading\"}]}";
	CHECK_EQ(hub.ingest(body).http_status, 200);
	std::vector<SourceStatus> s = hub.sources();
	CHECK_EQ(s.size(), static_cast<size_t>(1));
	CHECK_EQ(s[0].state, S("layout"));
	CHECK(s[0].detail.find('<') == std::string::npos);
	json::ParseResult pr = json::parse(hub.status_json());
	CHECK(pr.ok);
	hub.mark_all_sources("stopped", "reader stopped");
	CHECK_EQ(hub.sources()[0].state, S("stopped"));
	// the table is bounded
	ChatLimits lim;
	lim.max_sources = 2;
	ChatHub small(clock, lim);
	small.ingest(batch(msg("1", "A", "x", "twitch", "c1") + "," + msg("2", "A", "x", "twitch", "c2") + "," + msg("3", "A", "x", "twitch", "c3")));
	CHECK_EQ(small.sources().size(), static_cast<size_t>(2));
}

// ---- L2: config schema 3 --------------------------------------------------------------------

TEST(L2, config_v2_migrates_to_v3_with_empty_chat)
{
	InsecureTestProtector p;
	LoadResult r = parse_config("{\"schema\":2,\"destinations\":[]}", p);
	CHECK(r.status == LoadStatus::Migrated);
	CHECK(r.needs_save);
	CHECK_EQ(r.config.schema, 3);
	CHECK(r.config.chat.sources.empty());
	CHECK_FALSE(r.config.chat.start_with_obs);
	std::string out, err;
	CHECK(serialize_config(r.config, out, err));
	CHECK(out.find("\"schema\": 3") != std::string::npos);
	LoadResult again = parse_config(out, p);
	CHECK(again.status == LoadStatus::Ok);
}

TEST(L2, config_chat_block_round_trips_and_validates)
{
	InsecureTestProtector p;
	std::string text = "{\"schema\":3,\"chat\":{\"start_with_obs\":true,\"node_path\":\"C:\\\\Program Files\\\\nodejs\\\\node.exe\","
			   "\"quiet_minutes\":9,\"hide_commands\":true,\"hide_bots\":true,\"blocked_words\":[\"spam\",\"\",5,\"x\\u0001y\"],"
			   "\"blocked_users\":[\"troll\"],\"sources\":["
			   "{\"id\":\"c1\",\"platform\":\"twitch\",\"target\":\"shroud\",\"enabled\":true},"
			   "{\"id\":\"c2\",\"platform\":\"twitch\",\"target\":\"https://evil.example/\",\"enabled\":true},"
			   "{\"id\":\"c3\",\"platform\":\"myspace\",\"target\":\"x\"},"
			   "{\"id\":\"c1\",\"platform\":\"kick\",\"target\":\"xqc\",\"enabled\":false},"
			   "{\"id\":\"c5\",\"platform\":\"twitch\",\"target\":\"Shroud\"},"
			   "7]}}";
	LoadResult r = parse_config(text, p);
	CHECK(r.status == LoadStatus::Ok);
	CHECK(r.config.chat.start_with_obs);
	CHECK_EQ(r.config.chat.quiet_minutes, 9);
	CHECK_EQ(r.config.chat.sources.size(), static_cast<size_t>(2)); // evil target, unknown platform, unnormalized target and non-object skipped
	CHECK_EQ(r.config.chat.sources[0].target, S("shroud"));
	CHECK(r.config.chat.sources[1].id != S("c1")); // duplicate id was replaced
	CHECK_FALSE(r.config.chat.sources[1].enabled);
	CHECK_EQ(r.config.chat.blocked_words.size(), static_cast<size_t>(2)); // "", 5 skipped; control char removed from the other
	CHECK_EQ(r.config.chat.blocked_words[1], S("xy"));
	CHECK(r.warnings.size() >= 3);
	std::string out, err;
	CHECK(serialize_config(r.config, out, err));
	LoadResult again = parse_config(out, p);
	CHECK(again.status == LoadStatus::Ok);
	CHECK_EQ(again.config.chat.sources.size(), static_cast<size_t>(2));
	CHECK_EQ(again.config.chat.node_path, r.config.chat.node_path);
	CHECK(again.config.chat.hide_bots);
}

TEST(L2, config_node_path_validation)
{
	CHECK(valid_node_path(""));
	CHECK(valid_node_path("C:\\Program Files\\nodejs\\node.exe"));
	CHECK(valid_node_path("D:/tools/node.exe"));
	CHECK(valid_node_path("\\\\server\\share\\node.exe"));
	CHECK_FALSE(valid_node_path("node.exe"));                       // relative: PATH lookup is the empty setting
	CHECK_FALSE(valid_node_path("C:\\x\\notnode.exe"));
	CHECK_FALSE(valid_node_path("C:\\x\\node.exe --evil"));
	CHECK_FALSE(valid_node_path("C:\\x\\node.exe\" --inspect \"a"));
	CHECK_FALSE(valid_node_path("C:\\x\\node.cmd"));
	CHECK_FALSE(valid_node_path("C:\\x\\node.exe|calc"));
	CHECK_FALSE(valid_node_path(std::string("C:\\x\\node.exe") + std::string(1, '\0')));
	CHECK_FALSE(valid_node_path(std::string(300, 'a')));
}

TEST(L2, reader_config_has_enabled_sources_and_never_a_token)
{
	Config cfg;
	ChatSource a;
	a.id = "c1";
	a.platform = "twitch";
	a.target = "shroud";
	ChatSource b;
	b.id = "c2";
	b.platform = "kick";
	b.target = "xqc";
	b.enabled = false;
	cfg.chat.sources.push_back(a);
	cfg.chat.sources.push_back(b);
	std::string text = build_reader_config(cfg, 51734);
	json::ParseResult pr = json::parse(text);
	CHECK(pr.ok);
	CHECK_EQ(pr.value.find("port")->as_int(), static_cast<int64_t>(51734));
	CHECK_EQ(pr.value.find("sources")->size(), static_cast<size_t>(1));
	CHECK(text.find("token") == std::string::npos);
	CHECK(text.find("key") == std::string::npos);
}

// ---- L2: routing ----------------------------------------------------------------------------

static HttpRequest creq(const std::string &method, const std::string &target, const std::string &host)
{
	HttpRequest r;
	r.method = method;
	r.target = target;
	size_t q = target.find('?');
	r.path = q == std::string::npos ? target : target.substr(0, q);
	r.query = q == std::string::npos ? std::string() : target.substr(q + 1);
	r.version = "HTTP/1.1";
	if (!host.empty())
		r.headers["host"] = host;
	return r;
}

static HttpRequest ingest_req(const std::string &body, const std::string &token = "tok123", int port = 5000)
{
	HttpRequest r = creq("POST", "/chat/ingest", "127.0.0.1:" + std::to_string(port));
	if (!token.empty())
		r.headers["authorization"] = "Bearer " + token;
	r.headers["content-type"] = "application/json";
	r.headers["content-length"] = std::to_string(body.size());
	r.body = body;
	return r;
}

static std::string hdr(const HttpResponse &r, const std::string &name)
{
	for (size_t i = 0; i < r.headers.size(); ++i)
		if (r.headers[i].first == name)
			return r.headers[i].second;
	return std::string();
}

TEST(L2, ingest_requires_token_host_json_and_length_before_reading_a_body)
{
	MockClock clock(1);
	ChatHub hub(clock);
	hub.set_token("tok123");
	StateHub sh;
	AssetMap assets;
	std::string ok_body = batch(msg("1", "A", "hi"));
	HttpResponse r = handle_request(ingest_req(ok_body), 5000, assets, sh, &hub);
	CHECK_EQ(r.status, 200);
	CHECK(r.body.find("\"accepted\":1") != std::string::npos);

	CHECK_EQ(handle_request(ingest_req(ok_body, ""), 5000, assets, sh, &hub).status, 401);           // no header
	CHECK_EQ(handle_request(ingest_req(ok_body, "wrong"), 5000, assets, sh, &hub).status, 401);
	CHECK_EQ(handle_request(ingest_req(ok_body, "tok12"), 5000, assets, sh, &hub).status, 401);       // prefix
	CHECK_EQ(handle_request(ingest_req(ok_body, "tok1234"), 5000, assets, sh, &hub).status, 401);     // longer
	HttpRequest basic = ingest_req(ok_body);
	basic.headers["authorization"] = "Basic dG9rMTIz";
	CHECK_EQ(handle_request(basic, 5000, assets, sh, &hub).status, 401);
	HttpRequest bearer_lc = ingest_req(ok_body);
	bearer_lc.headers["authorization"] = "bearer tok123";
	CHECK_EQ(handle_request(bearer_lc, 5000, assets, sh, &hub).status, 200);                         // scheme is case-insensitive

	HttpRequest rebind = ingest_req(ok_body);
	rebind.headers["host"] = "evil.example:5000";
	CHECK_EQ(handle_request(rebind, 5000, assets, sh, &hub).status, 403);
	HttpRequest nohost = ingest_req(ok_body);
	nohost.headers.erase("host");
	CHECK_EQ(handle_request(nohost, 5000, assets, sh, &hub).status, 403);
	// a browser page (any Origin or fetch metadata) is refused even with the right token
	HttpRequest origin = ingest_req(ok_body);
	origin.headers["origin"] = "http://127.0.0.1:5000";
	CHECK_EQ(handle_request(origin, 5000, assets, sh, &hub).status, 403);
	HttpRequest fetchsite = ingest_req(ok_body);
	fetchsite.headers["sec-fetch-site"] = "same-origin";
	CHECK_EQ(handle_request(fetchsite, 5000, assets, sh, &hub).status, 403);

	HttpRequest nodefetch = ingest_req(ok_body); // what Node's fetch adds: Sec-Fetch-Mode only
	nodefetch.headers["sec-fetch-mode"] = "cors";
	CHECK_EQ(handle_request(nodefetch, 5000, assets, sh, &hub).status, 200);

	HttpRequest ct = ingest_req(ok_body);
	ct.headers["content-type"] = "text/plain";
	CHECK_EQ(handle_request(ct, 5000, assets, sh, &hub).status, 415);
	ct.headers["content-type"] = "application/x-www-form-urlencoded";
	CHECK_EQ(handle_request(ct, 5000, assets, sh, &hub).status, 415);
	HttpRequest nolen = ingest_req(ok_body);
	nolen.headers.erase("content-length");
	CHECK_EQ(handle_request(nolen, 5000, assets, sh, &hub).status, 411);
	HttpRequest badlen = ingest_req(ok_body);
	badlen.headers["content-length"] = "12abc";
	CHECK_EQ(handle_request(badlen, 5000, assets, sh, &hub).status, 400);
	badlen.headers["content-length"] = "-5";
	CHECK_EQ(handle_request(badlen, 5000, assets, sh, &hub).status, 400);
	HttpRequest bigclaim = ingest_req(ok_body);
	bigclaim.headers["content-length"] = "999999999";
	CHECK_EQ(handle_request(bigclaim, 5000, assets, sh, &hub).status, 413);
	HttpRequest chunked = ingest_req(ok_body);
	chunked.headers["transfer-encoding"] = "chunked";
	CHECK_EQ(handle_request(chunked, 5000, assets, sh, &hub).status, 400);
	HttpRequest mismatch = ingest_req(ok_body);
	mismatch.body = ok_body.substr(0, 10);
	CHECK_EQ(handle_request(mismatch, 5000, assets, sh, &hub).status, 400);
	CHECK_EQ(hdr(handle_request(ingest_req(ok_body, ""), 5000, assets, sh, &hub), "WWW-Authenticate"), S("Bearer"));

	// methods
	HttpRequest get = creq("GET", "/chat/ingest", "127.0.0.1:5000");
	CHECK_EQ(handle_request(get, 5000, assets, sh, &hub).status, 405);
	HttpRequest put = ingest_req(ok_body);
	put.method = "PUT";
	CHECK_EQ(handle_request(put, 5000, assets, sh, &hub).status, 405);
	// no token set: everything is refused
	hub.set_token("");
	CHECK_EQ(handle_request(ingest_req(ok_body, "tok123"), 5000, assets, sh, &hub).status, 401);
	CHECK_EQ(handle_request(ingest_req(ok_body, " "), 5000, assets, sh, &hub).status, 401);
}

TEST(L2, ingest_does_not_exist_without_a_chat_hub_and_posts_elsewhere_stay_refused)
{
	StateHub sh;
	AssetMap assets;
	HttpResponse r = handle_request(ingest_req("{}"), 5000, assets, sh);
	CHECK_EQ(r.status, 405); // the original read-only behaviour
	MockClock clock(1);
	ChatHub hub(clock);
	hub.set_token("tok123");
	HttpRequest other = ingest_req("{}");
	other.target = other.path = "/state.json";
	CHECK_EQ(handle_request(other, 5000, assets, sh, &hub).status, 405);
	CHECK_EQ(handle_request(creq("GET", "/chat/events", "127.0.0.1:5000"), 5000, assets, sh).status, 404);
	CHECK_EQ(handle_request(creq("GET", "/chat/status.json", "127.0.0.1:5000"), 5000, assets, sh).status, 404);
}

TEST(L2, chat_reads_follow_the_same_host_origin_and_fetch_site_rules)
{
	MockClock clock(1);
	ChatHub hub(clock);
	hub.set_token("tok123");
	StateHub sh;
	AssetMap assets;
	Asset page;
	page.content_type = "text/html; charset=utf-8";
	page.body = "<!doctype html>";
	assets["/chat.html"] = page;
	const char *paths[] = {"/chat/events", "/chat/status.json", "/chat.html"};
	for (size_t i = 0; i < 3; ++i) {
		HttpResponse ok = handle_request(creq("GET", paths[i], "127.0.0.1:5000"), 5000, assets, sh, &hub);
		CHECK_EQ(ok.status, 200);
		CHECK(hdr(ok, "Access-Control-Allow-Origin").empty());
		CHECK_EQ(hdr(ok, "X-Content-Type-Options"), S("nosniff"));
		CHECK_EQ(handle_request(creq("GET", paths[i], "evil.example:5000"), 5000, assets, sh, &hub).status, 403);
		HttpRequest o = creq("GET", paths[i], "127.0.0.1:5000");
		o.headers["origin"] = "https://evil.example";
		CHECK_EQ(handle_request(o, 5000, assets, sh, &hub).status, 403);
		if (i < 2) {
			HttpRequest cross = creq("GET", paths[i], "127.0.0.1:5000");
			cross.headers["sec-fetch-site"] = "cross-site";
			CHECK_EQ(handle_request(cross, 5000, assets, sh, &hub).status, 403);
		}
	}
	HttpResponse ev = handle_request(creq("GET", "/chat/events?backlog=7", "127.0.0.1:5000"), 5000, assets, sh, &hub);
	CHECK(ev.sse);
	CHECK_EQ(ev.sse_kind, 1);
	CHECK_EQ(ev.chat_backlog, static_cast<size_t>(7));
	HttpRequest resume = creq("GET", "/chat/events", "127.0.0.1:5000");
	resume.headers["last-event-id"] = "41";
	ev = handle_request(resume, 5000, assets, sh, &hub);
	CHECK(ev.chat_resume);
	CHECK_EQ(ev.chat_after, static_cast<uint64_t>(41));
	CHECK_EQ(handle_request(creq("GET", "/chat/events?backlog=9999", "127.0.0.1:5000"), 5000, assets, sh, &hub).chat_backlog, static_cast<size_t>(200));
}

TEST(L2, chat_page_csp_allows_only_platform_emote_hosts_for_images)
{
	MockClock clock(1);
	ChatHub hub(clock);
	StateHub sh;
	AssetMap assets;
	Asset page;
	page.content_type = "text/html; charset=utf-8";
	page.body = "x";
	assets["/chat.html"] = page;
	assets["/overlay.html"] = page;
	HttpResponse r = handle_request(creq("GET", "/chat.html", "127.0.0.1:5000"), 5000, assets, sh, &hub);
	std::string csp = hdr(r, "Content-Security-Policy");
	CHECK(csp.find("img-src 'self' data: https://") != std::string::npos);
	CHECK(csp.find("static-cdn.jtvnw.net") != std::string::npos);
	CHECK(csp.find("script-src 'self'") != std::string::npos);
	CHECK(csp.find("unsafe-eval") == std::string::npos);
	CHECK(csp.find("img-src https:") == std::string::npos);
	CHECK(csp.find("connect-src 'self'") != std::string::npos);
	// the state overlay keeps its strict policy
	HttpResponse o = handle_request(creq("GET", "/overlay.html", "127.0.0.1:5000"), 5000, assets, sh, &hub);
	CHECK(hdr(o, "Content-Security-Policy").find("static-cdn") == std::string::npos);
}

// ---- L3: real sockets -----------------------------------------------------------------------

static void cnet_init()
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

static csock cconnect(int port)
{
	cnet_init();
	csock s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == kBadCSock)
		return kBadCSock;
	sockaddr_in a;
	std::memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons(static_cast<unsigned short>(port));
	inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
		cclose(s);
		return kBadCSock;
	}
#ifdef _WIN32
	DWORD t = 4000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&t), sizeof t);
#else
	struct timeval tv;
	tv.tv_sec = 4;
	tv.tv_usec = 0;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
	return s;
}

static std::string crecv(csock s, const std::string &until = std::string())
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

static std::string cexchange(int port, const std::string &raw)
{
	csock s = cconnect(port);
	if (s == kBadCSock)
		return "<connect failed>";
	send(s, raw.data(), static_cast<int>(raw.size()), 0);
	std::string r = crecv(s);
	cclose(s);
	return r;
}

static std::string post_raw(int port, const std::string &body, const std::string &token, const std::string &host_override = std::string(),
			    const std::string &extra_headers = std::string())
{
	std::string host = host_override.empty() ? "127.0.0.1:" + std::to_string(port) : host_override;
	std::string req = "POST /chat/ingest HTTP/1.1\r\nHost: " + host + "\r\nContent-Type: application/json\r\nContent-Length: " +
			  std::to_string(body.size()) + "\r\n";
	if (!token.empty())
		req += "Authorization: Bearer " + token + "\r\n";
	req += extra_headers + "Connection: close\r\n\r\n" + body;
	return cexchange(port, req);
}

TEST(L3, chat_ingest_over_the_wire_then_sse_delivers_in_order)
{
	StateHub sh;
	SystemClock clock;
	ChatHub hub(clock);
	hub.set_token("secrettoken");
	ServerOptions opts;
	HttpServer srv(sh, AssetMap(), opts, &hub);
	std::string err;
	CHECK(srv.start(err));
	int port = srv.port();

	std::string good = post_raw(port, batch(msg("w1", "Wire", "first") + "," + msg("w2", "Wire", "second")), "secrettoken");
	CHECK(good.find("HTTP/1.1 200 OK") == 0);
	CHECK(good.find("\"accepted\":2") != std::string::npos);
	CHECK(post_raw(port, batch(msg("w3", "Wire", "x")), "wrong").find("HTTP/1.1 401") == 0);
	CHECK(post_raw(port, batch(msg("w3", "Wire", "x")), "").find("HTTP/1.1 401") == 0);
	CHECK(post_raw(port, batch(msg("w3", "Wire", "x")), "secrettoken", "evil.example:" + std::to_string(port)).find("HTTP/1.1 403") == 0);
	CHECK(post_raw(port, batch(msg("w3", "Wire", "x")), "secrettoken", "", "Origin: http://127.0.0.1:" + std::to_string(port) + "\r\n").find("HTTP/1.1 403") == 0);

	// SSE: the two messages come from the backlog once the window has passed (the accept loop flushes)
	csock s = cconnect(port);
	CHECK(s != kBadCSock);
	std::string req = "GET /chat/events HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
	send(s, req.data(), static_cast<int>(req.size()), 0);
	std::string got = crecv(s, "\"second\"");
	cclose(s);
	CHECK(got.find("text/event-stream") != std::string::npos);
	size_t a = got.find("\"first\""), b = got.find("\"second\"");
	CHECK(a != std::string::npos && b != std::string::npos && a < b);
	CHECK(got.find("id: 1\n") != std::string::npos);

	// resume: Last-Event-ID skips what was seen
	s = cconnect(port);
	req = "GET /chat/events HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nLast-Event-ID: 1\r\n\r\n";
	send(s, req.data(), static_cast<int>(req.size()), 0);
	got = crecv(s, "\"second\"");
	cclose(s);
	CHECK(got.find("\"first\"") == std::string::npos);
	CHECK(got.find("\"second\"") != std::string::npos);

	// a live message arrives while connected
	s = cconnect(port);
	req = "GET /chat/events?backlog=0 HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n\r\n";
	send(s, req.data(), static_cast<int>(req.size()), 0);
	crecv(s, "retry:");
	post_raw(port, batch(msg("w9", "Late", "live one")), "secrettoken");
	got = crecv(s, "live one");
	cclose(s);
	CHECK(got.find("live one") != std::string::npos);
	CHECK(got.find("\"first\"") == std::string::npos);

	std::string st = cexchange(port, "GET /chat/status.json HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n\r\n");
	CHECK(st.find("\"accepted\":3") != std::string::npos);
	srv.stop();
	hub.shutdown();
}

TEST(L3, unauthenticated_post_is_refused_without_reading_the_body)
{
	StateHub sh;
	SystemClock clock;
	ChatHub hub(clock);
	hub.set_token("secrettoken");
	HttpServer srv(sh, AssetMap(), ServerOptions(), &hub);
	std::string err;
	CHECK(srv.start(err));
	// claims 200000 bytes, sends none, no token: the answer must come back at once, not after a body timeout
	csock s = cconnect(srv.port());
	CHECK(s != kBadCSock);
	std::string head = "POST /chat/ingest HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) +
			   "\r\nContent-Type: application/json\r\nContent-Length: 200000\r\n\r\n";
	auto t0 = std::chrono::steady_clock::now();
	send(s, head.data(), static_cast<int>(head.size()), 0);
	std::string got = crecv(s, "\r\n\r\n");
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	cclose(s);
	CHECK(got.find("HTTP/1.1 401") == 0);
	CHECK(ms < 2000);
	// an oversized claim with a valid token is refused with 413, again without reading
	s = cconnect(srv.port());
	head = "POST /chat/ingest HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) +
	       "\r\nAuthorization: Bearer secrettoken\r\nContent-Type: application/json\r\nContent-Length: 50000000\r\n\r\n";
	send(s, head.data(), static_cast<int>(head.size()), 0);
	got = crecv(s, "\r\n\r\n");
	cclose(s);
	CHECK(got.find("HTTP/1.1 413") == 0);
	srv.stop();
}

TEST(L3, slow_or_short_ingest_bodies_time_out_without_hanging_the_server)
{
	StateHub sh;
	SystemClock clock;
	ChatHub hub(clock);
	hub.set_token("secrettoken");
	ServerOptions opts;
	opts.read_timeout_ms = 600;
	HttpServer srv(sh, AssetMap(), opts, &hub);
	std::string err;
	CHECK(srv.start(err));
	csock s = cconnect(srv.port());
	std::string head = "POST /chat/ingest HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(srv.port()) +
			   "\r\nAuthorization: Bearer secrettoken\r\nContent-Type: application/json\r\nContent-Length: 500\r\n\r\n{\"v\":1";
	send(s, head.data(), static_cast<int>(head.size()), 0);
	std::string got = crecv(s);
	cclose(s);
	CHECK(got.find("HTTP/1.1 400") == 0); // short body: refused, not accepted
	// the server is still fine
	CHECK(post_raw(srv.port(), batch(msg("z1", "Z", "ok")), "secrettoken").find("HTTP/1.1 200 OK") == 0);
	srv.stop();
}

TEST(L3, server_with_chat_still_binds_loopback_only_and_refuses_foreign_peers)
{
	StateHub sh;
	SystemClock clock;
	ChatHub hub(clock);
	hub.set_token("secrettoken");
	HttpServer srv(sh, AssetMap(), ServerOptions(), &hub);
	srv.set_peer_override_for_test("192.168.1.50");
	std::string err;
	CHECK(srv.start(err));
	CHECK_EQ(srv.bound_address(), S("127.0.0.1"));
	std::string got = post_raw(srv.port(), batch(msg("f1", "F", "x")), "secrettoken");
	CHECK(got.find("HTTP/1.1") != 0); // dropped with no response at all
	CHECK(srv.refused_peers() >= 1);
	CHECK(hub.backlog(10).empty());
	srv.stop();
}
