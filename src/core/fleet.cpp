#include "fleet.hpp"
#include "redact.hpp"

#include <algorithm>

namespace oms {

namespace {
const int64_t kStopWatchdogMs = 30000;
}

struct Fleet::Runtime {
	Destination dest;
	HealthMachine health;
	StatsTracker stats;
	uint64_t epoch = 0;
	bool has_out = false;
	bool pending_start = false;
	bool depends_on_main = false;
	int64_t stopping_since = -1;
	double target_kbps = 0;
	double bitrate = 0;
	double dropped = 0;
	std::string error;
	std::string notice;

	Runtime(const Destination &d, HealthMachine::Rng rng) : dest(d), health(HealthConfig(), rng) {}
};

Fleet::Fleet(Config &cfg, const SecretProtector &protector, const Clock &clock, OutputBackend &backend)
	: cfg_(cfg), protector_(protector), clock_(clock), backend_(backend),
	  rng_state_(static_cast<unsigned>(clock.now_ms() | 1) ^ 0x9E3779B9u)
{
	sync_with_config();
}

Fleet::~Fleet()
{
	shutdown();
}

Fleet::Runtime *Fleet::find(const std::string &id) const
{
	for (size_t i = 0; i < runtimes_.size(); ++i)
		if (runtimes_[i]->dest.id == id)
			return runtimes_[i].get();
	return nullptr;
}

void Fleet::sync_with_config()
{
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
		const Destination &o = r->dest;
		bool conn_changed = o.url != d.url || o.key_token != d.key_token || o.encoder != d.encoder ||
				    o.bitrate_kbps != d.bitrate_kbps || o.canvas != d.canvas || o.audio_track != d.audio_track ||
				    o.audio_bitrate_kbps != d.audio_bitrate_kbps || o.video_encoder != d.video_encoder ||
				    o.keyint_sec != d.keyint_sec || o.preset != d.preset || o.profile != d.profile ||
				    o.rate_control != d.rate_control || o.out_width != d.out_width || o.out_height != d.out_height ||
				    o.scale_type != d.scale_type || o.fps_div != d.fps_div || o.delay_sec != d.delay_sec ||
				    o.delay_preserve != d.delay_preserve || o.aitum_mode != d.aitum_mode ||
				    o.aitum_output != d.aitum_output || o.venc_extra.size() != d.venc_extra.size();
		if (conn_changed || !d.enabled)
			apply(*r, r->health.request_stop(clock_.now_ms()));
		r->dest = d;
	}
}

// ---------------------------------------------------------------------------------------------

void Fleet::on_main_stream_started()
{
	main_live_ = true;
	if (!cfg_.auto_start_with_stream)
		return;
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		if (r.dest.enabled && r.dest.auto_start)
			start_destination(r.dest.id);
	}
}

void Fleet::on_main_stream_stopping()
{
	// Outputs holding the main encoders must go first. Own-encoder outputs that auto-start with the
	// stream also stop with it; a manually started one keeps running.
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		if (uses_main_video(r.dest) || r.depends_on_main || r.dest.auto_start)
			apply(r, r.health.request_stop(clock_.now_ms()));
	}
}

void Fleet::on_main_stream_stopped()
{
	main_live_ = false;
}

void Fleet::start_destination(const std::string &id)
{
	Runtime *r = find(id);
	if (!r || !r->dest.enabled)
		return;
	if (uses_main_video(r->dest) && !main_live_) {
		r->error = "Start the main stream first: this destination shares its encoders.";
		return;
	}
	r->error.clear();
	apply(*r, r->health.request_start(clock_.now_ms()));
}

void Fleet::stop_destination(const std::string &id)
{
	Runtime *r = find(id);
	if (r)
		apply(*r, r->health.request_stop(clock_.now_ms()));
}

// One destination failing to start is recorded on that destination and the loop goes on.
void Fleet::start_all()
{
	for (size_t i = 0; i < runtimes_.size(); ++i)
		start_destination(runtimes_[i]->dest.id);
}

void Fleet::stop_all()
{
	for (size_t i = 0; i < runtimes_.size(); ++i)
		apply(*runtimes_[i], runtimes_[i]->health.request_stop(clock_.now_ms()));
}

void Fleet::shutdown()
{
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		r.health.request_stop(clock_.now_ms());
		release_attempt(r, true);
	}
}

// ---------------------------------------------------------------------------------------------

void Fleet::apply(Runtime &r, const Command &c)
{
	if (c.kind == CommandKind::StartOutput) {
		if (r.has_out) {
			r.pending_start = true; // previous attempt is still shutting down
			return;
		}
		launch(r);
	} else if (c.kind == CommandKind::StopOutput) {
		r.pending_start = false;
		if (r.has_out) {
			if (r.stopping_since < 0)
				r.stopping_since = clock_.now_ms();
			backend_.request_stop(r.dest.id);
		}
	}
}

void Fleet::fail_launch(Runtime &r, const std::string &why)
{
	std::vector<std::string> none;
	std::string safe = redact(why, none);
	r.error = safe;
	if (log_)
		log_(1, r.dest.name + ": " + safe);
	push_event(Event{r.dest.id, r.epoch, EvKind::Stopped, -2, safe});
}

bool Fleet::resolve(const Destination &base, Destination &eff, std::string &key, std::string &notice, std::string &err) const
{
	eff = base;
	key.clear();
	notice.clear();
	err.clear();
	std::string own_key;
	bool have_own = unlock_key(base, protector_, own_key);
	if (base.aitum_mode == "follow") {
		std::string text = aitum_ ? aitum_() : std::string();
		if (text.empty()) {
			notice = "Aitum settings not used: no aitum.json for the current OBS profile; using this destination's own settings";
		} else {
			AitumConfig ac = parse_aitum(text);
			if (ac.status != AitumStatus::Ok) {
				notice = "Aitum settings not used: " + ac.message + "; using this destination's own settings";
			} else if (const AitumOutput *o = find_aitum_output(ac, base.aitum_output)) {
				Destination mapped;
				std::string akey;
				std::vector<std::string> notes;
				if (apply_aitum(base, *o, mapped, akey, notes)) {
					eff = mapped;
					if (!akey.empty()) {
						secure_zero(own_key);
						own_key = akey;
						have_own = true;
					}
					secure_zero(akey);
					notice = notes.empty() ? std::string("following Aitum output \"") + base.aitum_output + "\"" : notes[0];
				} else {
					notice = "Aitum settings not used: " + (notes.empty() ? std::string("they do not validate") : notes.back()) +
						 "; using this destination's own settings";
				}
			} else {
				notice = "Aitum settings not used: no stream output named \"" + base.aitum_output +
					 "\" in aitum.json; using this destination's own settings";
			}
			wipe(ac);
		}
	}
	if (!have_own) {
		err = "stream key cannot be decrypted (saved by another Windows user or machine?): re-enter it";
		return false;
	}
	key = own_key;
	secure_zero(own_key);
	return true;
}

bool Fleet::launch(Runtime &r)
{
	r.epoch += 1;
	r.stats.reset();
	r.bitrate = 0;
	r.dropped = 0;
	r.depends_on_main = false;
	Destination eff;
	std::string key, err;
	if (!resolve(r.dest, eff, key, r.notice, err)) {
		fail_launch(r, err);
		return false;
	}
	if (!r.notice.empty() && log_)
		log_(0, r.dest.name + ": " + r.notice);
	double target = 0;
	bool dep = false;
	bool ok = backend_.launch(r.dest.id, r.epoch, eff, key, dep, target, err);
	secure_zero(key);
	r.has_out = backend_.has_output(r.dest.id);
	if (!ok) {
		fail_launch(r, err.empty() ? std::string("output failed to start") : err);
		return false;
	}
	r.depends_on_main = dep;
	r.target_kbps = target;
	return true;
}

void Fleet::release_attempt(Runtime &r, bool force)
{
	if (r.has_out || backend_.has_output(r.dest.id))
		backend_.release(r.dest.id, force);
	r.has_out = false;
	r.stopping_since = -1;
}

// ---------------------------------------------------------------------------------------------

void Fleet::push_event(const Event &e)
{
	std::lock_guard<std::mutex> lk(ev_m_);
	events_.push_back(e);
}

void Fleet::notify_started(const std::string &id, uint64_t epoch)
{
	push_event(Event{id, epoch, EvKind::Started, 0, std::string()});
}

void Fleet::notify_stopped(const std::string &id, uint64_t epoch, int code, const std::string &error)
{
	push_event(Event{id, epoch, EvKind::Stopped, code, error});
}

void Fleet::drain_events()
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
			continue;
		if (e.kind == EvKind::Started) {
			apply(*r, r->health.on_output_started(now));
		} else {
			std::vector<std::string> none;
			std::string err = redact(e.error, none);
			if (!err.empty())
				r->error = err;
			bool user_or_expected = (r->health.state() == State::Idle);
			if (!user_or_expected && log_)
				log_(1, r->dest.name + ": output stopped (" + (err.empty() ? "no error" : err) + ")");
			apply(*r, r->health.on_output_stopped(now, e.code != kOutputSuccess, err));
			release_attempt(*r, false);
			if (r->pending_start && r->health.state() == State::Connecting) {
				r->pending_start = false;
				launch(*r);
			}
		}
	}
}

void Fleet::sample(Runtime &r, int64_t now)
{
	if (!r.has_out)
		return;
	State s = r.health.state();
	if (s != State::Live && s != State::Degraded)
		return;
	Sample sm;
	if (!backend_.sample(r.dest.id, sm))
		return;
	sm.t_ms = now;
	r.stats.add(sm);
	r.bitrate = r.stats.bitrate_kbps(10000);
	r.dropped = r.stats.dropped_pct(10000);
	apply(r, r.health.on_sample(now, r.bitrate, r.dropped, r.target_kbps));
}

void Fleet::tick()
{
	int64_t now = clock_.now_ms();
	drain_events();
	for (size_t i = 0; i < runtimes_.size(); ++i) {
		Runtime &r = *runtimes_[i];
		apply(r, r.health.tick(now));
		sample(r, now);
		if (r.has_out && r.stopping_since >= 0 && now - r.stopping_since > kStopWatchdogMs) {
			if (log_)
				log_(2, r.dest.name + ": output did not stop in time; abandoning it");
			push_event(Event{r.dest.id, r.epoch, EvKind::Stopped, -4, "output did not stop in time"});
			r.stopping_since = now;
		}
	}
}

// ---------------------------------------------------------------------------------------------

std::vector<RuntimeView> Fleet::views() const
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
		rv.notice = r.notice;
		bool active = rv.state != State::Idle && rv.state != State::Failed;
		rv.can_stop = active;
		rv.can_start = !active && r.dest.enabled && (!uses_main_video(r.dest) || main_live_);
		v.push_back(rv);
	}
	return v;
}

std::vector<OverlayEntry> Fleet::overlay_entries() const
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

// ---------------------------------------------------------------------------------------------

std::vector<std::string> Fleet::double_stream_warnings() const
{
	std::vector<TargetRef> t;
	for (size_t i = 0; i < cfg_.destinations.size(); ++i) {
		const Destination &d = cfg_.destinations[i];
		if (!d.enabled)
			continue;
		TargetRef x;
		x.where = "\"" + d.name + "\" (this plugin)";
		x.url = d.url;
		unlock_key(d, protector_, x.key);
		t.push_back(x);
	}
	if (aitum_) {
		std::string text = aitum_();
		if (!text.empty()) {
			AitumConfig ac = parse_aitum(text);
			if (ac.status == AitumStatus::Ok) {
				for (size_t i = 0; i < ac.outputs.size(); ++i) {
					const AitumOutput &o = ac.outputs[i];
					if (!o.enabled || o.server.empty() || o.key.empty())
						continue;
					TargetRef x;
					x.where = "Aitum output \"" + o.name + "\" (settings file)";
					x.url = o.server;
					x.key = o.key;
					t.push_back(x);
				}
			}
			wipe(ac);
		}
	}
	if (external_) {
		std::vector<TargetRef> ex = external_();
		t.insert(t.end(), ex.begin(), ex.end());
	}
	std::vector<std::string> w = find_double_streaming(t);
	for (size_t i = 0; i < t.size(); ++i)
		secure_zero(t[i].key);
	return w;
}

bool Fleet::copy_from_aitum(const Destination &base, const std::string &output_name, Destination &out,
			    std::vector<std::string> &notes, std::string &err) const
{
	std::string text = aitum_ ? aitum_() : std::string();
	if (text.empty()) {
		err = "no aitum.json for the current OBS profile";
		return false;
	}
	AitumConfig ac = parse_aitum(text);
	if (ac.status != AitumStatus::Ok) {
		err = ac.message;
		return false;
	}
	const AitumOutput *o = find_aitum_output(ac, output_name);
	if (!o) {
		err = "no stream output named \"" + output_name + "\" in aitum.json";
		wipe(ac);
		return false;
	}
	std::string key;
	bool ok = apply_aitum(base, *o, out, key, notes);
	if (ok && !key.empty()) {
		std::string token;
		if (protector_.protect(key, token))
			out.key_token = token;
		else
			notes.push_back("the Aitum key could not be encrypted: enter it here");
	}
	secure_zero(key);
	wipe(ac);
	if (!ok) {
		err = notes.empty() ? std::string("the Aitum settings do not validate") : notes.back();
		return false;
	}
	out.aitum_mode = "copy"; // remembered for display; the settings are our own from here on
	out.aitum_output = output_name;
	return true;
}

std::vector<std::string> Fleet::aitum_output_names(std::string &message) const
{
	std::vector<std::string> names;
	message.clear();
	std::string text = aitum_ ? aitum_() : std::string();
	if (text.empty()) {
		message = "No aitum.json found for the current OBS profile.";
		return names;
	}
	AitumConfig ac = parse_aitum(text);
	if (ac.status != AitumStatus::Ok) {
		message = ac.message;
		wipe(ac);
		return names;
	}
	for (size_t i = 0; i < ac.outputs.size(); ++i)
		names.push_back(ac.outputs[i].name);
	if (names.empty())
		message = "Aitum has no stream outputs in this profile.";
	wipe(ac);
	return names;
}

} // namespace oms
