#include "state_feed.hpp"
#include "json.hpp"

#include <chrono>

namespace oms {

namespace {

std::string body_json(const std::vector<OverlayEntry> &entries, bool main_live)
{
	json::Value arr = json::Value::array();
	for (size_t i = 0; i < entries.size(); ++i) {
		const OverlayEntry &e = entries[i];
		json::Value o = json::Value::object();
		o.set("id", json::Value::string(e.id));
		o.set("platform", json::Value::string(e.platform));
		o.set("handle", json::Value::string(e.handle));
		o.set("state", json::Value::string(state_name(e.state)));
		o.set("live", json::Value::boolean(e.state == State::Live));
		arr.push(o);
	}
	json::Value root = json::Value::object();
	root.set("main_live", json::Value::boolean(main_live));
	root.set("destinations", arr);
	return json::dump(root);
}

} // namespace

std::string overlay_state_json(const std::vector<OverlayEntry> &entries, uint64_t seq, bool main_live)
{
	json::Value arr = json::Value::array();
	for (size_t i = 0; i < entries.size(); ++i) {
		const OverlayEntry &e = entries[i];
		json::Value o = json::Value::object();
		o.set("id", json::Value::string(e.id));
		o.set("platform", json::Value::string(e.platform));
		o.set("handle", json::Value::string(e.handle));
		o.set("state", json::Value::string(state_name(e.state)));
		o.set("live", json::Value::boolean(e.state == State::Live));
		arr.push(o);
	}
	json::Value root = json::Value::object();
	root.set("v", json::Value::number(1));
	root.set("seq", json::Value::number(static_cast<double>(seq)));
	root.set("main_live", json::Value::boolean(main_live));
	root.set("destinations", arr);
	return json::dump(root);
}

StateHub::StateHub() : version_(0), down_(false)
{
	json_ = overlay_state_json(std::vector<OverlayEntry>(), 0, false);
}

bool StateHub::publish(const std::vector<OverlayEntry> &entries, bool main_live)
{
	std::string body = body_json(entries, main_live);
	std::lock_guard<std::mutex> lk(m_);
	if (version_ > 0 && body == last_body_)
		return false;
	++version_;
	last_body_ = body;
	json_ = overlay_state_json(entries, version_, main_live);
	cv_.notify_all();
	return true;
}

std::string StateHub::snapshot(uint64_t *version) const
{
	std::lock_guard<std::mutex> lk(m_);
	if (version)
		*version = version_;
	return json_;
}

bool StateHub::wait_for_change(uint64_t seen, int timeout_ms, std::string &json, uint64_t &version)
{
	std::unique_lock<std::mutex> lk(m_);
	cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return down_ || version_ != seen; });
	if (down_ || version_ == seen)
		return false;
	json = json_;
	version = version_;
	return true;
}

void StateHub::shutdown()
{
	std::lock_guard<std::mutex> lk(m_);
	down_ = true;
	cv_.notify_all();
}

bool StateHub::is_shutdown() const
{
	std::lock_guard<std::mutex> lk(m_);
	return down_;
}

} // namespace oms
