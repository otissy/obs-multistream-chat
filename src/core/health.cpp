#include "health.hpp"

#include <cstdio>

namespace oms {

const char *state_name(State s)
{
	switch (s) {
	case State::Idle: return "idle";
	case State::Connecting: return "connecting";
	case State::Live: return "live";
	case State::Degraded: return "degraded";
	case State::Reconnecting: return "reconnecting";
	case State::Failed: return "failed";
	}
	return "idle";
}

HealthMachine::HealthMachine(const HealthConfig &cfg, Rng rng) : cfg_(cfg), rng_(rng) {}

double HealthMachine::next_rand()
{
	if (!rng_)
		return 0.5;
	double r = rng_();
	if (r < 0.0)
		r = 0.0;
	if (r >= 1.0)
		r = 0.999999;
	return r;
}

int64_t HealthMachine::backoff_for(int attempt, const HealthConfig &cfg, double rand01)
{
	if (attempt < 1)
		attempt = 1;
	int64_t d = cfg.base_backoff_ms < 1 ? 1 : cfg.base_backoff_ms;
	for (int i = 1; i < attempt && d < cfg.max_backoff_ms; ++i)
		d *= 2;
	if (d > cfg.max_backoff_ms)
		d = cfg.max_backoff_ms;
	double j = cfg.jitter < 0 ? 0 : (cfg.jitter > 1 ? 1 : cfg.jitter);
	double f = 1.0 + j * (2.0 * rand01 - 1.0);
	int64_t out = static_cast<int64_t>(double(d) * f);
	if (out > cfg.max_backoff_ms)
		out = cfg.max_backoff_ms;
	if (out < 1)
		out = 1;
	return out;
}

int64_t HealthMachine::uptime_ms(int64_t now) const
{
	if ((state_ == State::Live || state_ == State::Degraded) && live_since_ms_ >= 0)
		return now - live_since_ms_;
	return 0;
}

void HealthMachine::enter(State s, int64_t now)
{
	state_ = s;
	since_ms_ = now;
	if (s != State::Degraded)
		detail_.clear();
}

void HealthMachine::register_failure(int64_t now, const std::string &error)
{
	bool was_live = (state_ == State::Live || state_ == State::Degraded);
	if (was_live && live_since_ms_ >= 0 && now - live_since_ms_ >= cfg_.stable_ms)
		attempts_ = 0; // it ran fine for a while: this is a fresh problem, not a storm
	live_since_ms_ = -1;
	bad_since_ms_ = -1;
	good_since_ms_ = -1;
	last_error_ = error;
	++attempts_;
	if (attempts_ > cfg_.max_attempts) {
		next_retry_ms_ = -1;
		enter(State::Failed, now);
		return;
	}
	next_retry_ms_ = now + backoff_for(attempts_, cfg_, next_rand());
	enter(State::Reconnecting, now);
}

Command HealthMachine::request_start(int64_t now)
{
	Command c;
	switch (state_) {
	case State::Connecting:
	case State::Live:
	case State::Degraded:
		return c;
	case State::Reconnecting: // "retry now"
		next_retry_ms_ = -1;
		enter(State::Connecting, now);
		output_active_ = true;
		c.kind = CommandKind::StartOutput;
		return c;
	case State::Idle:
	case State::Failed:
		attempts_ = 0;
		next_retry_ms_ = -1;
		last_error_.clear();
		enter(State::Connecting, now);
		output_active_ = true;
		c.kind = CommandKind::StartOutput;
		return c;
	}
	return c;
}

Command HealthMachine::request_stop(int64_t now)
{
	Command c;
	next_retry_ms_ = -1;
	live_since_ms_ = -1;
	bad_since_ms_ = -1;
	good_since_ms_ = -1;
	attempts_ = 0;
	enter(State::Idle, now);
	if (output_active_)
		c.kind = CommandKind::StopOutput; // idempotent: the glue ignores a stop with nothing running
	return c;
}

Command HealthMachine::on_output_started(int64_t now)
{
	Command c;
	switch (state_) {
	case State::Connecting:
		enter(State::Live, now);
		live_since_ms_ = now;
		bad_since_ms_ = -1;
		good_since_ms_ = -1;
		output_active_ = true;
		return c;
	case State::Live:
	case State::Degraded:
		return c;
	case State::Idle:
	case State::Failed:
	case State::Reconnecting:
		// We did not want it running (user stop, or it connected just after we gave up).
		output_active_ = true;
		c.kind = CommandKind::StopOutput;
		return c;
	}
	return c;
}

Command HealthMachine::on_output_stopped(int64_t now, bool had_error, const std::string &error)
{
	Command c;
	output_active_ = false;
	switch (state_) {
	case State::Idle:
	case State::Failed:
	case State::Reconnecting:
		return c; // expected (stop we asked for) or already handled
	case State::Connecting:
	case State::Live:
	case State::Degraded:
		register_failure(now, had_error && !error.empty() ? error : std::string("connection closed"));
		return c;
	}
	return c;
}

Command HealthMachine::on_sample(int64_t now, double bitrate_kbps, double dropped_pct, double target_kbps)
{
	Command c;
	if (state_ != State::Live && state_ != State::Degraded)
		return c;
	if (live_since_ms_ >= 0 && now - live_since_ms_ < cfg_.warmup_ms)
		return c;
	bool low_rate = target_kbps > 0 && bitrate_kbps < target_kbps * cfg_.degraded_min_ratio;
	bool drops = dropped_pct >= cfg_.degraded_drop_pct;
	bool bad = low_rate || drops;

	if (state_ == State::Live) {
		if (!bad) {
			bad_since_ms_ = -1;
			return c;
		}
		if (bad_since_ms_ < 0)
			bad_since_ms_ = now;
		if (now - bad_since_ms_ >= cfg_.degrade_after_ms) {
			char buf[96];
			if (drops)
				std::snprintf(buf, sizeof buf, "dropping %.1f%% of frames", dropped_pct);
			else
				std::snprintf(buf, sizeof buf, "bitrate %.0f of %.0f kbps", bitrate_kbps, target_kbps);
			enter(State::Degraded, now);
			detail_ = buf;
			good_since_ms_ = -1;
		}
		return c;
	}
	// Degraded
	if (bad) {
		good_since_ms_ = -1;
		return c;
	}
	if (good_since_ms_ < 0)
		good_since_ms_ = now;
	if (now - good_since_ms_ >= cfg_.recover_ms) {
		enter(State::Live, now);
		bad_since_ms_ = -1;
		good_since_ms_ = -1;
	}
	return c;
}

Command HealthMachine::tick(int64_t now)
{
	Command c;
	switch (state_) {
	case State::Connecting:
		if (now - since_ms_ >= cfg_.connect_timeout_ms) {
			register_failure(now, "connect timed out");
			c.kind = CommandKind::StopOutput; // abandon the half-open attempt; its stop event is ignored
		}
		return c;
	case State::Reconnecting:
		if (next_retry_ms_ >= 0 && now >= next_retry_ms_) {
			next_retry_ms_ = -1;
			enter(State::Connecting, now);
			output_active_ = true;
			c.kind = CommandKind::StartOutput;
		}
		return c;
	case State::Live:
	case State::Degraded:
		if (attempts_ > 0 && live_since_ms_ >= 0 && now - live_since_ms_ >= cfg_.stable_ms)
			attempts_ = 0;
		return c;
	default:
		return c;
	}
}

} // namespace oms
