// The five chat platforms and what a user may point the reader at.
//   - a "target" is a bare channel / video / broadcast id, never a free URL
//   - normalize_chat_target() accepts either a bare id or a URL on that platform's own host(s)
//     and returns the bare id; the page URL is then built from a fixed template
//   - so a user (or a hand-edited config) cannot aim the reader at an arbitrary site
// The same rules are implemented in reader/lib/targets.js; .tests/chat/vectors/targets.json is
// checked by both.
#pragma once
#include <string>
#include <vector>

namespace oms {

struct ChatPlatform {
	const char *id;    // twitch, youtube, kick, tiktok, x
	const char *label; // dock label
	bool experimental; // tiktok, x
	const char *hint;  // what to enter
};

const std::vector<ChatPlatform> &chat_platforms();
const ChatPlatform *find_chat_platform(const std::string &id);

// false + err (a short sentence for the dock) when the input is not acceptable.
bool normalize_chat_target(const std::string &platform, const std::string &input, std::string &target, std::string &err);

// The page the reader opens, from an already normalized target ("" when invalid).
std::string chat_page_url(const std::string &platform, const std::string &target);

// Hosts the emote CDN allow-list accepts for a platform: exact host, or "*.suffix".
const std::vector<std::string> &chat_emote_hosts(const std::string &platform);
// true when url is https, has no credentials/port/odd characters and its host is on the platform's list.
bool emote_url_allowed(const std::string &platform, const std::string &url);
// Space separated CSP img-src source list covering every platform's emote hosts.
std::string chat_img_src_csp();

} // namespace oms
