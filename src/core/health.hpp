// Per-destination connection health state machine. Pure logic: no OBS, no threads, no
// wall clock. The caller feeds events and a monotonic time, and performs the returned Command.
//
//        request_start             on_output_started
//   Idle ------------> Connecting ------------------> Live <----> Degraded
//    ^                  |    ^                         |            |
//    | request_stop     |    | retry timer due         | output stopped / connect timeout
//    |                  v    |                         v
//    +------------- Reconnecting <---------------------+
//    |                  | attempts > max_attempts
//    +--- Failed <------+
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace oms {

enum class State { Idle, Connecting, Live, Degraded, Reconnecting, Failed };

const char *state_name(State s); // "idle" "connecting" "live" "degraded" "reconnecting" "failed"

enum class CommandKind { None, StartOutput, StopOutput };

struct Command {
	CommandKind kind = CommandKind::None;
};

struct HealthConfig {
	int64_t connect_timeout_ms = 15000;
	int64_t base_backoff_ms = 1000;
	int64_t max_backoff_ms = 60000;
	double jitter = 0.25;       // +/- fraction applied to each backoff
	int max_attempts = 8;       // consecutive failures before Failed
	int64_t stable_ms = 30000;  // live this long => failure counter resets
	double degraded_drop_pct = 3.0;
	double degraded_min_ratio = 0.5; // bitrate below ratio * target counts as bad (target > 0)
	int64_t warmup_ms = 8000;        // after going Live, ignore samples (rate ramps up)
	int64_t degrade_after_ms = 5000; // bad continuously this long => Degraded
	int64_t recover_ms = 10000;      // good continuously this long => Live again
};

class HealthMachine {
public:
	// rng returns a number in [0,1). Default 0.5 gives zero jitter (deterministic).
	using Rng = std::function<double()>;

	explicit HealthMachine(const HealthConfig &cfg = HealthConfig(), Rng rng = Rng());

	State state() const { return state_; }
	int attempts() const { return attempts_; }
	// Absolute time of the next retry, or -1 when none is scheduled.
	int64_t next_retry_ms() const { return next_retry_ms_; }
	int64_t state_since_ms() const { return since_ms_; }
	int64_t uptime_ms(int64_t now) const;
	const std::string &last_error() const { return last_error_; }
	// Why the state is Degraded ("dropping 7.5% of frames", "bitrate 400 of 6000 kbps"), else "".
	const std::string &detail() const { return detail_; }

	Command request_start(int64_t now);
	Command request_stop(int64_t now);
	Command on_output_started(int64_t now);
	// error text must already be free of secrets (the caller passes it through redact()).
	Command on_output_stopped(int64_t now, bool had_error, const std::string &error);
	Command on_sample(int64_t now, double bitrate_kbps, double dropped_pct, double target_kbps);
	Command tick(int64_t now);

	// attempt >= 1. Capped exponential with jitter; never exceeds max_backoff_ms, never below 1.
	static int64_t backoff_for(int attempt, const HealthConfig &cfg, double rand01);

private:
	void enter(State s, int64_t now);
	void register_failure(int64_t now, const std::string &error);
	double next_rand();

	HealthConfig cfg_;
	Rng rng_;
	State state_ = State::Idle;
	int64_t since_ms_ = 0;
	int attempts_ = 0;
	int64_t next_retry_ms_ = -1;
	int64_t live_since_ms_ = -1;
	int64_t bad_since_ms_ = -1;
	int64_t good_since_ms_ = -1;
	bool output_active_ = false; // we believe an output exists/is starting and has not reported stop
	std::string last_error_;
	std::string detail_;
};

} // namespace oms
