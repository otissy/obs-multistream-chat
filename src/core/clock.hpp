// Time source. Everything time-dependent in the core takes a Clock, so tests
// drive time by hand and the state machines never read the wall clock.
#pragma once
#include <chrono>
#include <cstdint>

namespace oms {

class Clock {
public:
	virtual ~Clock() = default;
	// Milliseconds on a monotonic scale. Only differences are meaningful.
	virtual int64_t now_ms() const = 0;
};

class SystemClock : public Clock {
public:
	int64_t now_ms() const override
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}
};

class MockClock : public Clock {
public:
	explicit MockClock(int64_t start_ms = 0) : t_(start_ms) {}
	int64_t now_ms() const override { return t_; }
	void advance(int64_t ms) { t_ += ms; }
	void set(int64_t ms) { t_ = ms; }

private:
	int64_t t_;
};

} // namespace oms
