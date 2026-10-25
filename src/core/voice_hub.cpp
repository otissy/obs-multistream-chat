#include "voice_hub.hpp"

#include "json.hpp"

#include <chrono>

namespace oms {

uint64_t choose_leader(const std::vector<VoiceListener> &listeners)
{
	const VoiceListener *best = nullptr;
	for (const VoiceListener &l : listeners) {
		if (!best) {
			best = &l;
			continue;
		}
		if (l.active != best->active) {
			if (l.active)
				best = &l;
			continue;
		}
		if (l.obs != best->obs) {
			if (l.obs)
				best = &l;
			continue;
		}
		if (l.cid > best->cid)
			best = &l;
	}
	return best ? best->cid : 0;
}

namespace {

void put32(std::string &s, uint32_t v)
{
	for (int i = 0; i < 4; ++i)
		s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void put16(std::string &s, uint16_t v)
{
	s.push_back(static_cast<char>(v & 0xff));
	s.push_back(static_cast<char>((v >> 8) & 0xff));
}

} // namespace

std::string make_wav(const std::string &pcm16le, uint32_t rate)
{
	uint32_t data = static_cast<uint32_t>(pcm16le.size() & ~static_cast<size_t>(1));
	std::string w;
	w.reserve(44 + data);
	w += "RIFF";
	put32(w, 36 + data);
	w += "WAVEfmt ";
	put32(w, 16);
	put16(w, 1); // PCM
	put16(w, 1); // mono
	put32(w, rate);
	put32(w, rate * 2);
	put16(w, 2);
	put16(w, 16);
	w += "data";
	put32(w, data);
	w.append(pcm16le, 0, data);
	return w;
}

std::string voice_clip_json(const VoiceClip &c)
{
	json::Value o = json::Value::object();
	o.set("id", json::Value::number(static_cast<double>(c.id)));
	o.set("author", json::Value::string(c.author));
	o.set("platform", json::Value::string(c.platform));
	o.set("rate", json::Value::number(c.rate));
	double ms = c.rate ? c.samples * 1000.0 / c.rate : 0;
	o.set("ms", json::Value::number(static_cast<double>(static_cast<long long>(ms))));
	return json::dump(o, false);
}

VoiceHub::VoiceHub() {}
VoiceHub::VoiceHub(const Limits &l) : limits_(l) {}

void VoiceHub::bump_locked()
{
	++version_;
	cv_.notify_all();
}

uint64_t VoiceHub::push(const std::string &author, const std::string &platform, uint32_t rate, const std::string &pcm16le)
{
	if (pcm16le.size() < 2 || rate < 8000 || rate > 192000 || pcm16le.size() > limits_.max_clip_bytes)
		return 0;
	std::lock_guard<std::mutex> lk(m_);
	if (shutdown_)
		return 0;
	VoiceClip c;
	c.id = next_id_++;
	c.author = author.substr(0, 200);
	c.platform = platform.substr(0, 32);
	c.rate = rate;
	c.pcm16le = pcm16le.substr(0, pcm16le.size() & ~static_cast<size_t>(1));
	c.samples = c.pcm16le.size() / 2;
	bytes_ += c.pcm16le.size();
	clips_.push_back(std::move(c));
	while (!clips_.empty() && (clips_.size() > limits_.max_clips || bytes_ > limits_.max_bytes)) {
		bytes_ -= clips_.front().pcm16le.size();
		clips_.pop_front();
	}
	uint64_t id = next_id_ - 1;
	bump_locked();
	return id;
}

bool VoiceHub::clip_wav(uint64_t id, std::string &wav) const
{
	std::lock_guard<std::mutex> lk(m_);
	for (const VoiceClip &c : clips_)
		if (c.id == id) {
			wav = make_wav(c.pcm16le, c.rate);
			return true;
		}
	return false;
}

std::vector<VoiceClip> VoiceHub::since(uint64_t after) const
{
	std::lock_guard<std::mutex> lk(m_);
	std::vector<VoiceClip> out;
	for (const VoiceClip &c : clips_)
		if (c.id > after) {
			VoiceClip m;
			m.id = c.id;
			m.author = c.author;
			m.platform = c.platform;
			m.rate = c.rate;
			m.samples = c.samples;
			out.push_back(std::move(m));
		}
	return out;
}

uint64_t VoiceHub::last_id() const
{
	std::lock_guard<std::mutex> lk(m_);
	return next_id_ - 1;
}

size_t VoiceHub::clip_count() const
{
	std::lock_guard<std::mutex> lk(m_);
	return clips_.size();
}

size_t VoiceHub::bytes() const
{
	std::lock_guard<std::mutex> lk(m_);
	return bytes_;
}

void VoiceHub::flush()
{
	std::lock_guard<std::mutex> lk(m_);
	clips_.clear();
	bytes_ = 0;
	++flush_gen_;
	bump_locked();
}

uint64_t VoiceHub::flush_generation() const
{
	std::lock_guard<std::mutex> lk(m_);
	return flush_gen_;
}

uint64_t VoiceHub::add_listener(bool obs, bool active, bool local)
{
	std::lock_guard<std::mutex> lk(m_);
	VoiceListener l;
	l.cid = next_cid_++;
	l.obs = obs;
	l.active = active;
	l.local = local;
	listeners_.push_back(l);
	bump_locked();
	return l.cid;
}

void VoiceHub::remove_listener(uint64_t cid)
{
	std::lock_guard<std::mutex> lk(m_);
	for (size_t i = 0; i < listeners_.size(); ++i)
		if (listeners_[i].cid == cid) {
			listeners_.erase(listeners_.begin() + static_cast<std::ptrdiff_t>(i));
			bump_locked();
			return;
		}
}

uint64_t VoiceHub::leader() const
{
	std::lock_guard<std::mutex> lk(m_);
	return choose_leader(listeners_);
}

size_t VoiceHub::listener_count() const
{
	std::lock_guard<std::mutex> lk(m_);
	return listeners_.size();
}

bool VoiceHub::local_wanted() const
{
	std::lock_guard<std::mutex> lk(m_);
	uint64_t id = choose_leader(listeners_);
	if (id == 0)
		return true;
	for (const VoiceListener &l : listeners_)
		if (l.cid == id)
			return l.local;
	return true;
}

uint64_t VoiceHub::version() const
{
	std::lock_guard<std::mutex> lk(m_);
	return version_;
}

bool VoiceHub::wait_for_change(uint64_t seen, int ms) const
{
	std::unique_lock<std::mutex> lk(m_);
	return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return version_ != seen || shutdown_; }) && version_ != seen;
}

void VoiceHub::shutdown()
{
	std::lock_guard<std::mutex> lk(m_);
	shutdown_ = true;
	cv_.notify_all();
}

VoiceRoute route_voice_clip(bool preview, const VoiceHub *hub)
{
	VoiceRoute r;
	if (preview || !hub) {
		r.local = true;
		return r;
	}
	r.page = true;
	r.local = hub->local_wanted();
	return r;
}

bool is_speaking_chat_url(const std::string &url, const std::string &access_key)
{
	if (access_key.empty())
		return false;
	std::string page = "/k/" + access_key + "/chat.html";
	bool host_ok = url.compare(0, 17, "http://127.0.0.1:") == 0 || url.compare(0, 17, "http://localhost:") == 0;
	size_t at = url.find(page);
	if (!host_ok || at == std::string::npos || url.find('/', 17) != at)
		return false;
	size_t q = at + page.size();
	if (q >= url.size() || url[q] != '?')
		return false;
	std::string query = url.substr(q + 1);
	size_t hash = query.find('#');
	if (hash != std::string::npos)
		query.resize(hash);
	size_t pos = 0;
	while (pos <= query.size()) {
		size_t end = query.find('&', pos);
		if (query.substr(pos, end == std::string::npos ? std::string::npos : end - pos) == "voice=1")
			return true;
		if (end == std::string::npos)
			break;
		pos = end + 1;
	}
	return false;
}

bool VoiceHub::is_shutdown() const
{
	std::lock_guard<std::mutex> lk(m_);
	return shutdown_;
}

} // namespace oms
