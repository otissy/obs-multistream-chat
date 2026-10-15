#include "plan.hpp"

#include <algorithm>

namespace oms {

std::string track_label(int track, const std::string &name)
{
	std::string s = "Track " + std::to_string(track);
	if (!name.empty())
		s += ": " + name;
	return s;
}

bool make_plan(const Destination &d, const EnvSnapshot &env, LaunchPlan &plan, std::string &err)
{
	plan = LaunchPlan();
	err.clear();
	if (d.audio_track < 0 || d.audio_track > kMaxAudioTracks) {
		err = "audio track must be 1-6";
		return false;
	}
	if (d.encoder == "shared") {
		if (!env.main_live) {
			err = "Start the main stream first: this destination shares its encoders.";
			return false;
		}
		if (!d.canvas.empty()) {
			err = "a destination on another canvas needs its own video encoder";
			return false;
		}
		if (env.main_audio_tracks.empty()) {
			err = "the main stream has no usable video/audio encoder to share";
			return false;
		}
		plan.video_share_main = true;
		if (d.audio_track == 0) {
			plan.audio_share_main = true;
			plan.main_audio_index = 0;
		} else {
			for (size_t i = 0; i < env.main_audio_tracks.size(); ++i)
				if (env.main_audio_tracks[i] == d.audio_track) {
					plan.audio_share_main = true;
					plan.main_audio_index = static_cast<int>(i);
					break;
				}
			plan.audio_track = d.audio_track; // otherwise: own encoder on that track
		}
		plan.depends_on_main = true;
		return true;
	}
	if (d.encoder != "own") {
		err = "unknown encoder mode";
		return false;
	}
	const CanvasInfo *c = nullptr;
	for (size_t i = 0; i < env.canvases.size(); ++i) {
		const CanvasInfo &k = env.canvases[i];
		if (d.canvas.empty() ? k.is_main : (!k.is_main && k.name == d.canvas)) {
			c = &k;
			break;
		}
	}
	if (!c) {
		err = d.canvas.empty() ? "the main canvas is not available"
				       : "canvas \"" + d.canvas + "\" is not available (Aitum creates its canvases when OBS starts)";
		return false;
	}
	if (!c->has_video || c->width <= 0 || c->height <= 0) {
		err = "canvas \"" + (d.canvas.empty() ? std::string("main") : d.canvas) + "\" has no video output yet";
		return false;
	}
	if (env.check_encoders &&
	    std::find(env.video_encoders.begin(), env.video_encoders.end(), d.video_encoder) == env.video_encoders.end()) {
		err = "video encoder \"" + d.video_encoder + "\" is not installed";
		return false;
	}
	plan.canvas = d.canvas;
	plan.canvas_w = c->width;
	plan.canvas_h = c->height;
	if (d.out_width > 0 && d.out_height > 0 && (d.out_width != c->width || d.out_height != c->height)) {
		plan.out_w = d.out_width;
		plan.out_h = d.out_height;
	}
	if (d.audio_track != 0)
		plan.audio_track = d.audio_track;
	else if (env.main_live && !env.main_audio_tracks.empty() && env.main_audio_tracks[0] > 0)
		plan.audio_track = env.main_audio_tracks[0];
	else
		plan.audio_track = 1;
	plan.depends_on_main = false;
	return true;
}

} // namespace oms
