#include "chat_hub.hpp"
#include "chat_targets.hpp"

#include <algorithm>
#include <cctype>

namespace oms {

namespace {

const uint32_t kBad = 0xFFFFFFFFu;

// Strict UTF-8 decode of one code point at s[i]. On success advances i. On failure returns kBad
// and advances by exactly one byte.
uint32_t decode_one(const std::string &s, size_t &i)
{
	unsigned char c = static_cast<unsigned char>(s[i]);
	if (c < 0x80) {
		++i;
		return c;
	}
	int need = 0;
	uint32_t cp = 0, minv = 0;
	if (c >= 0xC2 && c <= 0xDF) {
		need = 1;
		cp = c & 0x1F;
		minv = 0x80;
	} else if (c >= 0xE0 && c <= 0xEF) {
		need = 2;
		cp = c & 0x0F;
		minv = 0x800;
	} else if (c >= 0xF0 && c <= 0xF4) {
		need = 3;
		cp = c & 0x07;
		minv = 0x10000;
	} else {
		++i;
		return kBad;
	}
	if (i + static_cast<size_t>(need) >= s.size()) {
		++i;
		return kBad;
	}
	for (int k = 1; k <= need; ++k) {
		unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
		if ((cc & 0xC0) != 0x80) {
			++i;
			return kBad;
		}
		cp = (cp << 6) | (cc & 0x3F);
	}
	if (cp < minv || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
		++i;
		return kBad;
	}
	i += static_cast<size_t>(need) + 1;
	return cp;
}

void append_utf8(std::string &o, uint32_t cp)
{
	if (cp < 0x80) {
		o.push_back(static_cast<char>(cp));
	} else if (cp < 0x800) {
		o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
		o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
		o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	} else {
		o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
		o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

bool is_space_cp(uint32_t cp)
{
	return cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x85 || cp == 0xA0 ||
	       cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
	       cp == 0x3000;
}

// Characters that are invisible or reorder text and have no business in a chat line.
bool is_dropped_cp(uint32_t cp)
{
	if (cp < 0x20)
		return true; // other controls (whitespace ones were mapped before)
	if (cp >= 0x7F && cp <= 0x9F)
		return true;
	if (cp == 0xAD || cp == 0x34F || cp == 0x61C || cp == 0x115F || cp == 0x1160 || cp == 0x17B4 || cp == 0x17B5 ||
	    cp == 0x3164 || cp == 0xFEFF || cp == 0xFFA0 || cp == 0xFFFC)
		return true;
	if (cp >= 0x180B && cp <= 0x180F)
		return true;
	if (cp == 0x200B || cp == 0x200E || cp == 0x200F)
		return true; // keep ZWNJ/ZWJ: needed by Persian text and emoji sequences
	if (cp >= 0x202A && cp <= 0x202E)
		return true; // bidi embedding / override
	if (cp >= 0x2060 && cp <= 0x206F)
		return true; // word joiner, invisible operators, bidi isolates, deprecated format characters
	if (cp >= 0xFE00 && cp <= 0xFE0E)
		return true; // variation selectors (FE0F, the emoji one, stays)
	if (cp >= 0xFDD0 && cp <= 0xFDEF)
		return true;
	if (cp >= 0xFFF0 && cp <= 0xFFFB)
		return true;
	if ((cp & 0xFFFE) == 0xFFFE)
		return true; // noncharacters
	if (cp >= 0xE0000 && cp <= 0xE0FFF)
		return true; // tag characters, variation selector supplement
	return false;
}

bool is_combining_cp(uint32_t cp)
{
	return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x483 && cp <= 0x489) || (cp >= 0x591 && cp <= 0x5C7) ||
	       (cp >= 0x610 && cp <= 0x61A) || (cp >= 0x64B && cp <= 0x65F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
	       (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

bool is_ascii_letter(uint32_t cp)
{
	return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

// Removes anything that looks like a tag: "<" + letter / "/" / "!" / "?" up to the next ">" (or the
// end when there is none). "<3" and "a < b" are not tags and stay. Returns true if it changed anything.
bool strip_tags(std::vector<uint32_t> &v)
{
	std::vector<uint32_t> out;
	out.reserve(v.size());
	bool changed = false;
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i] == '<' && i + 1 < v.size() &&
		    (is_ascii_letter(v[i + 1]) || v[i + 1] == '/' || v[i + 1] == '!' || v[i + 1] == '?')) {
			size_t j = i + 1;
			while (j < v.size() && v[j] != '>')
				++j;
			i = j; // skips the '>' too (loop ++i); at the end j == size and the loop ends
			changed = true; // replaced by nothing, so "b<i></i>adword" cannot slip past a blocklist
			continue;
		}
		out.push_back(v[i]);
	}
	v.swap(out);
	return changed;
}

bool has_control_byte(const std::string &s)
{
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x20 || c == 0x7F)
			return true;
	}
	return false;
}

bool valid_source_id(const std::string &s)
{
	if (s.empty() || s.size() > 32)
		return false;
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (!(std::isalnum(c) || c == '_' || c == '-') || c >= 0x80)
			return false;
	}
	return true;
}

std::string get_str(const json::Value &o, const char *k)
{
	const json::Value *v = o.find(k);
	return (v && v->is_string()) ? v->as_string() : std::string();
}

bool state_known(const std::string &s)
{
	static const char *ok[] = {"starting", "reading", "quiet", "layout", "login", "error", "stopped"};
	for (size_t i = 0; i < sizeof ok / sizeof ok[0]; ++i)
		if (s == ok[i])
			return true;
	return false;
}

} // namespace

std::string ascii_lower(const std::string &s)
{
	std::string o = s;
	for (size_t i = 0; i < o.size(); ++i)
		if (o[i] >= 'A' && o[i] <= 'Z')
			o[i] = static_cast<char>(o[i] - 'A' + 'a');
	return o;
}

std::string sanitize_chat_text(const std::string &in_full, size_t max_codepoints, bool trim)
{
	const size_t kMaxInput = 65536;
	std::string in = in_full.size() > kMaxInput ? in_full.substr(0, kMaxInput) : in_full;
	std::vector<uint32_t> cps;
	cps.reserve(in.size());
	for (size_t i = 0; i < in.size();) {
		uint32_t cp = decode_one(in, i);
		cps.push_back(cp == kBad ? 0xFFFD : cp);
	}
	for (int pass = 0; pass < 3 && strip_tags(cps); ++pass) {
	}
	std::vector<uint32_t> out;
	out.reserve(cps.size());
	int combining_run = 0;
	bool last_space = false;
	for (size_t i = 0; i < cps.size(); ++i) {
		uint32_t cp = cps[i];
		if (is_space_cp(cp))
			cp = ' ';
		else if (is_dropped_cp(cp))
			continue;
		if (cp == ' ') {
			if (last_space)
				continue;
			last_space = true;
			combining_run = 0;
			out.push_back(cp);
			continue;
		}
		last_space = false;
		if (is_combining_cp(cp)) {
			if (++combining_run > 3)
				continue;
		} else {
			combining_run = 0;
		}
		out.push_back(cp);
	}
	if (trim) {
		while (!out.empty() && out.back() == ' ')
			out.pop_back();
		size_t lead = 0;
		while (lead < out.size() && out[lead] == ' ')
			++lead;
		out.erase(out.begin(), out.begin() + static_cast<long>(lead));
	}
	if (out.size() > max_codepoints) {
		out.resize(max_codepoints > 0 ? max_codepoints - 1 : 0);
		if (max_codepoints > 0)
			out.push_back(0x2026);
	}
	std::string res;
	res.reserve(out.size());
	for (size_t i = 0; i < out.size(); ++i)
		append_utf8(res, out[i]);
	return res;
}

bool valid_chat_color(const std::string &c)
{
	if (c.size() != 7 || c[0] != '#')
		return false;
	for (size_t i = 1; i < 7; ++i)
		if (!std::isxdigit(static_cast<unsigned char>(c[i])))
			return false;
	return true;
}

bool is_known_bot_name(const std::string &n)
{
	static const char *bots[] = {"nightbot",  "streamelements", "streamlabs", "moobot",   "fossabot", "wizebot",
				     "botrix",    "kickbot",        "sery_bot",   "buttsbot", "supibot",  "soundalerts",
				     "commanderroot", "streamlabsbot", "nightbot_"};
	for (size_t i = 0; i < sizeof bots / sizeof bots[0]; ++i)
		if (n == bots[i])
			return true;
	return false;
}

std::string chat_message_json(const ChatMessage &m)
{
	json::Value o = json::Value::object();
	o.set("seq", json::Value::number(static_cast<double>(m.seq)));
	o.set("platform", json::Value::string(m.platform));
	o.set("id", json::Value::string(m.id));
	o.set("author", json::Value::string(m.author));
	o.set("color", json::Value::string(m.color));
	json::Value parts = json::Value::array();
	for (size_t i = 0; i < m.parts.size(); ++i) {
		json::Value p = json::Value::object();
		p.set("t", json::Value::string(m.parts[i].emote ? "emote" : "text"));
		p.set("v", json::Value::string(m.parts[i].text));
		if (m.parts[i].emote)
			p.set("u", json::Value::string(m.parts[i].url));
		parts.push(p);
	}
	o.set("parts", parts);
	json::Value badges = json::Value::array();
	for (size_t i = 0; i < m.badges.size(); ++i)
		badges.push(json::Value::string(m.badges[i]));
	o.set("badges", badges);
	return json::dump(o, false);
}

// ---------------------------------------------------------------------------------------------

ChatHub::ChatHub(const Clock &clock, const ChatLimits &limits) : clock_(clock), limits_(limits) {}

void ChatHub::set_token(const std::string &token)
{
	std::lock_guard<std::mutex> lk(m_);
	token_ = token;
}

bool ChatHub::token_ok(const std::string &presented) const
{
	std::string want;
	{
		std::lock_guard<std::mutex> lk(m_);
		want = token_;
	}
	if (want.empty())
		return false;
	// constant time over the longer length; a length difference alone also fails
	size_t n = std::max(want.size(), presented.size());
	unsigned diff = static_cast<unsigned>(want.size() ^ presented.size());
	for (size_t i = 0; i < n; ++i) {
		unsigned char a = i < want.size() ? static_cast<unsigned char>(want[i]) : 0;
		unsigned char b = i < presented.size() ? static_cast<unsigned char>(presented[i]) : 0;
		diff |= static_cast<unsigned>(a ^ b);
	}
	return diff == 0;
}

void ChatHub::set_filter(const ChatFilter &f)
{
	ChatFilter c;
	for (size_t i = 0; i < f.words.size() && c.words.size() < 200; ++i) {
		std::string w = ascii_lower(sanitize_chat_text(f.words[i], 60));
		if (!w.empty())
			c.words.push_back(w);
	}
	for (size_t i = 0; i < f.users.size() && c.users.size() < 200; ++i) {
		std::string u = ascii_lower(sanitize_chat_text(f.users[i], limits_.max_author));
		if (!u.empty())
			c.users.push_back(u);
	}
	c.hide_commands = f.hide_commands;
	c.hide_bots = f.hide_bots;
	std::lock_guard<std::mutex> lk(m_);
	filter_ = c;
}

void ChatHub::accept_status(const json::Value &obj, int64_t now)
{
	if (!obj.is_object())
		return;
	std::string id = get_str(obj, "id");
	std::string platform = get_str(obj, "platform");
	std::string state = get_str(obj, "state");
	if (!valid_source_id(id) || !find_chat_platform(platform) || !state_known(state))
		return;
	std::map<std::string, SourceStatus>::iterator it = sources_.find(id);
	if (it == sources_.end()) {
		if (sources_.size() >= limits_.max_sources)
			return;
		SourceStatus s;
		s.id = id;
		it = sources_.insert(std::make_pair(id, s)).first;
	}
	it->second.platform = platform;
	it->second.state = state;
	it->second.detail = sanitize_chat_text(get_str(obj, "detail"), 160);
	it->second.updated_ms = now;
}

void ChatHub::accept_message(const json::Value &obj, int64_t sent_ms, bool have_sent, int64_t now, IngestResult &res)
{
	if (!obj.is_object()) {
		++res.invalid;
		return;
	}
	std::string platform = get_str(obj, "platform");
	std::string source = get_str(obj, "source");
	std::string id = get_str(obj, "id");
	if (!find_chat_platform(platform) || !valid_source_id(source) || id.empty() || id.size() > 128 || has_control_byte(id)) {
		++res.invalid;
		return;
	}
	// the source table is bounded: an unknown source id beyond the cap is refused
	std::map<std::string, SourceStatus>::iterator sit = sources_.find(source);
	if (sit == sources_.end() && sources_.size() >= limits_.max_sources) {
		++res.invalid;
		return;
	}

	ChatMessage m;
	m.platform = platform;
	m.source = source;
	m.id = id;
	m.author = sanitize_chat_text(get_str(obj, "author"), limits_.max_author);
	if (m.author.empty()) {
		++res.invalid;
		return;
	}
	std::string color = get_str(obj, "color");
	m.color = valid_chat_color(color) ? ascii_lower(color) : std::string();

	// parts: [{"t":"text","v":".."},{"t":"emote","v":"name","u":"https://.."}]
	const json::Value *parts = obj.find("parts");
	std::vector<ChatPart> raw;
	if (parts && parts->is_array()) {
		if (parts->size() > limits_.max_parts) {
			++res.invalid;
			return;
		}
		for (size_t i = 0; i < parts->size(); ++i) {
			const json::Value &p = parts->at(i);
			if (!p.is_object())
				continue;
			std::string t = get_str(p, "t");
			ChatPart cp;
			if (t == "text") {
				cp.text = sanitize_chat_text(get_str(p, "v"), limits_.max_text + 1, false);
			} else if (t == "emote") {
				cp.emote = true;
				cp.text = sanitize_chat_text(get_str(p, "v"), 32);
				cp.url = get_str(p, "u");
				if (!emote_url_allowed(platform, cp.url)) {
					// not on the platform's own CDN list: shown as text, never as an image
					cp.emote = false;
					cp.url.clear();
				}
			} else {
				continue;
			}
			if (cp.text.empty() && !cp.emote)
				continue;
			raw.push_back(cp);
		}
	} else {
		ChatPart cp;
		cp.text = sanitize_chat_text(get_str(obj, "text"), limits_.max_text + 1, false);
		if (!cp.text.empty())
			raw.push_back(cp);
	}
	// merge adjacent text parts (re-collapsing the double spaces that appear at the join)
	size_t emotes = 0;
	for (size_t i = 0; i < raw.size(); ++i) {
		ChatPart &p = raw[i];
		if (p.emote) {
			if (++emotes > limits_.max_emotes)
				continue;
			m.parts.push_back(p);
		} else if (!m.parts.empty() && !m.parts.back().emote) {
			m.parts.back().text = sanitize_chat_text(m.parts.back().text + p.text, limits_.max_text + 1, false);
		} else {
			m.parts.push_back(p);
		}
	}
	// trim the ends of the whole line
	if (!m.parts.empty() && !m.parts.front().emote)
		m.parts.front().text = sanitize_chat_text(m.parts.front().text, limits_.max_text + 1, false);
	if (!m.parts.empty() && !m.parts.front().emote) {
		std::string &t = m.parts.front().text;
		size_t lead = 0;
		while (lead < t.size() && t[lead] == ' ')
			++lead;
		t.erase(0, lead);
	}
	if (!m.parts.empty() && !m.parts.back().emote) {
		std::string &t = m.parts.back().text;
		while (!t.empty() && t.back() == ' ')
			t.pop_back();
	}
	// total text budget across parts, in code points
	size_t budget = limits_.max_text;
	std::vector<ChatPart> final_parts;
	for (size_t i = 0; i < m.parts.size(); ++i) {
		ChatPart p = m.parts[i];
		if (!p.emote) {
			p.text = sanitize_chat_text(p.text, budget, false);
			size_t cps = 0;
			for (size_t k = 0; k < p.text.size(); ++k)
				if ((static_cast<unsigned char>(p.text[k]) & 0xC0) != 0x80)
					++cps;
			budget = cps >= budget ? 0 : budget - cps;
			if (p.text.empty())
				continue;
		}
		final_parts.push_back(p);
	}
	m.parts.swap(final_parts);
	if (m.parts.empty()) {
		++res.invalid;
		return;
	}

	const json::Value *bd = obj.find("badges");
	if (bd && bd->is_array()) {
		for (size_t i = 0; i < bd->size() && m.badges.size() < limits_.max_badges; ++i) {
			if (!bd->at(i).is_string())
				continue;
			std::string b = sanitize_chat_text(bd->at(i).as_string(), 24);
			if (!b.empty())
				m.badges.push_back(b);
		}
	}

	// plain text for filters
	std::string plain;
	for (size_t i = 0; i < m.parts.size(); ++i) {
		if (!plain.empty())
			plain += ' ';
		plain += m.parts[i].text;
	}
	std::string plain_l = ascii_lower(plain);
	std::string author_l = ascii_lower(m.author);

	// duplicates by (platform, id)
	std::string key = platform + std::string(1, '\x1f') + id;
	if (seen_.count(key)) {
		++res.duplicates;
		++total_dups_;
		return;
	}
	seen_.insert(key);
	seen_order_.push_back(key);
	while (seen_order_.size() > limits_.dedupe_keep) {
		seen_.erase(seen_order_.front());
		seen_order_.pop_front();
	}

	// filters
	bool blocked = false;
	for (size_t i = 0; i < filter_.users.size() && !blocked; ++i)
		blocked = (filter_.users[i] == author_l);
	for (size_t i = 0; i < filter_.words.size() && !blocked; ++i)
		blocked = plain_l.find(filter_.words[i]) != std::string::npos;
	if (!blocked && filter_.hide_commands && plain.size() >= 2 && (plain[0] == '!' || plain[0] == '/') && plain[1] != ' ')
		blocked = true;
	if (!blocked && filter_.hide_bots && is_known_bot_name(author_l))
		blocked = true;

	if (sit == sources_.end()) {
		SourceStatus s;
		s.id = source;
		s.platform = platform;
		s.state = "reading";
		s.updated_ms = now;
		sit = sources_.insert(std::make_pair(source, s)).first;
	}
	if (blocked) {
		++res.blocked;
		++total_blocked_;
		++sit->second.dropped;
		return;
	}

	// per-source token bucket
	Bucket &b = buckets_[source];
	if (b.last_ms == 0) {
		b.tokens = limits_.rate_burst;
		b.last_ms = now > 0 ? now : 1;
	}
	double dt = static_cast<double>(now - b.last_ms) / 1000.0;
	if (dt > 0) {
		b.tokens = std::min<double>(limits_.rate_burst, b.tokens + dt * limits_.rate_per_sec);
		b.last_ms = now;
	}
	if (b.tokens < 1.0) {
		++res.rate_limited;
		++total_rate_;
		++sit->second.dropped;
		return;
	}
	b.tokens -= 1.0;

	int64_t age = 0;
	const json::Value *tv = obj.find("t");
	if (have_sent && tv && tv->is_number()) {
		double d = static_cast<double>(sent_ms) - tv->as_number();
		if (d > 0)
			age = static_cast<int64_t>(std::min<double>(d, limits_.max_age_ms));
	}
	m.arrival_ms = now;
	m.order_ms = now - age;
	m.arrival_idx = ++arrival_counter_;
	pending_.push_back(m);
	++res.accepted;
	++total_accepted_;
	++sit->second.accepted;
	if (sit->second.state == "starting" || sit->second.state.empty())
		sit->second.state = "reading";
	sit->second.updated_ms = now;
}

IngestResult ChatHub::ingest(const std::string &body)
{
	IngestResult res;
	if (body.size() > limits_.max_body) {
		res.http_status = 413;
		res.error = "body too large";
		return res;
	}
	json::ParseResult pr = json::parse(body, limits_.max_body, 8);
	if (!pr.ok || !pr.value.is_object()) {
		res.http_status = 400;
		res.error = "invalid JSON";
		return res;
	}
	const json::Value &root = pr.value;
	const json::Value *ver = root.find("v");
	if (!ver || !ver->is_number() || ver->as_int() != 1) {
		res.http_status = 400;
		res.error = "unsupported version";
		return res;
	}
	const json::Value *msgs = root.find("messages");
	const json::Value *srcs = root.find("sources");
	if ((msgs && !msgs->is_array()) || (srcs && !srcs->is_array())) {
		res.http_status = 400;
		res.error = "messages and sources must be lists";
		return res;
	}
	if (msgs && msgs->size() > limits_.max_batch) {
		res.http_status = 413;
		res.error = "too many messages in one batch";
		return res;
	}
	if (srcs && srcs->size() > limits_.max_sources) {
		res.http_status = 413;
		res.error = "too many sources";
		return res;
	}
	const json::Value *sent = root.find("sent");
	bool have_sent = sent && sent->is_number();
	int64_t sent_ms = have_sent ? sent->as_int() : 0;

	int64_t now = clock_.now_ms();
	std::lock_guard<std::mutex> lk(m_);
	if (down_) {
		res.http_status = 503;
		res.error = "shutting down";
		return res;
	}
	if (srcs)
		for (size_t i = 0; i < srcs->size(); ++i)
			accept_status(srcs->at(i), now);
	if (msgs)
		for (size_t i = 0; i < msgs->size(); ++i)
			accept_message(msgs->at(i), sent_ms, have_sent, now, res);
	total_invalid_ += static_cast<uint64_t>(res.invalid);
	if (limits_.reorder_ms <= 0)
		flush_locked(now);
	return res;
}

void ChatHub::flush_locked(int64_t now)
{
	std::vector<ChatMessage> due, keep;
	for (size_t i = 0; i < pending_.size(); ++i) {
		if (pending_[i].arrival_ms + limits_.reorder_ms <= now)
			due.push_back(pending_[i]);
		else
			keep.push_back(pending_[i]);
	}
	if (due.empty())
		return;
	std::sort(due.begin(), due.end(), [](const ChatMessage &a, const ChatMessage &b) {
		if (a.order_ms != b.order_ms)
			return a.order_ms < b.order_ms;
		return a.arrival_idx < b.arrival_idx;
	});
	for (size_t i = 0; i < due.size(); ++i) {
		due[i].seq = ++seq_;
		ring_.push_back(due[i]);
		while (ring_.size() > limits_.ring)
			ring_.pop_front();
	}
	pending_.swap(keep);
	cv_.notify_all();
}

void ChatHub::flush()
{
	int64_t now = clock_.now_ms();
	std::lock_guard<std::mutex> lk(m_);
	flush_locked(now);
}

uint64_t ChatHub::last_seq() const
{
	std::lock_guard<std::mutex> lk(m_);
	return seq_;
}

bool ChatHub::wait_for(uint64_t after, int timeout_ms, std::vector<ChatMessage> &out, size_t max)
{
	out.clear();
	std::unique_lock<std::mutex> lk(m_);
	cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return down_ || seq_ > after; });
	if (down_ || seq_ <= after)
		return false;
	for (size_t i = 0; i < ring_.size() && out.size() < max; ++i)
		if (ring_[i].seq > after)
			out.push_back(ring_[i]);
	return !out.empty();
}

std::vector<ChatMessage> ChatHub::backlog(size_t n) const
{
	std::lock_guard<std::mutex> lk(m_);
	std::vector<ChatMessage> out;
	size_t from = ring_.size() > n ? ring_.size() - n : 0;
	for (size_t i = from; i < ring_.size(); ++i)
		out.push_back(ring_[i]);
	return out;
}

std::vector<SourceStatus> ChatHub::sources() const
{
	std::lock_guard<std::mutex> lk(m_);
	std::vector<SourceStatus> out;
	for (std::map<std::string, SourceStatus>::const_iterator it = sources_.begin(); it != sources_.end(); ++it)
		out.push_back(it->second);
	return out;
}

void ChatHub::mark_all_sources(const std::string &state, const std::string &detail)
{
	int64_t now = clock_.now_ms();
	std::lock_guard<std::mutex> lk(m_);
	for (std::map<std::string, SourceStatus>::iterator it = sources_.begin(); it != sources_.end(); ++it) {
		it->second.state = state;
		it->second.detail = detail;
		it->second.updated_ms = now;
	}
}

std::string ChatHub::status_json() const
{
	int64_t now = clock_.now_ms();
	std::lock_guard<std::mutex> lk(m_);
	json::Value o = json::Value::object();
	o.set("v", json::Value::number(1));
	json::Value arr = json::Value::array();
	for (std::map<std::string, SourceStatus>::const_iterator it = sources_.begin(); it != sources_.end(); ++it) {
		json::Value s = json::Value::object();
		s.set("id", json::Value::string(it->second.id));
		s.set("platform", json::Value::string(it->second.platform));
		s.set("state", json::Value::string(it->second.state));
		s.set("detail", json::Value::string(it->second.detail));
		s.set("accepted", json::Value::number(static_cast<double>(it->second.accepted)));
		s.set("dropped", json::Value::number(static_cast<double>(it->second.dropped)));
		s.set("age_ms", json::Value::number(static_cast<double>(now - it->second.updated_ms)));
		arr.push(s);
	}
	o.set("sources", arr);
	json::Value c = json::Value::object();
	c.set("accepted", json::Value::number(static_cast<double>(total_accepted_)));
	c.set("duplicates", json::Value::number(static_cast<double>(total_dups_)));
	c.set("blocked", json::Value::number(static_cast<double>(total_blocked_)));
	c.set("rate_limited", json::Value::number(static_cast<double>(total_rate_)));
	c.set("invalid", json::Value::number(static_cast<double>(total_invalid_)));
	o.set("counts", c);
	return json::dump(o, false);
}

void ChatHub::shutdown()
{
	std::lock_guard<std::mutex> lk(m_);
	down_ = true;
	cv_.notify_all();
}

bool ChatHub::is_shutdown() const
{
	std::lock_guard<std::mutex> lk(m_);
	return down_;
}

} // namespace oms
