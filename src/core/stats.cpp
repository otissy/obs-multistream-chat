#include "stats.hpp"

namespace oms {

void StatsTracker::add(const Sample &s)
{
	if (!ring_.empty()) {
		const Sample &last = ring_.newest();
		if (s.bytes < last.bytes || s.frames < last.frames || s.dropped < last.dropped || s.t_ms < last.t_ms)
			ring_.clear();
	}
	ring_.push(s);
}

size_t StatsTracker::window_start(int64_t window_ms) const
{
	size_t n = ring_.size();
	int64_t newest_t = ring_.newest().t_ms;
	size_t start = n - 1;
	while (start > 0 && newest_t - ring_.at(start - 1).t_ms <= window_ms)
		--start;
	return start;
}

double StatsTracker::bitrate_kbps(int64_t window_ms) const
{
	if (ring_.size() < 2)
		return 0.0;
	size_t a = window_start(window_ms);
	const Sample &o = ring_.at(a);
	const Sample &n = ring_.newest();
	int64_t dt = n.t_ms - o.t_ms;
	if (dt <= 0)
		return 0.0;
	// bytes * 8 bits / dt ms = kilobits per second
	return double(n.bytes - o.bytes) * 8.0 / double(dt);
}

double StatsTracker::dropped_pct(int64_t window_ms) const
{
	if (ring_.size() < 2)
		return 0.0;
	size_t a = window_start(window_ms);
	const Sample &o = ring_.at(a);
	const Sample &n = ring_.newest();
	int64_t df = n.frames - o.frames;
	int64_t dd = n.dropped - o.dropped;
	if (df <= 0)
		return 0.0;
	double p = double(dd) * 100.0 / double(df);
	return p > 100.0 ? 100.0 : p;
}

double StatsTracker::dropped_pct_total() const
{
	if (ring_.empty())
		return 0.0;
	const Sample &n = ring_.newest();
	if (n.frames <= 0)
		return 0.0;
	double p = double(n.dropped) * 100.0 / double(n.frames);
	return p > 100.0 ? 100.0 : p;
}

} // namespace oms
