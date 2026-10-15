// Reads (never writes) Aitum Stream Suite's per-profile output settings and maps one output entry
// onto our Destination model. Pure logic: text in, structs out, so it is tested with synthetic files.
//
// Verified against Aitum Stream Suite 1.2.4 (see PROJECT.md "Aitum Stream Suite 1.2.4"): the file is
// <profile folder>\aitum.json, it has NO version field, so the gate is structural. Anything that does
// not look like that shape fails soft with a message and never throws.
//
// SECRETS: AitumOutput::key may hold a stream key. It is wiped by wipe(); no message produced here
// ever contains a key or a server URL.
#pragma once
#include "destination.hpp"

#include <string>
#include <vector>

namespace oms {

constexpr size_t kMaxAitumFileBytes = 4u << 20;
constexpr const char *kAitumInspectedVersion = "1.2.4";

struct AitumCanvas {
	std::string name;
	int width = 0;
	int height = 0;
};

struct AitumOutput {
	std::string name;
	std::string canvas;
	bool enabled = false;
	// stream_server / stream_key: strings seen in the plugin binary, not in any inspected file.
	std::string server;
	std::string key; // SECRET
	bool from_unverified_keys = false;
	std::string video_encoder;
	int bitrate_kbps = 0;
	int keyint_sec = -1; // -1 = not set
	std::string preset, profile, rate_control;
	bool scale = false;
	int width = 0, height = 0;
	int scale_type = -1;
	std::vector<ExtraSetting> extra;
	std::string audio_encoder;
	int audio_bitrate_kbps = 0;
	int audio_tracks_mask = 0; // bit 0 = track 1
	bool custom_delay = false;
	int delay_sec = 0;
	bool delay_preserve = false;
	bool shares_other_encoder = false; // output_video_encoder names another Aitum output
};

enum class AitumStatus {
	Ok,
	NotFound,     // no aitum.json for this profile
	TooBig,
	Malformed,    // not JSON / not an object
	UnknownShape  // JSON, but not the inspected layout
};

struct AitumConfig {
	AitumStatus status = AitumStatus::NotFound;
	std::string message;               // for the dock; no values from the file
	std::vector<AitumCanvas> canvases;
	std::vector<AitumOutput> outputs;  // type == "stream" only
	int skipped_outputs = 0;           // other types (backtrack, virtual_cam, record...) or unreadable entries
	std::vector<std::string> notes;
};

AitumConfig parse_aitum(const std::string &text);
// Wipes every key held in the config.
void wipe(AitumConfig &c);
const AitumOutput *find_aitum_output(const AitumConfig &c, const std::string &name);

// Applies `o` on top of `base` (id, name, platform, handle, enabled, auto_start, key_token and the
// own URL are kept unless Aitum supplies a server). `key_out` receives Aitum's key if it has one
// (the caller seals it for "copy", or holds it for the launch for "follow", then wipes it).
// Returns false when the result would not validate; `notes` explains what was and was not mapped.
bool apply_aitum(const Destination &base, const AitumOutput &o, Destination &out, std::string &key_out,
		 std::vector<std::string> &notes);

// ---- double-streaming detection ------------------------------------------------------------
struct TargetRef {
	std::string where; // label that may be shown: "Twitch (plugin)", "Aitum output Vertical Stream", "OBS main stream"
	std::string url;
	std::string key;   // compared only, never shown
};
// Lowercases scheme and host, drops the default port, trailing slashes, query and fragment.
std::string normalize_target_url(const std::string &url);
bool same_target(const TargetRef &a, const TargetRef &b);
// One warning per pair of different `where` that share server AND key. Messages never carry values.
std::vector<std::string> find_double_streaming(const std::vector<TargetRef> &targets);

} // namespace oms
