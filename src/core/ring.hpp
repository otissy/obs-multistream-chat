// Fixed-capacity ring buffer: push overwrites the oldest element when full.
#pragma once
#include <cstddef>
#include <vector>

namespace oms {

template<typename T> class RingBuffer {
public:
	explicit RingBuffer(size_t capacity) : buf_(capacity ? capacity : 1), head_(0), size_(0) {}

	size_t capacity() const { return buf_.size(); }
	size_t size() const { return size_; }
	bool empty() const { return size_ == 0; }

	void push(const T &v)
	{
		if (size_ == buf_.size()) {
			buf_[head_] = v; // overwrite oldest
			head_ = (head_ + 1) % buf_.size();
		} else {
			buf_[(head_ + size_) % buf_.size()] = v;
			++size_;
		}
	}

	// i = 0 is the oldest element, i = size()-1 the newest. Caller checks i < size().
	const T &at(size_t i) const { return buf_[(head_ + i) % buf_.size()]; }
	const T &newest() const { return at(size_ - 1); }
	const T &oldest() const { return at(0); }

	void clear()
	{
		head_ = 0;
		size_ = 0;
	}

private:
	std::vector<T> buf_;
	size_t head_;
	size_t size_;
};

} // namespace oms
