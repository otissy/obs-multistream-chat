#include "voice_sources.hpp"

#include "voice_hub.hpp"

#include <obs-module.h>

#include <cstring>

namespace oms {

namespace {

struct Ctx {
	std::string key;
	VoiceSourceReport r;
};

bool visit(void *p, obs_source_t *s)
{
	Ctx *c = static_cast<Ctx *>(p);
	const char *id = obs_source_get_unversioned_id(s);
	if (!id || std::strcmp(id, "browser_source") != 0)
		return true;
	obs_data_t *d = obs_source_get_settings(s);
	if (!d)
		return true;
	const char *url = obs_data_get_string(d, "url");
	bool speaking = url && is_speaking_chat_url(url, c->key);
	bool rerouted = obs_data_get_bool(d, "reroute_audio");
	obs_data_release(d);
	if (speaking) {
		++c->r.speaking;
		if (!rerouted)
			++c->r.not_rerouted;
	}
	return true;
}

} // namespace

VoiceSourceReport check_voice_sources(const std::string &access_key)
{
	Ctx c;
	c.key = access_key;
	if (!access_key.empty())
		obs_enum_sources(visit, &c);
	return c.r;
}

std::string describe_voice_sources(const VoiceSourceReport &r)
{
	if (r.not_rerouted == 0)
		return std::string();
	return std::to_string(r.not_rerouted) + " chat overlay source(s) read chat aloud but \"Control audio via OBS\" is off, "
	       "so the voice plays on the desktop speakers instead of the stream: open the browser source's properties and tick it.";
}

} // namespace oms
