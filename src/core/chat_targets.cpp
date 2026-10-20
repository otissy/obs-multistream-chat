#include "chat_targets.hpp"

#include <cctype>
#include <initializer_list>
#include <set>

namespace oms {

namespace {

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

bool all_of_class(const std::string &s, const char *extra, size_t lo, size_t hi)
{
	if (s.size() < lo || s.size() > hi)
		return false;
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c >= 0x80)
			return false;
		bool ok = std::isalnum(c) != 0;
		for (const char *e = extra; !ok && *e; ++e)
			ok = (c == static_cast<unsigned char>(*e));
		if (!ok)
			return false;
	}
	return true;
}

std::vector<std::string> split(const std::string &s, char sep)
{
	std::vector<std::string> out;
	size_t from = 0;
	while (true) {
		size_t p = s.find(sep, from);
		out.push_back(s.substr(from, p == std::string::npos ? std::string::npos : p - from));
		if (p == std::string::npos)
			break;
		from = p + 1;
	}
	return out;
}

struct Url {
	std::string host;
	std::vector<std::string> segs; // non-empty path segments
	std::string query;
};

// Accepts "https://host/path?query#frag" or "host/path" (no scheme). Anything else: false.
bool parse_url(const std::string &in, Url &u)
{
	std::string s = in;
	std::string ls = lower(s);
	if (ls.compare(0, 8, "https://") == 0)
		s = s.substr(8);
	else if (ls.compare(0, 7, "http://") == 0)
		return false; // chat pages are https only
	else if (s.find("://") != std::string::npos)
		return false;
	size_t hash = s.find('#');
	if (hash != std::string::npos)
		s = s.substr(0, hash);
	size_t qm = s.find('?');
	std::string query;
	if (qm != std::string::npos) {
		query = s.substr(qm + 1);
		s = s.substr(0, qm);
	}
	size_t slash = s.find('/');
	std::string host = slash == std::string::npos ? s : s.substr(0, slash);
	std::string path = slash == std::string::npos ? std::string() : s.substr(slash);
	if (host.empty())
		return false;
	for (size_t i = 0; i < host.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(host[i]);
		if (!(std::isalnum(c) || c == '.' || c == '-') || c >= 0x80)
			return false; // rejects userinfo "@", ports ":", unicode, spaces
	}
	if (host.front() == '.' || host.back() == '.')
		return false;
	for (size_t i = 0; i < path.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(path[i]);
		if (c <= 0x20 || c == 0x7f || c == '\\' || c == '%')
			return false; // no encoded tricks in the few paths we understand
	}
	u.host = lower(host);
	u.query = query;
	u.segs.clear();
	std::vector<std::string> parts = split(path, '/');
	for (size_t i = 0; i < parts.size(); ++i)
		if (!parts[i].empty())
			u.segs.push_back(parts[i]);
	return true;
}

bool host_in(const std::string &h, std::initializer_list<const char *> list)
{
	for (const char *x : list)
		if (h == x)
			return true;
	return false;
}

std::string query_param(const std::string &q, const std::string &key)
{
	std::vector<std::string> kv = split(q, '&');
	for (size_t i = 0; i < kv.size(); ++i) {
		size_t eq = kv[i].find('=');
		if (eq != std::string::npos && kv[i].substr(0, eq) == key)
			return kv[i].substr(eq + 1);
	}
	return std::string();
}

bool twitch_name(const std::string &s) { return all_of_class(s, "_", 3, 25); }
bool kick_name(const std::string &s) { return all_of_class(s, "_-", 3, 25); }
bool tiktok_name(const std::string &s) { return all_of_class(s, "_.", 2, 24) && s.back() != '.'; }
bool youtube_id(const std::string &s) { return all_of_class(s, "_-", 11, 11); }
// YouTube channel handle without the @: 3-30 of a-z 0-9 _ - . , not starting or ending with a dot
bool youtube_handle(const std::string &s) { return all_of_class(s, "_-.", 3, 30) && s.front() != '.' && s.back() != '.'; }
// YouTube channel id: UC + 22 of A-Z a-z 0-9 _ -
bool youtube_channel_id(const std::string &s) { return s.size() == 24 && s.compare(0, 2, "UC") == 0 && all_of_class(s, "_-", 24, 24); }
bool has_digit_or_dash(const std::string &s) { return s.find_first_of("0123456789_-") != std::string::npos; }

// "@handle" (lower case) or false + err
bool youtube_channel(const std::string &handle, std::string &target, std::string &err)
{
	if (!youtube_handle(handle)) {
		err = "that is not a valid YouTube channel handle (@name)";
		return false;
	}
	target = "@" + lower(handle);
	return true;
}
bool x_id(const std::string &s) { return all_of_class(s, "_-", 8, 40); }

// TikTok names may contain dots, so a dot alone does not make a TikTok input a link.
bool looks_like_url(const std::string &platform, const std::string &s)
{
	return s.find('/') != std::string::npos || (platform != "tiktok" && s.find('.') != std::string::npos) ||
	       s.find(':') != std::string::npos || s.find('?') != std::string::npos;
}

} // namespace

const std::vector<ChatPlatform> &chat_platforms()
{
	static const std::vector<ChatPlatform> v = {
		{"twitch", "Twitch", false, "channel name, or a twitch.tv link"},
		{"youtube", "YouTube", false, "channel as @name (the live stream is found by itself), or a video id or link"},
		{"kick", "Kick", false, "channel name, or a kick.com link"},
		{"tiktok", "TikTok (experimental)", true, "@username, or a tiktok.com live link"},
		{"x", "X (experimental)", true, "broadcast id, or an x.com/i/broadcasts/... link"}};
	return v;
}

const ChatPlatform *find_chat_platform(const std::string &id)
{
	const std::vector<ChatPlatform> &v = chat_platforms();
	for (size_t i = 0; i < v.size(); ++i)
		if (id == v[i].id)
			return &v[i];
	return nullptr;
}

bool normalize_chat_target(const std::string &platform, const std::string &input_raw, std::string &target, std::string &err)
{
	target.clear();
	err.clear();
	if (!find_chat_platform(platform)) {
		err = "unknown chat platform";
		return false;
	}
	std::string input = trim(input_raw);
	if (input.empty() || input.size() > 300) {
		err = "enter a channel, video id or link";
		return false;
	}
	for (size_t i = 0; i < input.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(input[i]);
		if (c < 0x20 || c == 0x7f) {
			err = "control characters are not allowed";
			return false;
		}
	}
	std::string cand;
	// YouTube: a channel (@handle, or a UC... channel id) is found live by the reader; an 11-character id is one video
	if (platform == "youtube" && input[0] == '@' && input.find('/') == std::string::npos)
		return youtube_channel(input.substr(1), target, err);
	bool is_url = looks_like_url(platform, input);
	if (!is_url) {
		cand = input;
		if (platform == "tiktok" && !cand.empty() && cand[0] == '@')
			cand = cand.substr(1);
		if (platform == "youtube") {
			if (youtube_channel_id(cand)) {
				target = cand;
				return true;
			}
			// bare 11 characters with a digit, - or _ is a video id; anything else typed bare is a channel name
			// ("RoyalAscent" is 11 letters: a channel, not a video)
			if (!(youtube_id(cand) && has_digit_or_dash(cand)))
				return youtube_channel(cand, target, err);
		}
	} else {
		Url u;
		if (!parse_url(input, u)) {
			err = "that link is not acceptable (https links to the platform only)";
			return false;
		}
		if (platform == "twitch") {
			if (!host_in(u.host, {"twitch.tv", "www.twitch.tv", "m.twitch.tv"}) || u.segs.empty()) {
				err = "not a twitch.tv link";
				return false;
			}
			cand = (u.segs[0] == "popout" && u.segs.size() >= 2) ? u.segs[1] : u.segs[0];
		} else if (platform == "kick") {
			if (!host_in(u.host, {"kick.com", "www.kick.com"}) || u.segs.empty()) {
				err = "not a kick.com link";
				return false;
			}
			cand = (u.segs[0] == "popout" && u.segs.size() >= 2) ? u.segs[1] : u.segs[0];
		} else if (platform == "youtube") {
			if (u.host == "youtu.be") {
				if (u.segs.empty()) {
					err = "no video id in that link";
					return false;
				}
				cand = u.segs[0];
			} else if (host_in(u.host, {"youtube.com", "www.youtube.com", "m.youtube.com"}) && !u.segs.empty() &&
				   !u.segs[0].empty() && u.segs[0][0] == '@') {
				return youtube_channel(u.segs[0].substr(1), target, err); // youtube.com/@name, /@name/live ...
			} else if (host_in(u.host, {"youtube.com", "www.youtube.com", "m.youtube.com"}) && u.segs.size() >= 2 &&
				   u.segs[0] == "channel") {
				if (!youtube_channel_id(u.segs[1])) {
					err = "that YouTube channel link has no valid channel id (UC...)";
					return false;
				}
				target = u.segs[1]; // youtube.com/channel/UC...
				return true;
			} else if (host_in(u.host, {"youtube.com", "www.youtube.com", "m.youtube.com"})) {
				if (!u.segs.empty() && (u.segs[0] == "watch" || u.segs[0] == "live_chat"))
					cand = query_param(u.query, "v");
				else if (u.segs.size() >= 2 && (u.segs[0] == "live" || u.segs[0] == "embed"))
					cand = u.segs[1];
				if (cand.empty()) {
					err = "that YouTube link has no video id (a channel link is not enough: use the live video link)";
					return false;
				}
			} else {
				err = "not a youtube.com link";
				return false;
			}
		} else if (platform == "tiktok") {
			if (!host_in(u.host, {"tiktok.com", "www.tiktok.com"}) || u.segs.empty() || u.segs[0].empty() ||
			    u.segs[0][0] != '@') {
				err = "not a tiktok.com/@user link";
				return false;
			}
			cand = u.segs[0].substr(1);
		} else if (platform == "x") {
			if (!host_in(u.host, {"x.com", "www.x.com", "twitter.com", "www.twitter.com", "mobile.twitter.com"}) ||
			    u.segs.size() < 3 || u.segs[0] != "i" || u.segs[1] != "broadcasts") {
				err = "not an x.com/i/broadcasts/... link";
				return false;
			}
			cand = u.segs[2];
		}
	}
	bool ok = false;
	if (platform == "twitch")
		ok = twitch_name(cand);
	else if (platform == "kick")
		ok = kick_name(cand);
	else if (platform == "youtube")
		ok = youtube_id(cand);
	else if (platform == "tiktok")
		ok = tiktok_name(cand);
	else if (platform == "x")
		ok = x_id(cand);
	if (!ok) {
		err = std::string("that is not a valid ") + find_chat_platform(platform)->label + " name or id";
		return false;
	}
	target = (platform == "twitch" || platform == "kick" || platform == "tiktok") ? lower(cand) : cand;
	return true;
}

std::string chat_page_url(const std::string &platform, const std::string &target)
{
	std::string t, err;
	if (!normalize_chat_target(platform, target, t, err) || t != target)
		return std::string();
	if (platform == "twitch")
		return "https://www.twitch.tv/popout/" + t + "/chat";
	if (platform == "youtube" && t[0] == '@')
		return "https://www.youtube.com/" + t + "/live"; // the channel: the reader finds the live video there
	if (platform == "youtube" && youtube_channel_id(t))
		return "https://www.youtube.com/channel/" + t + "/live";
	if (platform == "youtube")
		return "https://www.youtube.com/live_chat?is_popout=1&v=" + t;
	if (platform == "kick")
		return "https://kick.com/popout/" + t + "/chat";
	if (platform == "tiktok")
		return "https://www.tiktok.com/@" + t + "/live";
	if (platform == "x")
		return "https://x.com/i/broadcasts/" + t;
	return std::string();
}

const std::vector<std::string> &chat_emote_hosts(const std::string &platform)
{
	static const std::vector<std::string> twitch = {
		"static-cdn.jtvnw.net", "cdn.7tv.app", "cdn.betterttv.net", "cdn.frankerfacez.com"
	};
	static const std::vector<std::string> youtube = {
		"yt3.ggpht.com", "yt3.googleusercontent.com", "www.gstatic.com", "www.youtube.com", "cdn.7tv.app"
	};
	static const std::vector<std::string> kick = {
		"files.kick.com", "cdn.7tv.app", "cdn.betterttv.net", "cdn.frankerfacez.com"
	};
	static const std::vector<std::string> tiktok = {"*.tiktokcdn.com", "*.tiktokcdn-us.com", "*.ibyteimg.com"};
	static const std::vector<std::string> x = {"*.twimg.com"};
	static const std::vector<std::string> none;
	if (platform == "twitch")
		return twitch;
	if (platform == "youtube")
		return youtube;
	if (platform == "kick")
		return kick;
	if (platform == "tiktok")
		return tiktok;
	if (platform == "x")
		return x;
	return none;
}

bool emote_url_allowed(const std::string &platform, const std::string &url)
{
	if (url.size() < 12 || url.size() > 512 || url.compare(0, 8, "https://") != 0)
		return false;
	for (size_t i = 0; i < url.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(url[i]);
		if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`' || c == '(' ||
		    c == ')')
			return false;
	}
	size_t end = url.find_first_of("/?#", 8);
	std::string host = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
	if (host.empty())
		return false;
	for (size_t i = 0; i < host.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(host[i]);
		if (!(std::isalnum(c) || c == '.' || c == '-'))
			return false; // userinfo, port, ipv6, unicode
	}
	host = lower(host);
	if (host.front() == '.' || host.back() == '.' || host.find("..") != std::string::npos)
		return false;
	const std::vector<std::string> &allowed = chat_emote_hosts(platform);
	for (size_t i = 0; i < allowed.size(); ++i) {
		const std::string &a = allowed[i];
		if (a[0] == '*') {
			std::string suffix = a.substr(1); // ".twimg.com"
			if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
				return true;
		} else if (host == a) {
			// www.youtube.com is allowed only for its own emoji path, not the whole host
			if (host == "www.youtube.com" && url.compare(8 + host.size(), 16, "/s/gaming/emoji/") != 0)
				continue;
			return true;
		}
	}
	return false;
}

std::string chat_img_src_csp()
{
	std::set<std::string> hosts;
	const char *plats[] = {"twitch", "youtube", "kick", "tiktok", "x"};
	for (size_t p = 0; p < 5; ++p) {
		const std::vector<std::string> &v = chat_emote_hosts(plats[p]);
		for (size_t i = 0; i < v.size(); ++i)
			hosts.insert("https://" + v[i] + (v[i] == "www.youtube.com" ? "/s/gaming/emoji/" : ""));
	}
	std::string out = "'self' data:";
	for (std::set<std::string>::const_iterator it = hosts.begin(); it != hosts.end(); ++it)
		out += " " + *it;
	return out;
}

} // namespace oms
