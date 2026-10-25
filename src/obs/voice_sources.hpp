// Finds the browser sources that show this install's chat overlay with the voice on (voice=1) and
// checks their "Control audio via OBS" (browser source setting reroute_audio). Read-only: the
// plugin never changes a source the user made; it reports how many need the box ticked.
#pragma once
#include <string>

namespace oms {

struct VoiceSourceReport {
	int speaking = 0;      // chat overlay sources with voice=1
	int not_rerouted = 0;  // ... of which "Control audio via OBS" is off
};

VoiceSourceReport check_voice_sources(const std::string &access_key);
// "" when everything is fine, else one sentence for the Chat tab notice (never contains the URL or key).
std::string describe_voice_sources(const VoiceSourceReport &r);

} // namespace oms
