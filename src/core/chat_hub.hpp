// Merge point for chat read from the platforms' public chat pages by the reader process.
//   reader --(POST /chat/ingest, bearer token, loopback)--> ChatHub --(SSE /chat/events)--> overlay
//
// Everything that arrives is untrusted text from a web page. The hub:
//   - validates the schema and enforces size / rate caps
//   - reduces every text to plain text (no HTML, no control or bidi-override characters)
//   - lets an emote through as an image only when its URL is https on that platform's own CDN list
//   - drops duplicates by (platform, id), applies the user's blocklist and bot/command filters
//   - releases messages after a short reordering window, ordered by when the reader saw them
//   - keeps a ring buffer so a newly opened overlay sees the last few messages
// Pure logic with an injected Clock: tests drive time by hand. Thread safe.
#pragma once
#include "clock.hpp"
#include "json.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace oms {

struct ChatPart {
	bool emote = false;
	std::string text; // text part: the text. emote part: the emote's name (alt text)
	std::string url;  // emote part only; already checked against the allow-list
};

struct ChatMessage {
	uint64_t seq = 0;
	std::string platform; // twitch youtube kick tiktok x
	std::string source;   // the user's source id (c1, c2...), only for rate limiting and status
	std::string id;
	std::string author;
	std::string color; // "#rrggbb" or ""
	std::vector<ChatPart> parts;
	std::vector<std::string> badges;
	int64_t arrival_ms = 0; // hub clock, when the batch arrived
	int64_t order_ms = 0;   // arrival minus how long the reader says the message had been waiting
	uint64_t arrival_idx = 0;
};

struct ChatLimits {
	size_t max_body = 262144;     // bytes per POST
	size_t max_batch = 100;       // messages per POST
	size_t max_text = 500;        // code points of message text
	size_t max_author = 40;       // code points
	size_t max_parts = 64;
	size_t max_emotes = 30;       // per message; extra emotes are dropped
	size_t max_badges = 6;
	size_t max_sources = 16;
	size_t ring = 500;            // messages kept for new viewers
	size_t dedupe_keep = 4096;    // (platform,id) pairs remembered
	int reorder_ms = 300;         // messages are released after this long, sorted
	int rate_per_sec = 20;        // per source, sustained
	int rate_burst = 60;          // per source
	int max_age_ms = 5000;        // largest "seen this long ago" correction accepted from a reader
};

struct ChatFilter {
	std::vector<std::string> words; // case-insensitive substring match on the plain text
	std::vector<std::string> users; // case-insensitive exact match on the author name
	bool hide_commands = false;     // text starting with "!" or "/" followed by a non-space
	bool hide_bots = false;         // well-known bot account names
};

struct SourceStatus {
	std::string id;
	std::string platform;
	std::string state; // starting reading quiet layout login error stopped
	std::string detail;
	int64_t updated_ms = 0;
	uint64_t accepted = 0;
	uint64_t dropped = 0;
};

struct IngestResult {
	int http_status = 200;
	std::string error; // short, safe to return to the caller
	int accepted = 0;
	int duplicates = 0;
	int blocked = 0;
	int rate_limited = 0;
	int invalid = 0;
};

// --- pure helpers (unit tested) ---------------------------------------------------------------

// Plain text from untrusted input: invalid UTF-8 -> U+FFFD, tags removed, controls / bidi overrides /
// zero-width and tag characters dropped, runs of combining marks capped, whitespace collapsed,
// cut to max_codepoints (with an ellipsis). trim=false keeps one leading/trailing space.
std::string sanitize_chat_text(const std::string &in, size_t max_codepoints, bool trim = true);
bool valid_chat_color(const std::string &c); // "#rrggbb" only
std::string chat_message_json(const ChatMessage &m);
bool is_known_bot_name(const std::string &lower_name);
std::string ascii_lower(const std::string &s);

class ChatHub {
public:
	explicit ChatHub(const Clock &clock, const ChatLimits &limits = ChatLimits());

	// "" = ingest disabled (every POST is refused with 401).
	void set_token(const std::string &token);
	bool token_ok(const std::string &presented) const; // constant time
	void set_filter(const ChatFilter &f);
	const ChatLimits &limits() const { return limits_; }

	// Body is the JSON the reader POSTs: {"v":1,"sent":ms,"messages":[...],"sources":[...]}
	IngestResult ingest(const std::string &body);
	// Moves messages whose window has passed into the ring, in order.
	void flush();

	uint64_t last_seq() const;
	// Messages with seq > after, at most max. Waits up to timeout_ms when there are none.
	// false: nothing new (timeout or shutdown).
	bool wait_for(uint64_t after, int timeout_ms, std::vector<ChatMessage> &out, size_t max);
	std::vector<ChatMessage> backlog(size_t n) const;

	std::vector<SourceStatus> sources() const;
	void mark_all_sources(const std::string &state, const std::string &detail); // reader stopped
	std::string status_json() const;

	void shutdown();
	bool is_shutdown() const;

private:
	struct Bucket {
		double tokens = 0;
		int64_t last_ms = 0;
	};
	void flush_locked(int64_t now);
	void accept_message(const json::Value &obj, int64_t sent_ms, bool have_sent, int64_t now, IngestResult &res);
	void accept_status(const json::Value &obj, int64_t now);

	const Clock &clock_;
	ChatLimits limits_;
	mutable std::mutex m_;
	std::condition_variable cv_;
	std::string token_;
	ChatFilter filter_;
	std::deque<ChatMessage> ring_;
	std::vector<ChatMessage> pending_;
	std::set<std::string> seen_;
	std::deque<std::string> seen_order_;
	std::map<std::string, Bucket> buckets_;
	std::map<std::string, SourceStatus> sources_;
	uint64_t seq_ = 0;
	uint64_t arrival_counter_ = 0;
	uint64_t total_accepted_ = 0, total_dups_ = 0, total_blocked_ = 0, total_rate_ = 0, total_invalid_ = 0;
	bool down_ = false;
};

} // namespace oms
