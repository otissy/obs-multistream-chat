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
		{"kick", "Kick", "", "kick",
		 "Kick Creator Dashboard > Stream: copy both the Stream URL and the Stream key."},
		{"tiktok", "TikTok", "", "tiktok",
		 "TikTok LIVE Studio / Live access: copy both the Server URL and the Stream key."},
		{"x", "X (Twitter)", "rtmps://prod-fastly-us-east-1.video.pscp.tv:443/live", "x",
		 "X Media Studio / Live Producer: copy Stream Key."},
		{"instagram", "Instagram Live", "", "generic",
		 "Instagram Live Producer: copy the Stream URL and Stream key."},
		{"facebook", "Facebook", "rtmps://live-api-s.facebook.com:443/rtmp/", "facebook",
		 "Facebook Live Producer > Streaming software: Stream key (expires; make a persistent key)."},
		{"restream", "Restream.io", "rtmp://live.restream.io/live", "generic",
		 "Restream Dashboard: copy the RTMP URL and Stream Key."},
		{"trovo", "Trovo", "", "generic",
		 "Trovo Creator Studio: copy the RTMP server and the key."},
		{"linkedin", "LinkedIn Live", "rtmps://live-api-s.linkedin.com:443/rtmp/", "generic",
		 "LinkedIn Live: copy Stream URL and Stream key."},
		{"custom", "Custom RTMP", "", "generic",
		 "Any RTMP or RTMPS server: URL and key from your provider."},
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

bool is_valid_token(const std::string &s, size_t max)
{
	if (s.empty() || s.size() > max)
		return false;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-'))
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
	if (d.encoder != "shared" && d.encoder != "own")
		out.push_back({"encoder", "Encoder must be shared or own."});
	if (d.bitrate_kbps < 0 || d.bitrate_kbps > 51000)
		out.push_back({"bitrate_kbps", "Bitrate must be 0-51000 kbps."});
	if (d.encoder == "own") {
		if (d.bitrate_kbps < 300 || d.bitrate_kbps > 51000)
			out.push_back({"bitrate_kbps", "An own encoder needs a bitrate of 300-51000 kbps."});
		if (!is_valid_token(d.video_encoder, 64))
			out.push_back({"video_encoder", "Video encoder id is missing or malformed."});
		if (d.keyint_sec < 0 || d.keyint_sec > 10)
			out.push_back({"keyint_sec", "Keyframe interval must be 0-10 seconds."});
		if (!d.preset.empty() && !is_valid_token(d.preset, 32))
			out.push_back({"preset", "Preset is malformed."});
		if (!d.profile.empty() && !is_valid_token(d.profile, 32))
			out.push_back({"profile", "Profile is malformed."});
		if (!d.rate_control.empty() && !is_valid_token(d.rate_control, 32))
			out.push_back({"rate_control", "Rate control is malformed."});
		bool w0 = d.out_width == 0, h0 = d.out_height == 0;
		if (w0 != h0 || (!w0 && (d.out_width < 2 || d.out_width > 8192 || d.out_height < 2 || d.out_height > 8192 ||
					 (d.out_width % 2) != 0 || (d.out_height % 2) != 0)))
			out.push_back({"out_width", "Rescale needs both width and height, even, 2-8192 (or both 0)."});
		if (d.scale_type < 0 || d.scale_type > 5)
			out.push_back({"scale_type", "Scale type must be 0-5."});
		if (d.fps_div < 1 || d.fps_div > 4)
			out.push_back({"fps_div", "Frame-rate divisor must be 1-4."});
		if (d.venc_extra.size() > kMaxExtraSettings)
			out.push_back({"venc_extra", "Too many extra encoder settings."});
		for (size_t i = 0; i < d.venc_extra.size() && i < kMaxExtraSettings; ++i) {
			const ExtraSetting &e = d.venc_extra[i];
			bool key_ok = !e.key.empty() && e.key.size() <= 40;
			for (size_t k = 0; key_ok && k < e.key.size(); ++k) {
				unsigned char c = static_cast<unsigned char>(e.key[k]);
				if (!(std::islower(c) || std::isdigit(c) || c == '_'))
					key_ok = false;
			}
			bool type_ok = e.type == 'i' || e.type == 'd' || e.type == 'b' || e.type == 's';
			if (!key_ok || !type_ok || e.value.size() > 128 || has_control_chars(e.value)) {
				out.push_back({"venc_extra", "An extra encoder setting is malformed."});
				break;
			}
		}
	}
	if (!d.canvas.empty()) {
		if (d.canvas.size() > 128 || has_control_chars(d.canvas))
			out.push_back({"canvas", "Canvas name is at most 128 bytes with no control characters."});
		if (d.encoder != "own")
			out.push_back({"canvas", "A destination on another canvas needs its own video encoder."});
	}
	if (d.audio_track < 0 || d.audio_track > kMaxAudioTracks)
		out.push_back({"audio_track", "Audio track must be 0 (as the main stream) or 1-6."});
	if (d.audio_bitrate_kbps < 64 || d.audio_bitrate_kbps > 320)
		out.push_back({"audio_bitrate_kbps", "Audio bitrate must be 64-320 kbps."});
	if (d.delay_sec < 0 || d.delay_sec > 3600)
		out.push_back({"delay_sec", "Delay must be 0-3600 seconds."});
	if (d.aitum_mode != "off" && d.aitum_mode != "copy" && d.aitum_mode != "follow")
		out.push_back({"aitum_mode", "Aitum mode must be off, copy or follow."});
	if (d.aitum_output.size() > 128 || has_control_chars(d.aitum_output))
		out.push_back({"aitum_output", "Aitum output name is at most 128 bytes with no control characters."});
	if (d.aitum_mode == "follow" && d.aitum_output.empty())
		out.push_back({"aitum_output", "Follow mode needs the name of an Aitum output."});
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
