// The per-destination output fleet, independent of libobs. One Runtime per destination (state
// machine, stats, attempt bookkeeping) and nothing shared between them except an event queue, so
// any number of outputs run side by side and one failing output never touches another.
// The libobs side implements OutputBackend; tests implement it with mocks.
#pragma once
#include "aitum.hpp"
#include "clock.hpp"
#include "config.hpp"
#include "health.hpp"
#include "secret.hpp"
#include "state_feed.hpp"
#include "stats.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace oms {

constexpr int kOutputSuccess = 0; // OBS_OUTPUT_SUCCESS

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
	std::string notice;     // e.g. "Aitum settings not used: file not found" (no secrets)
	bool can_start = false;
	bool can_stop = false;
};

class Fleet;

// level: 0 info, 1 warning, 2 error. The text is already redacted.
using LogFn = std::function<void(int level, const std::string &msg)>;

class OutputBackend {
public:
	virtual ~OutputBackend() {}
	// Builds and starts one output. Must not block on the network. `key` is plaintext and must not
	// outlive the call. On true the backend later reports through Fleet::notify_started/stopped.
	// On false `err` is a short secret-free reason. Either way the backend may hold an attempt
	// (has_output) that release() drops. depends_on_main / target_kbps are filled on success.
	virtual bool launch(const std::string &id, uint64_t epoch, const Destination &eff, const std::string &key,
			    bool &depends_on_main, double &target_kbps, std::string &err) = 0;
	virtual void request_stop(const std::string &id) = 0; // asynchronous, never blocks the caller
	virtual bool sample(const std::string &id, Sample &out) = 0;
	virtual void release(const std::string &id, bool force) = 0;
	virtual bool has_output(const std::string &id) const = 0;
};

// Returns the text of Aitum's aitum.json for the active profile ("" when there is none).
using AitumTextProvider = std::function<std::string()>;
// Outputs other than ours that are streaming right now (libobs side), for double-stream checks.
using ExternalTargets = std::function<std::vector<TargetRef>()>;

class Fleet {
public:
	Fleet(Config &cfg, const SecretProtector &protector, const Clock &clock, OutputBackend &backend);
	~Fleet();

	void set_aitum_provider(AitumTextProvider p) { aitum_ = p; }
	void set_external_targets(ExternalTargets p) { external_ = p; }
	void set_logger(LogFn f) { log_ = f; }

	void sync_with_config();
	void on_main_stream_started();
	void on_main_stream_stopping();
	void on_main_stream_stopped();
	bool main_live() const { return main_live_; }

	void start_destination(const std::string &id);
	void stop_destination(const std::string &id);
	void start_all();
	void stop_all();
	void shutdown();
	void tick();

	// Thread-safe: called from libobs signal threads (or mock threads).
	void notify_started(const std::string &id, uint64_t epoch);
	void notify_stopped(const std::string &id, uint64_t epoch, int code, const std::string &error);

	std::vector<RuntimeView> views() const;
	std::vector<OverlayEntry> overlay_entries() const;

	// Same server+key active in two places. Unlocks keys transiently; never returns values.
	std::vector<std::string> double_stream_warnings() const;

	// Resolves what a launch will use: own settings, or Aitum's when aitum_mode == "follow".
	// Public so the dock can preview and tests can drive it.
	bool resolve(const Destination &base, Destination &eff, std::string &key, std::string &notice, std::string &err) const;

	// "Copy now": maps the Aitum output onto `base` (key sealed). false + message on failure.
	bool copy_from_aitum(const Destination &base, const std::string &output_name, Destination &out, std::vector<std::string> &notes,
			     std::string &err) const;

	// Names of Aitum's stream outputs, for the dock drop-down. Empty + message when unreadable.
	std::vector<std::string> aitum_output_names(std::string &message) const;

private:
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
	void push_event(const Event &e);

	Config &cfg_;
	const SecretProtector &protector_;
	const Clock &clock_;
	OutputBackend &backend_;
	AitumTextProvider aitum_;
	ExternalTargets external_;
	LogFn log_;
	std::vector<std::unique_ptr<Runtime>> runtimes_;
	std::mutex ev_m_;
	std::vector<Event> events_;
	bool main_live_ = false;
	unsigned rng_state_;
};

} // namespace oms
