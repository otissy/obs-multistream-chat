// Decides, from a destination and a snapshot of what OBS has right now, which video and audio
// encoders an output uses. Pure logic (the glue builds the snapshot from libobs and executes the plan).
#pragma once
#include "destination.hpp"

#include <string>
#include <vector>

namespace oms {

struct CanvasInfo {
	std::string name;
	bool is_main = false;
	bool has_video = false; // obs_canvas_has_video
	int width = 0;
	int height = 0;
};

struct EnvSnapshot {
	bool main_live = false;
	// One entry per audio encoder the main stream output carries, in encoder-index order:
	// the 1-based mixer track it encodes (0 = no encoder at that index).
	std::vector<int> main_audio_tracks;
	std::vector<CanvasInfo> canvases; // includes the main canvas
	bool check_encoders = false;      // false: skip the installed-encoder check (tests)
	std::vector<std::string> video_encoders; // installed video encoder ids
};

struct LaunchPlan {
	bool video_share_main = false;
	std::string canvas;     // "" = main canvas
	int canvas_w = 0, canvas_h = 0;
	int out_w = 0, out_h = 0; // 0 = no rescale
	bool audio_share_main = false;
	int main_audio_index = 0; // when audio_share_main
	int audio_track = 1;      // 1-based, own audio encoder
	bool depends_on_main = false; // must stop before the main stream's encoders go away
};

// false: `err` is a short, secret-free reason for the dock.
bool make_plan(const Destination &d, const EnvSnapshot &env, LaunchPlan &plan, std::string &err);

// "Track 2" or "Track 2: Music" (name from the profile; "" when the user did not name it).
std::string track_label(int track, const std::string &name);

} // namespace oms
