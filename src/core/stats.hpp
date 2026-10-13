// Rolling output statistics built from periodic counter samples.
#pragma once
#include "ring.hpp"

#include <cstdint>

namespace oms {

// Cumulative counters as libobs reports them for one output (they restart at 0 per connection).
struct Sample {
	int64_t t_ms = 0;
	uint64_t bytes = 0;   // obs_output_get_total_bytes
	int64_t frames = 0;   // obs_output_get_total_frames
	int64_t dropped = 0;  // obs_output_get_frames_dropped
};

class StatsTracker {
public:
	explicit StatsTracker(size_t capacity = 180) : ring_(capacity) {}

	void reset() { ring_.clear(); }
	// A counter that goes backwards means a new connection: history is discarded first.
	void add(const Sample &s);

	size_t count() const { return ring_.size(); }
	bool has_latest() const { return !ring_.empty(); }
	const Sample &latest() const { return ring_.newest(); }

	// Average over the newest samples spanning at most window_ms. 0 when fewer than 2 samples
	// or no time elapsed.
	double bitrate_kbps(int64_t window_ms) const;
	// dropped frames / frames sent, in percent, over the same window; 0 without frame progress.
	double dropped_pct(int64_t window_ms) const;
	// Total dropped / total frames since the connection began.
	double dropped_pct_total() const;

private:
	// Index of the oldest sample inside the window ending at the newest sample.
	size_t window_start(int64_t window_ms) const;
	RingBuffer<Sample> ring_;
};

} // namespace oms
