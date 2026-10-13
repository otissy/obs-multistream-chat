// L2: stats tracker and health state machine, driven by a mock clock.
#include "oms_test.hpp"

#include "../../../src/core/clock.hpp"
#include "../../../src/core/health.hpp"
#include "../../../src/core/stats.hpp"

using namespace oms;

static Sample S(int64_t t, uint64_t bytes, int64_t frames, int64_t dropped)
{
	Sample s;
	s.t_ms = t;
	s.bytes = bytes;
	s.frames = frames;
	s.dropped = dropped;
	return s;
}

// ---- stats ---------------------------------------------------------------------------------

TEST(L2, stats_bitrate_from_byte_deltas)
{
	StatsTracker st;
	CHECK_NEAR(st.bitrate_kbps(10000), 0.0, 1e-9); // empty
	st.add(S(0, 0, 0, 0));
	CHECK_NEAR(st.bitrate_kbps(10000), 0.0, 1e-9); // one sample
	st.add(S(1000, 750000, 60, 0));                // 750 kB in 1 s = 6000 kbps
	CHECK_NEAR(st.bitrate_kbps(10000), 6000.0, 1e-6);
	st.add(S(2000, 1500000, 120, 0));
	CHECK_NEAR(st.bitrate_kbps(10000), 6000.0, 1e-6);
}

TEST(L2, stats_window_limits_history)
{
	StatsTracker st;
	st.add(S(0, 0, 0, 0));
	st.add(S(1000, 1000000, 60, 0)); // 8000 kbps early on
	st.add(S(2000, 1000000, 120, 0));
	st.add(S(3000, 1000000, 180, 0)); // stalled since t=1000
	CHECK_NEAR(st.bitrate_kbps(1000), 0.0, 1e-6);   // last second only: nothing sent
	CHECK(st.bitrate_kbps(10000) > 2000.0);          // whole history: average over 3 s
}

TEST(L2, stats_dropped_percent)
{
	StatsTracker st;
	st.add(S(0, 0, 0, 0));
	st.add(S(1000, 1, 100, 5));
	CHECK_NEAR(st.dropped_pct(10000), 5.0, 1e-9);
	CHECK_NEAR(st.dropped_pct_total(), 5.0, 1e-9);
	st.add(S(2000, 2, 100, 5)); // no frame progress
	CHECK_NEAR(st.dropped_pct(500), 0.0, 1e-9);
}

TEST(L2, stats_counter_reset_means_new_connection)
{
	StatsTracker st;
	st.add(S(0, 0, 0, 0));
	st.add(S(1000, 1000000, 100, 3));
	st.add(S(2000, 10, 1, 0)); // counters went backwards: reconnect
	CHECK_EQ(st.count(), size_t(1));
	CHECK_NEAR(st.bitrate_kbps(10000), 0.0, 1e-9);
}

TEST(L2, stats_ring_capacity_bounds_memory)
{
	StatsTracker st(10);
	for (int i = 0; i < 1000; ++i)
		st.add(S(i * 1000, uint64_t(i) * 1000, i, 0));
	CHECK_EQ(st.count(), size_t(10));
}

// ---- backoff -------------------------------------------------------------------------------

TEST(L2, backoff_doubles_then_caps)
{
	HealthConfig c;
	c.base_backoff_ms = 1000;
	c.max_backoff_ms = 60000;
	c.jitter = 0.0;
	CHECK_EQ(HealthMachine::backoff_for(1, c, 0.5), int64_t(1000));
	CHECK_EQ(HealthMachine::backoff_for(2, c, 0.5), int64_t(2000));
	CHECK_EQ(HealthMachine::backoff_for(3, c, 0.5), int64_t(4000));
	CHECK_EQ(HealthMachine::backoff_for(6, c, 0.5), int64_t(32000));
	CHECK_EQ(HealthMachine::backoff_for(7, c, 0.5), int64_t(60000));
	CHECK_EQ(HealthMachine::backoff_for(500, c, 0.5), int64_t(60000)); // no overflow
	CHECK_EQ(HealthMachine::backoff_for(0, c, 0.5), int64_t(1000));    // clamped
}

TEST(L2, backoff_jitter_stays_inside_bounds_and_cap)
{
	HealthConfig c;
	c.base_backoff_ms = 1000;
	c.max_backoff_ms = 60000;
	c.jitter = 0.25;
	for (int attempt = 1; attempt <= 12; ++attempt) {
		for (int k = 0; k <= 10; ++k) {
			double r = k / 10.0 * 0.999999;
			int64_t v = HealthMachine::backoff_for(attempt, c, r);
			int64_t nominal = 1000;
			for (int i = 1; i < attempt && nominal < 60000; ++i)
				nominal *= 2;
			if (nominal > 60000)
				nominal = 60000;
			CHECK(v >= int64_t(double(nominal) * 0.75) - 1);
			CHECK(v <= 60000);
			CHECK(v <= int64_t(double(nominal) * 1.25) + 1);
		}
	}
}

// ---- state machine -------------------------------------------------------------------------

static HealthConfig fast_cfg()
{
	HealthConfig c;
	c.jitter = 0.0;
	c.connect_timeout_ms = 10000;
	c.base_backoff_ms = 1000;
	c.max_backoff_ms = 8000;
	c.max_attempts = 3;
	c.stable_ms = 30000;
	c.warmup_ms = 5000;
	c.degrade_after_ms = 3000;
	c.recover_ms = 4000;
	return c;
}

TEST(L2, health_happy_path_idle_connecting_live)
{
	MockClock clk(1000);
	HealthMachine h(fast_cfg());
	CHECK(h.state() == State::Idle);
	Command c = h.request_start(clk.now_ms());
	CHECK(c.kind == CommandKind::StartOutput);
	CHECK(h.state() == State::Connecting);
	clk.advance(800);
	c = h.on_output_started(clk.now_ms());
	CHECK(c.kind == CommandKind::None);
	CHECK(h.state() == State::Live);
	clk.advance(5000);
	CHECK_EQ(h.uptime_ms(clk.now_ms()), int64_t(5000));
	c = h.request_stop(clk.now_ms());
	CHECK(c.kind == CommandKind::StopOutput);
	CHECK(h.state() == State::Idle);
	c = h.on_output_stopped(clk.now_ms(), false, "");
	CHECK(c.kind == CommandKind::None);
	CHECK(h.state() == State::Idle); // the expected stop does not start a retry
	CHECK_EQ(h.next_retry_ms(), int64_t(-1));
}

TEST(L2, health_connect_timeout_leads_to_reconnecting)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	clk.advance(9999);
	CHECK(h.tick(clk.now_ms()).kind == CommandKind::None);
	CHECK(h.state() == State::Connecting);
	clk.advance(1);
	Command c = h.tick(clk.now_ms());
	CHECK(c.kind == CommandKind::StopOutput);
	CHECK(h.state() == State::Reconnecting);
	CHECK_EQ(h.attempts(), 1);
	CHECK_EQ(h.next_retry_ms(), clk.now_ms() + 1000);
	// the stop we asked for arrives: ignored
	CHECK(h.on_output_stopped(clk.now_ms(), false, "").kind == CommandKind::None);
	CHECK(h.state() == State::Reconnecting);
}

TEST(L2, health_reconnect_uses_growing_backoff_then_fails)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	int64_t expected[3] = {1000, 2000, 4000};
	for (int i = 0; i < 3; ++i) {
		h.on_output_stopped(clk.now_ms(), true, "connect failed");
		CHECK(h.state() == State::Reconnecting);
		CHECK_EQ(h.attempts(), i + 1);
		CHECK_EQ(h.next_retry_ms(), clk.now_ms() + expected[i]);
		CHECK(h.tick(clk.now_ms() + expected[i] - 1).kind == CommandKind::None);
		clk.advance(expected[i]);
		Command c = h.tick(clk.now_ms());
		CHECK(c.kind == CommandKind::StartOutput);
		CHECK(h.state() == State::Connecting);
	}
	h.on_output_stopped(clk.now_ms(), true, "connect failed"); // 4th failure > max_attempts(3)
	CHECK(h.state() == State::Failed);
	CHECK_EQ(h.next_retry_ms(), int64_t(-1));
	CHECK(h.last_error() == "connect failed");
	clk.advance(3600000);
	CHECK(h.tick(clk.now_ms()).kind == CommandKind::None); // Failed stays failed
	// the user can restart it
	CHECK(h.request_start(clk.now_ms()).kind == CommandKind::StartOutput);
	CHECK_EQ(h.attempts(), 0);
	CHECK(h.state() == State::Connecting);
}

TEST(L2, health_backoff_never_exceeds_cap_in_machine)
{
	HealthConfig c = fast_cfg();
	c.max_attempts = 50;
	MockClock clk;
	HealthMachine h(c);
	h.request_start(clk.now_ms());
	for (int i = 0; i < 20; ++i) {
		h.on_output_stopped(clk.now_ms(), true, "x");
		CHECK(h.next_retry_ms() - clk.now_ms() <= 8000);
		clk.set(h.next_retry_ms());
		h.tick(clk.now_ms());
	}
}

TEST(L2, health_stable_live_resets_attempt_counter)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	h.on_output_stopped(clk.now_ms(), true, "e");
	h.on_output_stopped(clk.now_ms(), true, "e"); // ignored in Reconnecting
	CHECK_EQ(h.attempts(), 1);
	clk.set(h.next_retry_ms());
	h.tick(clk.now_ms());
	h.on_output_started(clk.now_ms());
	CHECK(h.state() == State::Live);
	clk.advance(29000);
	h.tick(clk.now_ms());
	CHECK_EQ(h.attempts(), 1);
	clk.advance(1000);
	h.tick(clk.now_ms());
	CHECK_EQ(h.attempts(), 0);
}

TEST(L2, health_failure_after_long_live_is_a_fresh_problem)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	h.on_output_started(clk.now_ms());
	clk.advance(60000);
	h.on_output_stopped(clk.now_ms(), true, "dropped");
	CHECK_EQ(h.attempts(), 1); // not 2: it ran fine for 60 s
	CHECK_EQ(h.next_retry_ms(), clk.now_ms() + 1000);
}

TEST(L2, health_user_stop_is_terminal_until_start)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	h.on_output_stopped(clk.now_ms(), true, "e");
	CHECK(h.state() == State::Reconnecting);
	Command c = h.request_stop(clk.now_ms());
	CHECK(c.kind == CommandKind::None); // nothing running to stop
	CHECK(h.state() == State::Idle);
	CHECK_EQ(h.next_retry_ms(), int64_t(-1));
	clk.advance(3600000);
	CHECK(h.tick(clk.now_ms()).kind == CommandKind::None);
	CHECK(h.state() == State::Idle);
}

TEST(L2, health_late_connect_after_stop_is_stopped_again)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	h.request_stop(clk.now_ms());
	Command c = h.on_output_started(clk.now_ms()); // output came up just after the user stopped
	CHECK(c.kind == CommandKind::StopOutput);
	CHECK(h.state() == State::Idle);
}

TEST(L2, health_degraded_on_drops_and_recovers_with_hysteresis)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	h.on_output_started(clk.now_ms());
	// warm-up: bad samples are ignored for the first 5 s
	clk.advance(1000);
	h.on_sample(clk.now_ms(), 100, 50.0, 6000);
	CHECK(h.state() == State::Live);
	clk.advance(5000);
	h.on_sample(clk.now_ms(), 6000, 9.0, 6000); // bad starts
	CHECK(h.state() == State::Live);
	clk.advance(2999);
	h.on_sample(clk.now_ms(), 6000, 9.0, 6000);
	CHECK(h.state() == State::Live);
	clk.advance(1);
	h.on_sample(clk.now_ms(), 6000, 9.0, 6000);
	CHECK(h.state() == State::Degraded);
	CHECK(h.detail().find("dropping") != std::string::npos);
	// a single good sample is not enough
	clk.advance(1000);
	h.on_sample(clk.now_ms(), 6000, 0.0, 6000);
	clk.advance(1000);
	h.on_sample(clk.now_ms(), 6000, 9.0, 6000); // bad again resets recovery
	CHECK(h.state() == State::Degraded);
	clk.advance(1000);
	h.on_sample(clk.now_ms(), 6000, 0.0, 6000);
	clk.advance(3999);
	h.on_sample(clk.now_ms(), 6000, 0.0, 6000);
	CHECK(h.state() == State::Degraded);
	clk.advance(1);
	h.on_sample(clk.now_ms(), 6000, 0.0, 6000);
	CHECK(h.state() == State::Live);
	CHECK(h.detail().empty());
}

TEST(L2, health_degraded_on_low_bitrate_only_when_target_known)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	h.on_output_started(clk.now_ms());
	clk.advance(6000);
	h.on_sample(clk.now_ms(), 100, 0.0, 0); // target unknown: bitrate not judged
	clk.advance(5000);
	h.on_sample(clk.now_ms(), 100, 0.0, 0);
	CHECK(h.state() == State::Live);
	h.on_sample(clk.now_ms(), 1000, 0.0, 6000); // 1000 < 3000
	clk.advance(3000);
	h.on_sample(clk.now_ms(), 1000, 0.0, 6000);
	CHECK(h.state() == State::Degraded);
	CHECK(h.detail().find("bitrate") != std::string::npos);
}

TEST(L2, health_samples_ignored_outside_live)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.on_sample(clk.now_ms(), 0, 100.0, 6000);
	CHECK(h.state() == State::Idle);
	h.request_start(clk.now_ms());
	h.on_sample(clk.now_ms(), 0, 100.0, 6000);
	CHECK(h.state() == State::Connecting);
}

TEST(L2, health_unexpected_clean_close_is_a_failure)
{
	MockClock clk;
	HealthMachine h(fast_cfg());
	h.request_start(clk.now_ms());
	h.on_output_started(clk.now_ms());
	h.on_output_stopped(clk.now_ms(), false, "");
	CHECK(h.state() == State::Reconnecting);
	CHECK(h.last_error() == "connection closed");
}

TEST(L2, health_jitter_uses_injected_rng_and_is_bounded)
{
	HealthConfig c = fast_cfg();
	c.jitter = 0.25;
	double r = 0.0;
	MockClock clk;
	HealthMachine h(c, [&r]() { return r; });
	h.request_start(clk.now_ms());
	h.on_output_stopped(clk.now_ms(), true, "e");
	CHECK_EQ(h.next_retry_ms(), int64_t(750)); // rng 0.0 => 1000 * 0.75
	r = 0.9999;
	h.request_start(clk.now_ms()); // retry now
	h.on_output_stopped(clk.now_ms(), true, "e");
	CHECK(h.next_retry_ms() - clk.now_ms() <= 2500); // attempt 2: 2000 * up to 1.25
	CHECK(h.next_retry_ms() - clk.now_ms() >= 2000);
}

TEST(L2, health_state_names)
{
	CHECK_EQ(std::string(state_name(State::Idle)), std::string("idle"));
	CHECK_EQ(std::string(state_name(State::Connecting)), std::string("connecting"));
	CHECK_EQ(std::string(state_name(State::Live)), std::string("live"));
	CHECK_EQ(std::string(state_name(State::Degraded)), std::string("degraded"));
	CHECK_EQ(std::string(state_name(State::Reconnecting)), std::string("reconnecting"));
	CHECK_EQ(std::string(state_name(State::Failed)), std::string("failed"));
}
