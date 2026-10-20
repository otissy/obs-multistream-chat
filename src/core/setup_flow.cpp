#include "setup_flow.hpp"

#include "chat_targets.hpp"

namespace oms {

SetupProgress setup_progress(const Config &cfg, bool overlay_copied)
{
	SetupProgress p;
	p.destination = !cfg.destinations.empty();
	p.chat = !cfg.chat.sources.empty();
	p.overlay = overlay_copied;
	return p;
}

std::string next_chat_source_id(const std::vector<ChatSource> &sources)
{
	for (unsigned n = 1;; ++n) {
		std::string id = "c" + std::to_string(n);
		bool used = false;
		for (size_t i = 0; i < sources.size() && !used; ++i)
			used = sources[i].id == id;
		if (!used)
			return id;
	}
}

AddResult add_chat_source(std::vector<ChatSource> &sources, const std::string &platform, const std::string &input,
			  std::string &target_out, std::string &err)
{
	std::string raw = input;
	size_t a = raw.find_first_not_of(" \t\r\n");
	size_t b = raw.find_last_not_of(" \t\r\n");
	raw = a == std::string::npos ? std::string() : raw.substr(a, b - a + 1);
	if (!normalize_chat_target(platform, raw, target_out, err))
		return AddResult::Invalid;
	for (size_t i = 0; i < sources.size(); ++i) {
		if (sources[i].platform != platform || sources[i].target != target_out)
			continue;
		if (sources[i].enabled)
			return AddResult::Duplicate;
		sources[i].enabled = true;
		return AddResult::Enabled;
	}
	if (sources.size() >= static_cast<size_t>(kMaxChatSources)) {
		err = "at most 32 chat sources";
		return AddResult::Full;
	}
	ChatSource s;
	s.id = next_chat_source_id(sources);
	s.platform = platform;
	s.target = target_out;
	s.enabled = true;
	sources.push_back(s);
	return AddResult::Added;
}

int add_sources_from_destinations(const std::vector<Destination> &dests, std::vector<ChatSource> &sources)
{
	int added = 0;
	for (size_t i = 0; i < dests.size(); ++i) {
		const Destination &d = dests[i];
		// YouTube needs the id of each live video, X a broadcast link: neither follows from a stream key
		if (d.platform != "twitch" && d.platform != "kick" && d.platform != "tiktok")
			continue;
		std::string raw = !d.handle.empty() ? d.handle : d.name;
		std::string target, err;
		bool exists = false;
		if (raw.empty() || !normalize_chat_target(d.platform, raw, target, err))
			continue;
		for (size_t j = 0; j < sources.size() && !exists; ++j)
			exists = sources[j].platform == d.platform && sources[j].target == target;
		if (exists || sources.size() >= static_cast<size_t>(kMaxChatSources))
			continue;
		if (add_chat_source(sources, d.platform, target, target, err) == AddResult::Added)
			++added;
	}
	return added;
}

int reader_start_mode(const ChatConfig &c)
{
	if (c.start_with_obs)
		return 1;
	return c.auto_start_with_stream ? 2 : 0;
}

void set_reader_start_mode(ChatConfig &c, int mode)
{
	c.start_with_obs = mode == 1;
	c.auto_start_with_stream = mode == 2;
}

} // namespace oms
