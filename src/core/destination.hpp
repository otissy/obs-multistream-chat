// One place a stream is sent to.
#pragma once
#include <string>
#include <vector>

namespace oms {

// One flat libobs encoder setting kept verbatim (e.g. "bf", "lookahead", "tune" from Aitum).
struct ExtraSetting {
	std::string key;   // [a-z0-9_]{1,40}
	char type = 's';   // 'i' int, 'd' double, 'b' bool, 's' string
	std::string value; // textual: "2", "0.5", "true"/"false", or the string
};

struct Destination {
	std::string id;       // opaque, [A-Za-z0-9_-]{1,32}, stable across edits
	std::string name;     // label in the dock
	std::string platform; // preset id: twitch, youtube, facebook, kick, tiktok, trovo, custom
	std::string url;      // rtmp:// or rtmps:// ingest, WITHOUT the key
	std::string handle;   // shown on stream next to the icon
	bool enabled = true;
	std::string encoder = "shared"; // "shared" (main stream's video encoder) or "own" (own video encoder)
	int bitrate_kbps = 0;           // own mode: video bitrate. shared mode: ignored (0).
	bool auto_start = true;         // start with the main stream

	// --- since schema 4 -------------------------------------------------------------------
	// Video source. "" = the main canvas; otherwise the NAME of an OBS canvas (Aitum creates
	// them). A non-main canvas needs its own video encoder, so encoder must be "own".
	std::string canvas;
	// Own video encoder (encoder == "own"). Defaults reproduce the 0.2.0 "x264" mode exactly.
	std::string video_encoder = "obs_x264"; // libobs encoder id; installed ids are checked at launch
	int keyint_sec = 2;                     // 0 = encoder default
	std::string preset = "veryfast";        // "" = encoder default
	std::string profile;                    // "" = encoder default
	std::string rate_control = "CBR";       // "" = encoder default
	int out_width = 0;                      // 0 + 0 = canvas size (no rescale); else both 2..8192, even
	int out_height = 0;
	int scale_type = 3;                     // obs_scale_type: 0 disable 1 point 2 bicubic 3 bilinear 4 lanczos 5 area
	int fps_div = 1;                        // 1..4: 60 fps with 2 -> 30 fps
	std::vector<ExtraSetting> venc_extra;   // further flat encoder settings (imported from Aitum)
	// Audio. 0 = as the main stream; 1..6 = OBS mixer track (Advanced Audio Properties).
	int audio_track = 0;
	int audio_bitrate_kbps = 160; // own audio encoder (AAC); ignored when the main encoder is reused
	// Stream delay for this output only (libobs obs_output_set_delay). Needs a start to apply.
	int delay_sec = 0;
	bool delay_preserve = false;
	// "Use Aitum Stream Suite output settings": off | copy | follow (see aitum.hpp).
	std::string aitum_mode = "off";
	std::string aitum_output; // name of the Aitum output

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
// [A-Za-z0-9_.-]{1,max}: encoder ids, presets, profiles.
bool is_valid_token(const std::string &s, size_t max);
// True when this destination takes the main stream's video encoder (so it needs the main stream).
inline bool uses_main_video(const Destination &d) { return d.encoder == "shared"; }
bool has_control_chars(const std::string &s);

// "d" + 8 hex chars from a seed (caller supplies randomness; the core stays deterministic).
std::string make_id(unsigned seed);

constexpr int kMaxDestinations = 16;
constexpr size_t kMaxExtraSettings = 32;
constexpr int kMaxAudioTracks = 6;

} // namespace oms
