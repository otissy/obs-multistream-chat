#include "output_manager.hpp"
#include "aitum.hpp"
#include "redact.hpp"

#include <obs-frontend-api.h>
#include <util/base.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace oms {

namespace {

void log_line(int level, const std::string &msg)
{
	// Nothing reaches the OBS log without passing through redact().
	std::vector<std::string> none;
	blog(level, "[obs-multistream-chat] %s", redact(msg, none).c_str());
}

// Stops an output without ever blocking the UI thread: obs_output_stop/force_stop can join the
// RTMP connect thread, which may take as long as the connect timeout.
void stop_in_worker(obs_output_t *out)
{
	obs_output_t *ref = obs_output_get_ref(out);
	if (!ref)
		return;
	std::thread([ref]() {
		if (obs_output_active(ref)) {
			obs_output_stop(ref);
			for (int i = 0; i < 40 && obs_output_active(ref); ++i)
				os_sleep_ms(100);
			if (obs_output_active(ref))
				obs_output_force_stop(ref);
		} else {
			obs_output_force_stop(ref);
		}
		obs_output_release(ref);
	}).detach();
}

// ---- canvases --------------------------------------------------------------------------------

struct CanvasEnum {
	std::vector<CanvasInfo> *out;
};

CanvasInfo describe_canvas(obs_canvas_t *c)
{
	CanvasInfo k;
	const char *n = obs_canvas_get_name(c);
	k.is_main = (obs_canvas_get_flags(c) & MAIN) != 0;
	k.name = k.is_main ? std::string() : (n ? n : "");
	k.has_video = obs_canvas_has_video(c);
	if (k.has_video) {
		video_t *v = obs_canvas_get_video(c);
		if (v) {
			k.width = static_cast<int>(video_output_get_width(v));
			k.height = static_cast<int>(video_output_get_height(v));
		}
		if (k.width <= 0 || k.height <= 0) {
			struct obs_video_info ovi;
			if (obs_canvas_get_video_info(c, &ovi)) {
				k.width = static_cast<int>(ovi.output_width);
				k.height = static_cast<int>(ovi.output_height);
			}
		}
	}
	return k;
}

bool enum_canvas_cb(void *param, obs_canvas_t *c)
{
	CanvasEnum *e = static_cast<CanvasEnum *>(param);
	if (obs_canvas_removed(c))
		return true;
	e->out->push_back(describe_canvas(c));
	return true;
}

} // namespace

struct OutputManager::Ctx {
	OutputManager *mgr;
	std::string id;
	uint64_t epoch;
};

OutputManager::OutputManager(Config &cfg, const SecretProtector &protector, const Clock &clock)
	: cfg_(cfg), clock_(clock), fleet_(cfg, protector, clock, *this)
{
	fleet_.set_logger([](int level, const std::string &m) {
		log_line(level == 0 ? LOG_INFO : level == 1 ? LOG_WARNING : LOG_ERROR, m);
	});
	fleet_.set_aitum_provider([]() -> std::string {
		char *dir = obs_frontend_get_current_profile_path();
		if (!dir)
			return std::string();
		std::filesystem::path p = std::filesystem::u8path(dir);
		bfree(dir);
		p /= "aitum.json";
		std::error_code ec;
		if (!std::filesystem::exists(p, ec) || ec)
			return std::string();
		uintmax_t sz = std::filesystem::file_size(p, ec);
		if (ec)
			return std::string();
		if (sz > kMaxAitumFileBytes)
			return std::string(kMaxAitumFileBytes + 1, ' '); // parse_aitum reports "too big"
		std::string text;
		if (!read_file(p.u8string(), text))
			return std::string();
		return text; // READ-ONLY: we never write or touch this file
	});
	fleet_.set_external_targets([this]() {
		std::vector<TargetRef> out;
		obs_output_t *main = obs_frontend_get_streaming_output();
		struct Ctx2 {
			std::vector<TargetRef> *out;
			OutputManager *self;
			obs_output_t *main;
		} c{&out, this, main};
		obs_enum_outputs(
			[](void *p, obs_output_t *o) {
				Ctx2 *c = static_cast<Ctx2 *>(p);
				if (!obs_output_active(o))
					return true;
				for (std::map<std::string, Attempt>::const_iterator it = c->self->attempts_.begin(); it != c->self->attempts_.end(); ++it)
					if (it->second.out == o)
						return true; // ours: the fleet checks those itself
				obs_service_t *svc = obs_output_get_service(o);
				if (!svc)
					return true;
				const char *u = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
				const char *k = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
				if (!u || !*u || !k || !*k)
					return true;
				TargetRef t;
				const char *n = obs_output_get_name(o);
				t.where = (c->main == o) ? std::string("OBS main stream") : std::string("OBS output \"") + (n ? n : "?") + "\"";
				t.url = u;
				t.key = k;
				c->out->push_back(t);
				return true;
			},
			&c);
		if (main)
			obs_output_release(main);
		return out;
	});
}

OutputManager::~OutputManager()
{
	fleet_.shutdown();
}

void OutputManager::on_main_stream_started()
{
	main_samples_.clear();
	fleet_.on_main_stream_started();
}

void OutputManager::on_main_stream_stopped()
{
	banner_.clear();
	main_samples_.clear();
	fleet_.on_main_stream_stopped();
}

// ---------------------------------------------------------------------------------------------
// pickers

std::vector<std::string> OutputManager::canvas_names() const
{
	std::vector<CanvasInfo> all;
	CanvasEnum e{&all};
	obs_enum_canvases(&enum_canvas_cb, &e);
	std::vector<std::string> names;
	for (size_t i = 0; i < all.size(); ++i)
		if (!all[i].is_main && !all[i].name.empty())
			names.push_back(all[i].name);
	return names;
}

std::vector<CanvasInfo> OutputManager::canvases() const
{
	std::vector<CanvasInfo> all;
	CanvasEnum e{&all};
	obs_enum_canvases(&enum_canvas_cb, &e);
	return all;
}

std::vector<EncoderChoice> OutputManager::video_encoders() const
{
	std::vector<EncoderChoice> out;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		uint32_t caps = obs_get_encoder_caps(id);
		if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		const char *codec = obs_get_encoder_codec(id);
		std::string c = codec ? codec : "";
		if (c != "h264" && c != "hevc" && c != "av1")
			continue;
		EncoderChoice e;
		e.id = id;
		const char *dn = obs_encoder_get_display_name(id);
		e.label = dn ? dn : id;
		e.codec = c;
		e.hardware = e.id != "obs_x264";
		out.push_back(e);
	}
	return out;
}

std::string OutputManager::track_name(int track) const
{
	config_t *pc = obs_frontend_get_profile_config();
	if (!pc || track < 1 || track > 6)
		return std::string();
	std::string key = "Track" + std::to_string(track) + "Name";
	const char *n = config_get_string(pc, "AdvOut", key.c_str());
	return n ? n : "";
}

AitumInfo OutputManager::aitum_info() const
{
	AitumInfo info;
	obs_module_t *m = obs_get_module("aitum-stream-suite");
	if (!m)
		return info;
	info.installed = true;
#ifdef _WIN32
	const char *bin = obs_get_module_binary_path(m);
	if (bin) {
		int wn = MultiByteToWideChar(CP_UTF8, 0, bin, -1, nullptr, 0);
		std::wstring w(static_cast<size_t>(wn > 0 ? wn : 1), L'\0');
		if (wn > 0)
			MultiByteToWideChar(CP_UTF8, 0, bin, -1, &w[0], wn);
		DWORD dummy = 0;
		DWORD sz = GetFileVersionInfoSizeW(w.c_str(), &dummy);
		if (sz > 0) {
			std::vector<char> buf(sz);
			if (GetFileVersionInfoW(w.c_str(), 0, sz, buf.data())) {
				VS_FIXEDFILEINFO *ffi = nullptr;
				UINT len = 0;
				if (VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID *>(&ffi), &len) && ffi) {
					char v[48];
					std::snprintf(v, sizeof v, "%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
						      HIWORD(ffi->dwFileVersionLS));
					info.version = v;
				}
			}
		}
	}
#endif
	info.version_inspected = info.version == kAitumInspectedVersion;
	return info;
}

// ---------------------------------------------------------------------------------------------
// environment snapshot for make_plan

EnvSnapshot OutputManager::snapshot() const
{
	EnvSnapshot env;
	obs_output_t *main = obs_frontend_get_streaming_output();
	env.main_live = fleet_.main_live() && main != nullptr;
	if (main) {
		int last = -1;
		std::vector<int> tracks(6, 0);
		for (int i = 0; i < 6; ++i) {
			obs_encoder_t *a = obs_output_get_audio_encoder(main, static_cast<size_t>(i));
			if (a) {
				tracks[static_cast<size_t>(i)] = static_cast<int>(obs_encoder_get_mixer_index(a)) + 1;
				last = i;
			}
		}
		tracks.resize(static_cast<size_t>(last + 1));
		env.main_audio_tracks = tracks;
		obs_output_release(main);
	}
	CanvasEnum e{&env.canvases};
	obs_enum_canvases(&enum_canvas_cb, &e);
	bool have_main = false;
	for (size_t i = 0; i < env.canvases.size(); ++i)
		have_main |= env.canvases[i].is_main;
	if (!have_main) {
		obs_canvas_t *mc = obs_get_main_canvas();
		if (mc) {
			env.canvases.push_back(describe_canvas(mc));
			obs_canvas_release(mc);
		}
	}
	env.check_encoders = true;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i)
		if (id && obs_get_encoder_type(id) == OBS_ENCODER_VIDEO)
			env.video_encoders.push_back(id);
	return env;
}

namespace {

void apply_extra(obs_data_t *s, const ExtraSetting &e)
{
	switch (e.type) {
	case 'i': obs_data_set_int(s, e.key.c_str(), std::atoll(e.value.c_str())); break;
	case 'd': obs_data_set_double(s, e.key.c_str(), std::atof(e.value.c_str())); break;
	case 'b': obs_data_set_bool(s, e.key.c_str(), e.value == "true"); break;
	default: obs_data_set_string(s, e.key.c_str(), e.value.c_str()); break;
	}
}

bool source_feeds_track(void *p, obs_source_t *src)
{
	int *bit = static_cast<int *>(p);
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) && (obs_source_get_audio_mixers(src) & (1u << *bit)))
		*bit = -1; // found
	return *bit >= 0;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// OutputBackend

bool OutputManager::launch(const std::string &id, uint64_t epoch, const Destination &eff, const std::string &key,
			   bool &depends_on_main, double &target_kbps, std::string &err)
{
	Attempt &a = attempts_[id];
	drop(a, true); // never two attempts for one destination
	target_kbps = 0;
	depends_on_main = false;

	LaunchPlan plan;
	if (!make_plan(eff, snapshot(), plan, err)) {
		attempts_.erase(id);
		return false;
	}

	obs_encoder_t *venc = nullptr;
	obs_encoder_t *aenc = nullptr;
	bool own_v = false, own_a = false;
	obs_output_t *main = nullptr;
	auto cleanup_encoders = [&]() {
		if (own_v && venc)
			obs_encoder_release(venc);
		if (own_a && aenc)
			obs_encoder_release(aenc);
		venc = aenc = nullptr;
	};

	if (plan.video_share_main || plan.audio_share_main) {
		main = obs_frontend_get_streaming_output();
		if (!main) {
			err = "the main stream is not running, so there are no encoders to share";
			attempts_.erase(id);
			return false;
		}
	}

	if (plan.video_share_main) {
		venc = obs_output_get_video_encoder(main);
		if (venc) {
			obs_data_t *s = obs_encoder_get_settings(venc);
			if (s) {
				target_kbps = double(obs_data_get_int(s, "bitrate"));
				obs_data_release(s);
			}
		}
	} else {
		// own video encoder on the chosen canvas
		obs_canvas_t *canvas = plan.canvas.empty() ? obs_get_main_canvas() : obs_get_canvas_by_name(plan.canvas.c_str());
		video_t *video = canvas ? obs_canvas_get_video(canvas) : nullptr;
		if (!canvas || !video) {
			if (canvas)
				obs_canvas_release(canvas);
			if (main)
				obs_output_release(main);
			err = plan.canvas.empty() ? "the main canvas has no video output"
						  : "canvas \"" + plan.canvas + "\" went away or has no video output";
			attempts_.erase(id);
			return false;
		}
		a.canvas = canvas; // held until the attempt is dropped
		obs_data_t *vs = obs_data_create();
		for (size_t i = 0; i < eff.venc_extra.size(); ++i)
			apply_extra(vs, eff.venc_extra[i]);
		if (!eff.rate_control.empty())
			obs_data_set_string(vs, "rate_control", eff.rate_control.c_str());
		obs_data_set_int(vs, "bitrate", eff.bitrate_kbps);
		if (eff.keyint_sec > 0)
			obs_data_set_int(vs, "keyint_sec", eff.keyint_sec);
		if (!eff.preset.empty())
			obs_data_set_string(vs, "preset", eff.preset.c_str());
		if (!eff.profile.empty())
			obs_data_set_string(vs, "profile", eff.profile.c_str());
		venc = obs_video_encoder_create(eff.video_encoder.c_str(), (eff.name + " video").c_str(), vs, nullptr);
		obs_data_release(vs);
		if (!venc) {
			if (main)
				obs_output_release(main);
			err = "could not create the video encoder \"" + eff.video_encoder + "\"";
			drop(a, true);
			attempts_.erase(id);
			return false;
		}
		own_v = true;
		obs_encoder_set_video(venc, video);
		if (plan.out_w > 0) {
			obs_encoder_set_scaled_size(venc, static_cast<uint32_t>(plan.out_w), static_cast<uint32_t>(plan.out_h));
			if (eff.scale_type > 0)
				obs_encoder_set_gpu_scale_type(venc, static_cast<enum obs_scale_type>(eff.scale_type));
		}
		if (eff.fps_div > 1 && !obs_encoder_set_frame_rate_divisor(venc, static_cast<uint32_t>(eff.fps_div))) {
			if (main)
				obs_output_release(main);
			cleanup_encoders();
			err = "the encoder rejected the frame-rate divisor";
			drop(a, true);
			attempts_.erase(id);
			return false;
		}
		target_kbps = double(eff.bitrate_kbps);
	}

	if (plan.audio_share_main) {
		aenc = obs_output_get_audio_encoder(main, static_cast<size_t>(plan.main_audio_index));
	} else {
		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", eff.audio_bitrate_kbps);
		aenc = obs_audio_encoder_create("ffmpeg_aac", (eff.name + " audio").c_str(), as, static_cast<size_t>(plan.audio_track - 1), nullptr);
		obs_data_release(as);
		own_a = aenc != nullptr;
		if (aenc)
			obs_encoder_set_audio(aenc, obs_get_audio());
		int bit = plan.audio_track - 1;
		obs_enum_sources(&source_feeds_track, &bit);
		if (bit >= 0)
			log_line(LOG_WARNING, eff.name + ": no audio source is routed to track " + std::to_string(plan.audio_track) +
						      " (Advanced Audio Properties): that track is silent");
	}

	if (!venc || !aenc) {
		if (main)
			obs_output_release(main);
		cleanup_encoders();
		err = !venc ? "the main stream has no usable video encoder to share" : "could not create the audio encoder";
		drop(a, true);
		attempts_.erase(id);
		return false;
	}

	a.out = obs_output_create("rtmp_output", eff.name.c_str(), nullptr, nullptr);
	if (a.out) {
		obs_output_set_video_encoder(a.out, venc);
		obs_output_set_audio_encoder(a.out, aenc, 0);
	}
	cleanup_encoders(); // the output keeps its own references
	if (main)
		obs_output_release(main);
	if (!a.out) {
		err = "could not create the RTMP output";
		drop(a, true);
		attempts_.erase(id);
		return false;
	}

	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", eff.url.c_str());
	obs_data_set_string(ss, "key", key.c_str());
	a.svc = obs_service_create("rtmp_custom", (eff.name + " service").c_str(), ss, nullptr);
	obs_data_release(ss);
	if (!a.svc) {
		err = "could not create the RTMP service";
		drop(a, true);
		attempts_.erase(id);
		return false;
	}
	obs_output_set_service(a.out, a.svc);
	// our own health machine owns reconnects: libobs must not retry behind its back
	obs_output_set_reconnect_settings(a.out, 0, 0);
	if (eff.delay_sec > 0)
		obs_output_set_delay(a.out, static_cast<uint32_t>(eff.delay_sec), eff.delay_preserve ? OBS_OUTPUT_DELAY_PRESERVE : 0);

	a.ctx = new Ctx{this, id, epoch};
	signal_handler_t *sh = obs_output_get_signal_handler(a.out);
	signal_handler_connect(sh, "start", &OutputManager::sig_start, a.ctx);
	signal_handler_connect(sh, "stop", &OutputManager::sig_stop, a.ctx);

	depends_on_main = plan.depends_on_main;
	log_line(LOG_INFO, eff.name + ": starting (" + (plan.video_share_main ? std::string("shared video") : eff.video_encoder) + ", " +
				   (plan.canvas.empty() ? std::string("main canvas") : "canvas " + plan.canvas) + ", audio track " +
				   std::to_string(plan.audio_share_main ? 0 : plan.audio_track) + (plan.audio_share_main ? " shared" : " own") +
				   ") to " + sanitize_url(eff.url));
	if (!obs_output_start(a.out)) {
		const char *e = obs_output_get_last_error(a.out);
		err = e && *e ? e : "output failed to start";
		drop(a, true);
		attempts_.erase(id);
		return false;
	}
	return true;
}

void OutputManager::drop(Attempt &a, bool disconnect)
{
	if (a.out) {
		signal_handler_t *sh = obs_output_get_signal_handler(a.out);
		if (disconnect && a.ctx && sh) {
			signal_handler_disconnect(sh, "start", &OutputManager::sig_start, a.ctx);
			signal_handler_disconnect(sh, "stop", &OutputManager::sig_stop, a.ctx);
		}
		if (obs_output_active(a.out))
			stop_in_worker(a.out); // the worker holds its own reference; ours can go
		obs_output_release(a.out);
		a.out = nullptr;
	}
	if (a.svc) {
		obs_service_release(a.svc);
		a.svc = nullptr;
	}
	if (a.canvas) {
		obs_canvas_release(a.canvas);
		a.canvas = nullptr;
	}
	// a.ctx is intentionally not freed: a late signal on a libobs thread may still hold the
	// pointer. It is a few dozen bytes per attempt and bounded by the reconnect limits.
	a.ctx = nullptr;
}

void OutputManager::request_stop(const std::string &id)
{
	std::map<std::string, Attempt>::iterator it = attempts_.find(id);
	if (it != attempts_.end() && it->second.out)
		stop_in_worker(it->second.out);
}

bool OutputManager::sample(const std::string &id, Sample &out)
{
	std::map<std::string, Attempt>::iterator it = attempts_.find(id);
	if (it == attempts_.end() || !it->second.out)
		return false;
	out.bytes = obs_output_get_total_bytes(it->second.out);
	out.frames = obs_output_get_total_frames(it->second.out);
	out.dropped = obs_output_get_frames_dropped(it->second.out);
	return true;
}

void OutputManager::release(const std::string &id, bool force)
{
	(void)force;
	std::map<std::string, Attempt>::iterator it = attempts_.find(id);
	if (it == attempts_.end())
		return;
	drop(it->second, true);
	attempts_.erase(it);
}

bool OutputManager::has_output(const std::string &id) const
{
	std::map<std::string, Attempt>::const_iterator it = attempts_.find(id);
	return it != attempts_.end() && it->second.out != nullptr;
}

// ---------------------------------------------------------------------------------------------
// libobs threads -> fleet queue

void OutputManager::sig_start(void *data, calldata_t *)
{
	Ctx *c = static_cast<Ctx *>(data);
	c->mgr->fleet_.notify_started(c->id, c->epoch);
}

void OutputManager::sig_stop(void *data, calldata_t *cd)
{
	Ctx *c = static_cast<Ctx *>(data);
	int code = static_cast<int>(calldata_int(cd, "code"));
	std::string err;
	obs_output_t *o = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
	if (o) {
		const char *e = obs_output_get_last_error(o);
		if (e)
			err = e;
	}
	if (err.empty() && code != OBS_OUTPUT_SUCCESS) {
		switch (code) {
		case OBS_OUTPUT_CONNECT_FAILED: err = "could not connect to the server"; break;
		case OBS_OUTPUT_INVALID_STREAM: err = "invalid stream key or path"; break;
		case OBS_OUTPUT_DISCONNECTED: err = "disconnected"; break;
		case OBS_OUTPUT_UNSUPPORTED: err = "unsupported by the server"; break;
		case OBS_OUTPUT_ENCODE_ERROR: err = "encoder error"; break;
		default: err = "output error"; break;
		}
	}
	c->mgr->fleet_.notify_stopped(c->id, c->epoch, code, err);
}

// ---------------------------------------------------------------------------------------------
// periodic work

void OutputManager::update_banner(int64_t now)
{
	if (!fleet_.main_live()) {
		banner_.clear();
		return;
	}
	MainSample m;
	m.t = now;
	m.frames = 0;
	m.dropped = 0;
	obs_output_t *main = obs_frontend_get_streaming_output();
	if (main) {
		m.frames = static_cast<uint64_t>(obs_output_get_total_frames(main));
		m.dropped = static_cast<uint64_t>(obs_output_get_frames_dropped(main));
		obs_output_release(main);
	}
	video_t *v = obs_get_video();
	m.skipped = v ? video_output_get_skipped_frames(v) : 0;
	m.lagged = obs_get_lagged_frames();
	m.total = obs_get_total_frames();
	main_samples_.push_back(m);
	while (main_samples_.size() > 2 && now - main_samples_.front().t > 15000)
		main_samples_.pop_front();
	if (main_samples_.size() < 4) {
		banner_.clear();
		return;
	}
	const MainSample &a = main_samples_.front();
	double df = double(m.frames) - double(a.frames);
	double dd = double(m.dropped) - double(a.dropped);
	double skipped = double(m.skipped) - double(a.skipped);
	double lagged = double(m.lagged) - double(a.lagged);
	double total = double(m.total) - double(a.total);
	char buf[200];
	if (total > 0 && (skipped + lagged) * 100.0 / total >= 2.0) {
		std::snprintf(buf, sizeof buf, "Encoder or GPU is the bottleneck: OBS skipped/lagged %.1f%% of frames in the last 15 s.",
			      (skipped + lagged) * 100.0 / total);
		banner_ = buf;
	} else if (df > 0 && dd * 100.0 / df >= 2.0) {
		std::snprintf(buf, sizeof buf, "Network is the bottleneck: the main stream dropped %.1f%% of frames in the last 15 s.",
			      dd * 100.0 / df);
		banner_ = buf;
	} else {
		banner_.clear();
	}
}

void OutputManager::tick()
{
	int64_t now = clock_.now_ms();
	fleet_.tick();
	update_banner(now);
	if (now - last_double_check_ >= 5000) {
		last_double_check_ = now;
		double_warnings_ = fleet_.double_stream_warnings();
	}
}

bool OutputManager::any_live() const
{
	std::vector<RuntimeView> v = fleet_.views();
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i].state == State::Live || v[i].state == State::Degraded)
			return true;
	}
	return false;
}

} // namespace oms
