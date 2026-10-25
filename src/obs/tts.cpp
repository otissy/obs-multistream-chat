#include "tts.hpp"

#include "voice.hpp"
#include "voice_hub.hpp"
#include "voice_player.hpp"
#include "voice_process.hpp"

#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace oms {

namespace {

std::string module_path(const char *rel)
{
	char *p = obs_module_file(rel);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

// data\voices next to data\voice: obs_module_file() only answers for files that exist
std::string packs_dir()
{
	const char *d = obs_get_module_data_path(obs_current_module());
	return d ? std::string(d) + "/voices" : std::string();
}

bool exists_utf8(const std::string &p)
{
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::u8path(p), ec);
}

bool load_catalogue(voice::Catalogue &cat)
{
	std::string file = module_path("voice/catalogue.json");
	if (file.empty())
		return false;
	std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	std::string err;
	if (!voice::parse_catalogue(ss.str(), cat, err)) {
		blog(LOG_WARNING, "[obs-multistream-chat] voice: %s", err.c_str());
		return false;
	}
	return true;
}

long long now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct ChatSpeaker::Impl {
	std::string node_path;
	std::string log_file;
	VoicePlayer player;
	VoiceProcess proc;
	VoiceHub *hub = nullptr;
	uint32_t next_id = 1;
	long long last_start_try = -100000;
	mutable std::mutex mu; // problem, metas
	std::string problem;
	std::map<uint32_t, SayMeta> metas; // request id -> who said it, until the helper's Done frame

	// One spoken piece of a line: to the overlay page (VoiceHub) and/or the local device.
	void on_audio(const voice::Frame &f)
	{
		SayMeta m;
		{
			std::lock_guard<std::mutex> lk(mu);
			auto it = metas.find(f.id);
			if (it != metas.end())
				m = it->second;
		}
		VoiceRoute r = route_voice_clip(m.preview, hub);
		if (r.page)
			hub->push(m.author, m.platform, f.sample_rate, f.payload);
		if (r.local)
			player.play(f.payload, f.sample_rate);
	}

	void forget(uint32_t id)
	{
		std::lock_guard<std::mutex> lk(mu);
		metas.erase(id);
	}

	void set_problem(const std::string &p)
	{
		std::lock_guard<std::mutex> lk(mu);
		problem = p;
	}

	bool ensure_running()
	{
		if (proc.running())
			return true;
		// a helper that will not start is retried at most every 10 s, not on every chat line
		if (now_ms() - last_start_try < 10000)
			return false;
		last_start_try = now_ms();
		std::string js = module_path("voice/index.js");
		std::string packs = packs_dir();
		if (js.empty() || packs.empty()) {
			set_problem("The voice helper or the voice packs are missing from the plugin folder. Re-run the installer.");
			return false;
		}
		std::string err;
		bool ok = proc.start(node_path, js, packs, log_file,
				     [this](const voice::Frame &f) {
					     switch (f.type) {
					     case voice::FrameType::Audio: on_audio(f); break;
					     case voice::FrameType::Done: forget(f.id); break;
					     case voice::FrameType::Error:
						     blog(LOG_WARNING, "[obs-multistream-chat] voice: line %u: %s",
							  static_cast<unsigned>(f.id), f.payload.substr(0, 300).c_str());
						     break;
					     case voice::FrameType::Ready:
						     blog(LOG_INFO, "[obs-multistream-chat] voice helper ready %s",
							  f.payload.substr(0, 100).c_str());
						     break;
					     default: break;
					     }
				     },
				     err);
		if (!ok) {
			blog(LOG_WARNING, "[obs-multistream-chat] voice helper did not start: %s", err.c_str());
			set_problem(err);
			return false;
		}
		set_problem("");
		return true;
	}
};

ChatSpeaker::ChatSpeaker(const std::string &node_path, const std::string &log_file, VoiceHub *hub) : impl_(new Impl)
{
	impl_->node_path = node_path;
	impl_->log_file = log_file;
	impl_->hub = hub;
}

ChatSpeaker::~ChatSpeaker()
{
	impl_->player.stop();
	impl_->proc.stop_blocking(1500);
}

std::vector<TtsVoice> ChatSpeaker::voices()
{
	std::vector<TtsVoice> out;
	voice::Catalogue cat;
	std::string packs = packs_dir();
	if (packs.empty() || !exists_utf8(packs) || !load_catalogue(cat))
		return out;
	auto installed = [&](const voice::Pack &p) {
		for (const std::string &f : p.required)
			if (!exists_utf8(packs + "/" + p.id + "/" + f))
				return false;
		return true;
	};
	for (const voice::Voice &v : voice::installed_voices(cat, installed)) {
		TtsVoice t;
		t.id = v.id;
		t.name = voice::display_name(v);
		out.push_back(t);
	}
	return out;
}

void ChatSpeaker::say(const std::string &utf8, const std::string &voice_id, int volume, int rate, const SayMeta &meta)
{
	if (utf8.empty() || voice_id.empty())
		return;
	if (!impl_->ensure_running())
		return;
	impl_->player.set_volume(volume);
	double speed = 1.0 + 0.1 * std::clamp(rate, -5, 5);
	uint32_t id = impl_->next_id++;
	{
		std::lock_guard<std::mutex> lk(impl_->mu);
		if (impl_->metas.size() > 256)
			impl_->metas.clear(); // a helper that never answered: never grow without bound
		impl_->metas[id] = meta;
	}
	if (!impl_->proc.send(voice::say_request(id, voice_id, utf8, speed)))
		impl_->set_problem("The voice helper stopped; it restarts on the next line.");
}

void ChatSpeaker::interrupt()
{
	impl_->player.stop();
	if (impl_->hub)
		impl_->hub->flush(); // the overlay page stops the current line too
	if (impl_->proc.running())
		impl_->proc.send(voice::flush_request());
}

std::string ChatSpeaker::problem() const
{
	std::lock_guard<std::mutex> lk(impl_->mu);
	return impl_->problem;
}

} // namespace oms
