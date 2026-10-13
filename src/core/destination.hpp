// One place a stream is sent to.
#pragma once
#include <string>
#include <vector>

namespace oms {

struct Destination {
	std::string id;       // opaque, [A-Za-z0-9_-]{1,32}, stable across edits
	std::string name;     // label in the dock
	std::string platform; // preset id: twitch, youtube, facebook, kick, tiktok, trovo, custom
	std::string url;      // rtmp:// or rtmps:// ingest, WITHOUT the key
	std::string handle;   // shown on stream next to the icon
	bool enabled = true;
	std::string encoder = "shared"; // "shared" (main stream's encoders) or "x264" (own encode)
	int bitrate_kbps = 0;           // x264 mode: video bitrate. shared mode: ignored (0).
	bool auto_start = true;         // start with the main stream

	// Key handling. key_token is what is stored (encrypted). key is plaintext and exists only
	// transiently: set by the edit dialog, consumed by seal_key(), then wiped.
	std::string key_token;
	std::string key;
};

struct PlatformPreset {
	const char *id;
	const char *label;
	const char *default_url; // may be "": the platform gives each account its own ingest
	const char *icon;        // file name under /icons/ without extension; "generic" if none bundled
	const char *url_hint;    // where the user finds url + key (dock tooltip)
};

const std::vector<PlatformPreset> &platform_presets();
const PlatformPreset *find_preset(const std::string &id);

struct ValidationIssue {
	std::string field;
	std::string message;
};

// require_key: demand key or key_token to be present (false when editing and keeping the old key).
std::vector<ValidationIssue> validate_destination(const Destination &d, bool require_key = true);

bool is_valid_id(const std::string &id);
bool has_control_chars(const std::string &s);

// "d" + 8 hex chars from a seed (caller supplies randomness; the core stays deterministic).
std::string make_id(unsigned seed);

constexpr int kMaxDestinations = 16;

} // namespace oms
