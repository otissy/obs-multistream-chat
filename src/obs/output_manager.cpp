#include "output_manager.hpp"
#include "redact.hpp"

#include <obs-frontend-api.h>
#include <util/base.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdio>
#include <thread>

namespace oms {

namespace {

const int64_t kStopWatchdogMs = 30000;

void log_line(int level, const std::string &msg)
{
	// Nothing reaches the OBS log without passing through redact().
	std::vector<std::string> none;
	blog(level, "[obs-multistream] %s", redact(msg, none).c_str());
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

} // namespace

struct OutputManager::Ctx {
	OutputManager *mgr;
	std::string id;
	uint64_t epoch;
};

struct OutputManager::Runtime {
	Destination dest;
	HealthMachine health;
	StatsTracker stats;
	obs_output_t *out = nullptr;
	obs_service_t *svc = nullptr;
	Ctx *ctx = nullptr;
	uint64_t epoch = 0;
	bool pending_start = false;
	int64_t stopping_since = -1;
	double target_kbps = 0;
	double bitrate = 0;
	double dropped = 0;
	std::string error;

	Runtime(const Destination &d, HealthMachine::Rng rng) : dest(d), health(HealthConfig(), rng) {}
};

OutputManager::OutputManager(Config &cfg, const SecretProtector &protector, const Clock &clock)
	: cfg_(cfg), protector_(protector), clock_(clock), rng_state_(static_cast<unsigned>(clock.now_ms() | 1) ^ 0x9E3779B9u)
{
	sync_with_config();
}

OutputManager::~OutputManager()
{
	shutdown();
}

// ---------------------------------------------------------------------------------------------

OutputManager::Runtime *OutputManager::find(const std::string &id) const
{
	for (size_t i = 0; i < runtimes_.size(); ++i)
		if (runtimes_[i]->dest.id == id)
			return runtimes_[i].get();
	return nullptr;
}

void OutputManager::sync_with_config()
{
	// remove runtimes whose destination is gone
	for (size_t i = runtimes_.size(); i-- > 0;) {
		bool present = false;
		for (size_t k = 0; k < cfg_.destinations.size(); ++k)
			if (cfg_.destinations[k].id == runtimes_[i]->dest.id)
				present = true;
		if (!present) {
			apply(*runtimes_[i], runtimes_[i]->health.request_stop(clock_.now_ms()));
			release_attempt(*runtimes_[i], true);
			runtimes_.erase(runtimes_.begin() + static_cast<long>(i));
		}
	}
	for (size_t k = 0; k < cfg_.destinations.size(); ++k) {
		const Destination &d = cfg_.destinations[k];
		Runtime *r = find(d.id);
		if (!r) {
			runtimes_.push_back(std::unique_ptr<Runtime>(new Runtime(d, [this]() {
				rng_state_ ^= rng_state_ << 13;
				rng_state_ ^= rng_state_ >> 17;
				rng_state_ ^= rng_state_ << 5;
				return double(rng_state_ & 0xFFFFFF) / double(0x1000000);
			})));
			continue;
		}
		bool conn_changed = r->dest.url != d.url || r->dest.key_token != d.key_token || r->dest.encoder != d.encoder ||
				    r->dest.bitrate_kbps != d.bitrate_kbps;
		if (conn_changed || !d.enabled) {
			// the connection details changed (or it was disabled): stop it, it restarts on request
			apply(*r, r->health.request_stop(clock_.now_ms()));
		}
		r->dest = d;
	}
}

// ---------------------------------------------------------------------------------------------
// main stream events

void OutputManager::on_main_stream_started()
{
	main_live_ = true;
	main_samples_.clear();
	if (!cfg_.auto_start_with_stream)
		return;
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		if (r.dest.enabled && r.dest.auto_start)
			start_destination(r.dest.id);
	}
}

void OutputManager::on_main_stream_stopping()
{
	// Shared outputs hold the main encoders: they must go first. x264 outputs that auto-start with
	// the stream also stop with it; a manually started one keeps running.
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		if (r.dest.encoder == "shared" || r.dest.auto_start)
			apply(r, r.health.request_stop(clock_.now_ms()));
	}
}

void OutputManager::on_main_stream_stopped()
{
	main_live_ = false;
	banner_.clear();
	main_samples_.clear();
}

// ---------------------------------------------------------------------------------------------
// user actions

void OutputManager::start_destination(const std::string &id)
{
	Runtime *r = find(id);
	if (!r || !r->dest.enabled)
		return;
	if (r->dest.encoder == "shared" && !main_live_) {
		r->error = "Start the main stream first: this destination shares its encoders.";
		return;
	}
	r->error.clear();
	apply(*r, r->health.request_start(clock_.now_ms()));
}

void OutputManager::stop_destination(const std::string &id)
{
	Runtime *r = find(id);
	if (r)
		apply(*r, r->health.request_stop(clock_.now_ms()));
}

void OutputManager::start_all()
{
	for (size_t i = 0; i < runtimes_.size(); ++i)
		start_destination(runtimes_[i]->dest.id);
}

void OutputManager::stop_all()
{
	for (size_t i = 0; i < runtimes_.size(); ++i)
		apply(*runtimes_[i], runtimes_[i]->health.request_stop(clock_.now_ms()));
}

void OutputManager::shutdown()
{
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		r.health.request_stop(clock_.now_ms());
		release_attempt(r, true);
	}
}

// ---------------------------------------------------------------------------------------------
// commands -> libobs

void OutputManager::apply(Runtime &r, const Command &c)
{
	if (c.kind == CommandKind::StartOutput) {
		if (r.out) {
			r.pending_start = true; // previous attempt is still shutting down
			return;
		}
		launch(r);
	} else if (c.kind == CommandKind::StopOutput) {
		r.pending_start = false;
		if (r.out) {
			if (r.stopping_since < 0)
				r.stopping_since = clock_.now_ms();
			stop_in_worker(r.out);
		}
	}
}

void OutputManager::fail_launch(Runtime &r, const std::string &why)
{
	std::vector<std::string> none;
	std::string safe = redact(why, none);
	r.error = safe;
	log_line(LOG_WARNING, r.dest.name + ": " + safe);
	push_event(Event{r.dest.id, r.epoch, EvKind::Stopped, -2, safe});
}

bool OutputManager::launch(Runtime &r)
{
	r.epoch += 1;
	r.stats.reset();
	r.bitrate = 0;
	r.dropped = 0;

	std::string key;
	if (!unlock_key(r.dest, protector_, key)) {
		fail_launch(r, "stream key cannot be decrypted (saved by another Windows user or machine?): re-enter it");
		return false;
	}

	obs_encoder_t *venc = nullptr;
	obs_encoder_t *aenc = nullptr;
	bool own = false;
	r.target_kbps = 0;

	if (r.dest.encoder == "shared") {
		obs_output_t *main = obs_frontend_get_streaming_output();
		if (!main) {
			secure_zero(key);
			fail_launch(r, "the main stream is not running, so there are no encoders to share");
			return false;
		}
		venc = obs_output_get_video_encoder(main);
		aenc = obs_output_get_audio_encoder(main, 0);
		if (venc) {
			obs_data_t *s = obs_encoder_get_settings(venc);
			if (s) {
				r.target_kbps = double(obs_data_get_int(s, "bitrate"));
				obs_data_release(s);
			}
		}
		// attach before dropping our reference to main: the output takes its own references
		obs_output_t *tmp = obs_output_create("rtmp_output", r.dest.name.c_str(), nullptr, nullptr);
		if (!tmp || !venc || !aenc) {
			if (tmp)
				obs_output_release(tmp);
			obs_output_release(main);
			secure_zero(key);
			fail_launch(r, "the main stream has no usable video/audio encoder to share");
			return false;
		}
		obs_output_set_video_encoder(tmp, venc);
		obs_output_set_audio_encoder(tmp, aenc, 0);
		obs_output_release(main);
		r.out = tmp;
	} else {
		own = true;
		obs_data_t *vs = obs_data_create();
		obs_data_set_string(vs, "rate_control", "CBR");
		obs_data_set_int(vs, "bitrate", r.dest.bitrate_kbps);
		obs_data_set_int(vs, "keyint_sec", 2);
		obs_data_set_string(vs, "preset", "veryfast");
		venc = obs_video_encoder_create("obs_x264", (r.dest.name + " video").c_str(), vs, nullptr);
		obs_data_release(vs);
		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", 160);
		aenc = obs_audio_encoder_create("ffmpeg_aac", (r.dest.name + " audio").c_str(), as, 0, nullptr);
		obs_data_release(as);
		if (!venc || !aenc) {
			if (venc)
				obs_encoder_release(venc);
			if (aenc)
				obs_encoder_release(aenc);
			secure_zero(key);
			fail_launch(r, "could not create the x264/AAC encoders");
			return false;
		}
		obs_encoder_set_video(venc, obs_get_video());
		obs_encoder_set_audio(aenc, obs_get_audio());
		r.target_kbps = double(r.dest.bitrate_kbps);
		r.out = obs_output_create("rtmp_output", r.dest.name.c_str(), nullptr, nullptr);
		if (r.out) {
			obs_output_set_video_encoder(r.out, venc);
			obs_output_set_audio_encoder(r.out, aenc, 0);
		}
		obs_encoder_release(venc); // the output keeps its own references
		obs_encoder_release(aenc);
		if (!r.out) {
			secure_zero(key);
			fail_launch(r, "could not create the RTMP output");
			return false;
		}
	}
	(void)own;

	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", r.dest.url.c_str());
	obs_data_set_string(ss, "key", key.c_str());
	r.svc = obs_service_create("rtmp_custom", (r.dest.name + " service").c_str(), ss, nullptr);
	obs_data_release(ss);
	secure_zero(key);
	if (!r.svc) {
		obs_output_release(r.out);
		r.out = nullptr;
		fail_launch(r, "could not create the RTMP service");
		return false;
	}
	obs_output_set_service(r.out, r.svc);
	// our own health machine owns reconnects: libobs must not retry behind its back
	obs_output_set_reconnect_settings(r.out, 0, 0);

	r.ctx = new Ctx{this, r.dest.id, r.epoch};
	signal_handler_t *sh = obs_output_get_signal_handler(r.out);
	signal_handler_connect(sh, "start", &OutputManager::sig_start, r.ctx);
	signal_handler_connect(sh, "stop", &OutputManager::sig_stop, r.ctx);

	log_line(LOG_INFO, r.dest.name + ": starting (" + r.dest.encoder + " encoders) to " + sanitize_url(r.dest.url));
	if (!obs_output_start(r.out)) {
		const char *e = obs_output_get_last_error(r.out);
		fail_launch(r, e && *e ? e : "output failed to start");
		return false;
	}
	return true;
}

// Called on the UI thread after a Stopped event (or at shutdown) to drop the finished attempt.
void OutputManager::release_attempt(Runtime &r, bool force)
{
	if (r.out) {
		signal_handler_t *sh = obs_output_get_signal_handler(r.out);
		if (r.ctx && sh) {
			signal_handler_disconnect(sh, "start", &OutputManager::sig_start, r.ctx);
			signal_handler_disconnect(sh, "stop", &OutputManager::sig_stop, r.ctx);
		}
		if (force && obs_output_active(r.out)) {
			stop_in_worker(r.out); // the worker holds its own reference; ours can go
		}
		obs_output_release(r.out);
		r.out = nullptr;
	}
	if (r.svc) {
		obs_service_release(r.svc);
		r.svc = nullptr;
	}
	// r.ctx is intentionally not freed: a late signal on a libobs thread may still hold the
	// pointer. It is 48 bytes per attempt and bounded by the reconnect limits.
	r.ctx = nullptr;
	r.stopping_since = -1;
}

// ---------------------------------------------------------------------------------------------
// libobs threads -> queue

void OutputManager::push_event(const Event &e)
{
	std::lock_guard<std::mutex> lk(ev_m_);
	events_.push_back(e);
}

void OutputManager::sig_start(void *data, calldata_t *)
{
	Ctx *c = static_cast<Ctx *>(data);
	c->mgr->push_event(Event{c->id, c->epoch, EvKind::Started, 0, std::string()});
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
	c->mgr->push_event(Event{c->id, c->epoch, EvKind::Stopped, code, err});
}

// ---------------------------------------------------------------------------------------------
// periodic work

void OutputManager::drain_events()
{
	std::vector<Event> evs;
	{
		std::lock_guard<std::mutex> lk(ev_m_);
		evs.swap(events_);
	}
	int64_t now = clock_.now_ms();
	for (size_t i = 0; i < evs.size(); ++i) {
		const Event &e = evs[i];
		Runtime *r = find(e.id);
		if (!r || e.epoch != r->epoch)
			continue; // destination removed, or an event from an attempt already replaced
		if (e.kind == EvKind::Started) {
			apply(*r, r->health.on_output_started(now));
		} else {
			std::vector<std::string> none;
			std::string err = redact(e.error, none);
			if (!err.empty())
				r->error = err;
			bool user_or_expected = (r->health.state() == State::Idle);
			if (!user_or_expected)
				log_line(LOG_WARNING, r->dest.name + ": output stopped (" + (err.empty() ? "no error" : err) + ")");
			apply(*r, r->health.on_output_stopped(now, e.code != OBS_OUTPUT_SUCCESS, err));
			release_attempt(*r, false);
			if (r->pending_start && r->health.state() == State::Connecting) {
				r->pending_start = false;
				launch(*r);
			}
		}
	}
}

void OutputManager::sample(Runtime &r, int64_t now)
{
	if (!r.out)
		return;
	State s = r.health.state();
	if (s != State::Live && s != State::Degraded)
		return;
	Sample sm;
	sm.t_ms = now;
	sm.bytes = obs_output_get_total_bytes(r.out);
	sm.frames = obs_output_get_total_frames(r.out);
	sm.dropped = obs_output_get_frames_dropped(r.out);
	r.stats.add(sm);
	r.bitrate = r.stats.bitrate_kbps(10000);
	r.dropped = r.stats.dropped_pct(10000);
	apply(r, r.health.on_sample(now, r.bitrate, r.dropped, r.target_kbps));
}

void OutputManager::update_banner(int64_t now)
{
	if (!main_live_) {
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
	drain_events();
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		apply(r, r.health.tick(now));
		sample(r, now);
		// watchdog: an output that never reported "stop"
		if (r.out && r.stopping_since >= 0 && now - r.stopping_since > kStopWatchdogMs) {
			log_line(LOG_ERROR, r.dest.name + ": output did not stop in time; abandoning it");
			push_event(Event{r.dest.id, r.epoch, EvKind::Stopped, -4, "output did not stop in time"});
			r.stopping_since = now;
		}
	}
	update_banner(now);
}

// ---------------------------------------------------------------------------------------------
// read side

std::vector<RuntimeView> OutputManager::views() const
{
	int64_t now = clock_.now_ms();
	std::vector<RuntimeView> v;
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		const Runtime &r = *runtimes_[i];
		RuntimeView rv;
		rv.dest = r.dest;
		rv.dest.key.clear();
		rv.dest.key_token.clear();
		rv.state = r.health.state();
		rv.bitrate_kbps = r.bitrate;
		rv.dropped_pct = r.dropped;
		rv.uptime_ms = r.health.uptime_ms(now);
		rv.attempts = r.health.attempts();
		rv.retry_in_ms = r.health.next_retry_ms() >= 0 ? std::max<int64_t>(0, r.health.next_retry_ms() - now) : -1;
		rv.detail = r.health.detail();
		rv.last_error = !r.health.last_error().empty() ? r.health.last_error() : r.error;
		bool active = rv.state != State::Idle && rv.state != State::Failed;
		rv.can_stop = active;
		rv.can_start = !active && r.dest.enabled && (r.dest.encoder != "shared" || main_live_);
		v.push_back(rv);
	}
	return v;
}

std::vector<OverlayEntry> OutputManager::overlay_entries() const
{
	std::vector<OverlayEntry> out;
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		const Runtime &r = *runtimes_[i];
		if (!r.dest.enabled)
			continue;
		OverlayEntry e;
		e.id = r.dest.id;
		e.platform = r.dest.platform;
		e.handle = r.dest.handle;
		e.state = r.health.state();
		out.push_back(e);
	}
	return out;
}

} // namespace oms
