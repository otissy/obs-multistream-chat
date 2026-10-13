// What the overlay is allowed to know. This is the ONLY data that leaves the plugin towards the
// browser source: platform id, handle, health state. No URLs, no keys, no stats, no names.
#pragma once
#include "health.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace oms {

struct OverlayEntry {
	std::string id;
	std::string platform;
	std::string handle;
	State state = State::Idle;
};

// {"v":1,"seq":<n>,"main_live":bool,"destinations":[{"id","platform","handle","state","live"}]}
// "live" is true only for state live (a degraded output is still connected and sending, but the
// overlay shows it as a warning, so it is reported separately through "state").
std::string overlay_state_json(const std::vector<OverlayEntry> &entries, uint64_t seq, bool main_live);

// Latest snapshot, shared between the UI thread (publisher) and HTTP threads (readers).
class StateHub {
public:
	StateHub();
	// Replaces the snapshot. Identical payloads (ignoring seq) are not re-published.
	// Returns true if a new version was stored.
	bool publish(const std::vector<OverlayEntry> &entries, bool main_live);

	std::string snapshot(uint64_t *version = nullptr) const;
	// Blocks until the version differs from `seen`, the timeout passes, or shutdown(). Returns true
	// if there is a newer snapshot (stored in json/version).
	bool wait_for_change(uint64_t seen, int timeout_ms, std::string &json, uint64_t &version);
	void shutdown();
	bool is_shutdown() const;

private:
	mutable std::mutex m_;
	std::condition_variable cv_;
	uint64_t version_;
	std::string json_;
	std::string last_body_; // payload without seq, for change detection
	bool down_;
};

} // namespace oms
