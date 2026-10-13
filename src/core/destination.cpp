#include "destination.hpp"

#include <cctype>
#include <cstdio>

namespace oms {

const std::vector<PlatformPreset> &platform_presets()
{
	// Default URLs are the platforms' long-standing public ingest hosts. Where an account gets a
	// personal or regional ingest (Kick, TikTok) the default is empty and the dialog says where
	// to copy it from. Always double-check against the platform's dashboard.
	static const std::vector<PlatformPreset> presets = {
		{"twitch", "Twitch", "rtmp://live.twitch.tv/app", "twitch",
		 "Twitch Creator Dashboard > Settings > Stream: Primary Stream key. RTMPS also works."},
		{"youtube", "YouTube", "rtmp://a.rtmp.youtube.com/live2", "youtube",
		 "YouTube Studio > Go live > Stream: Stream key. rtmps://a.rtmps.youtube.com:443/live2 also works."},
		{"facebook", "Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/", "facebook",
		 "Facebook Live Producer > Streaming software: Stream key (expires; make a persistent key)."},
		{"kick", "Kick", "", "kick",
		 "Kick Creator Dashboard > Stream: copy both the Stream URL and the Stream key."},
		{"tiktok", "TikTok", "", "tiktok",
		 "TikTok LIVE Studio / Live access: copy both the Server URL and the Stream key."},
		{"trovo", "Trovo", "", "generic", "Trovo Creator Studio: copy the RTMP server and the key."},
		{"custom", "Custom RTMP", "", "generic", "Any RTMP or RTMPS server: URL and key from your provider."},
	};
	return presets;
}

const PlatformPreset *find_preset(const std::string &id)
{
	const std::vector<PlatformPreset> &p = platform_presets();
	for (size_t i = 0; i < p.size(); ++i)
		if (id == p[i].id)
			return &p[i];
	return nullptr;
}

bool has_control_chars(const std::string &s)
{
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x20 || c == 0x7F)
			return true;
	}
	return false;
}

bool is_valid_id(const std::string &id)
{
	if (id.empty() || id.size() > 32)
		return false;
	for (size_t i = 0; i < id.size(); ++i) {
		char c = id[i];
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
			return false;
	}
	return true;
}

std::string make_id(unsigned seed)
{
	// xorshift scramble so consecutive seeds give unrelated-looking ids
	unsigned x = seed ? seed : 0x9E3779B9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	char buf[16];
	std::snprintf(buf, sizeof buf, "d%08x", x);
	return buf;
}

namespace {

bool starts_with_icase(const std::string &s, const char *prefix)
{
	size_t n = 0;
	while (prefix[n])
		++n;
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; ++i)
		if (std::tolower(static_cast<unsigned char>(s[i])) != prefix[i])
			return false;
	return true;
}

void check_url(const std::string &url, std::vector<ValidationIssue> &out)
{
	if (url.empty()) {
		out.push_back({"url", "RTMP URL is required."});
		return;
	}
	if (url.size() > 512) {
		out.push_back({"url", "URL is too long (max 512 characters)."});
		return;
	}
	if (!starts_with_icase(url, "rtmp://") && !starts_with_icase(url, "rtmps://")) {
		out.push_back({"url", "URL must start with rtmp:// or rtmps://."});
		return;
	}
	for (size_t i = 0; i < url.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(url[i]);
		if (c <= 0x20 || c == 0x7F) {
			out.push_back({"url", "URL must not contain spaces or control characters."});
			return;
		}
	}
	size_t host_start = url.find("://") + 3;
	size_t host_end = url.find_first_of("/?#", host_start);
	std::string authority = url.substr(host_start, host_end == std::string::npos ? std::string::npos
										      : host_end - host_start);
	if (authority.empty()) {
		out.push_back({"url", "URL has no host."});
		return;
	}
	if (authority.find('@') != std::string::npos) {
		out.push_back({"url", "Do not put a user name or password in the URL."});
		return;
	}
}

} // namespace

std::vector<ValidationIssue> validate_destination(const Destination &d, bool require_key)
{
	std::vector<ValidationIssue> out;
	if (!is_valid_id(d.id))
		out.push_back({"id", "Internal id is missing or malformed."});
	if (d.name.empty() || d.name.size() > 64 || has_control_chars(d.name))
		out.push_back({"name", "Name is required (1-64 characters, no control characters)."});
	if (!find_preset(d.platform))
		out.push_back({"platform", "Unknown platform."});
	check_url(d.url, out);
	if (d.handle.size() > 128 || has_control_chars(d.handle))
		out.push_back({"handle", "Handle is at most 128 bytes with no control characters."});
	if (d.encoder != "shared" && d.encoder != "x264")
		out.push_back({"encoder", "Encoder must be shared or x264."});
	if (d.bitrate_kbps < 0 || d.bitrate_kbps > 51000)
		out.push_back({"bitrate_kbps", "Bitrate must be 0-51000 kbps."});
	if (d.encoder == "x264" && (d.bitrate_kbps < 300 || d.bitrate_kbps > 51000))
		out.push_back({"bitrate_kbps", "x264 mode needs a bitrate of 300-51000 kbps."});
	if (!d.key.empty()) {
		if (d.key.size() > 512)
			out.push_back({"key", "Stream key is too long (max 512)."});
		for (size_t i = 0; i < d.key.size(); ++i) {
			unsigned char c = static_cast<unsigned char>(d.key[i]);
			if (c <= 0x20 || c == 0x7F) {
				out.push_back({"key", "Stream key must not contain spaces or control characters."});
				break;
			}
		}
	}
	if (require_key && d.key.empty() && d.key_token.empty())
		out.push_back({"key", "Stream key is required."});
	return out;
}

} // namespace oms
