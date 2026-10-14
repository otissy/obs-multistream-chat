// OBS-side owner of one rtmp output per destination. All methods run on the UI thread except
// the libobs signal callbacks, which only push into the event queue (see output_manager.cpp).
#pragma once
#include "config.hpp"
#include "health.hpp"
#include "state_feed.hpp"
#include "stats.hpp"

#include <obs.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace oms {

struct RuntimeView {
	Destination dest; // key and key_token are blanked in views
	State state = State::Idle;
	double bitrate_kbps = 0;
	double dropped_pct = 0;
	int64_t uptime_ms = 0;
	int attempts = 0;
	int64_t retry_in_ms = -1;
	std::string detail;     // degraded reason
	std::string last_error; // already redacted
	bool can_start = false;
	bool can_stop = false;
};

class OutputManager {
public:
	OutputManager(Config &cfg, const SecretProtector &protector, const Clock &clock);
	~OutputManager();

	// Destinations list changed in the config (add / edit / remove / enable).
	void sync_with_config();

	void on_main_stream_started();
	void on_main_stream_stopping();
	void on_main_stream_stopped();
	bool main_live() const { return main_live_; }

	void start_destination(const std::string &id);
	void stop_destination(const std::string &id);
	void start_all();
	void stop_all();
	// Hard stop for module unload / OBS exit: no waiting for stop signals.
	void shutdown();

	// Every 500 ms from the UI thread.
	void tick();

	std::vector<RuntimeView> views() const;
	std::vector<OverlayEntry> overlay_entries() const;
	// "" when nothing is wrong, else a one-line explanation of the bottleneck.
	std::string bottleneck_banner() const { return banner_; }

private:
	struct Ctx; // heap object handed to libobs signals; outlives the output
	struct Runtime;

	enum class EvKind { Started, Stopped };
	struct Event {
		std::string id;
		uint64_t epoch;
		EvKind kind;
		int code;
		std::string error;
	};

	Runtime *find(const std::string &id) const;
	void apply(Runtime &r, const Command &c);
	bool launch(Runtime &r);
	void release_attempt(Runtime &r, bool force);
	void fail_launch(Runtime &r, const std::string &why);
	void drain_events();
	void sample(Runtime &r, int64_t now);
	void update_banner(int64_t now);
	static void sig_start(void *data, calldata_t *cd);
	static void sig_stop(void *data, calldata_t *cd);
	void push_event(const Event &e);

	Config &cfg_;
	const SecretProtector &protector_;
	const Clock &clock_;
	std::vector<std::unique_ptr<Runtime>> runtimes_;
	std::mutex ev_m_;
	std::vector<Event> events_;
	bool main_live_ = false;
	std::string banner_;
	struct MainSample {
		int64_t t;
		uint64_t frames;
		uint64_t dropped;
		uint32_t skipped;
		uint32_t lagged;
		uint32_t total;
	};
	std::deque<MainSample> main_samples_;
	unsigned rng_state_;
};

} // namespace oms
