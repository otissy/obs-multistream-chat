// Chat voice reader: neural voices (Kokoro and Piper through sherpa-onnx) that run on this PC.
// The models run in the voice helper (voice\index.js, see VoiceProcess), never inside OBS; this class
// starts it on the first line, sends it the lines, and plays what comes back (VoicePlayer).
//   - lines are spoken in order; when chat outruns the voice the helper drops the oldest waiting lines
//   - chat text is cleaned by the helper (links, emoji, spam runs, length) before it is spoken
//   - only voices whose pack the installer put in data\voices are offered
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace oms {

struct TtsVoice {
	std::string id;   // catalogue id, e.g. "kokoro.bf_emma"
	std::string name; // "English (British): Emma, female"
};

class VoiceHub;

// Who said a line, so the chat overlay page can skip authors; preview = the dock's test buttons,
// which are heard locally only and never go to the overlay (route_voice_clip in voice_hub.hpp).
struct SayMeta {
	std::string author;
	std::string platform;
	bool preview = false;
};

class ChatSpeaker {
public:
	// node_path: as for the chat reader ("" searches PATH); log_file: where the helper's stderr goes.
	// hub: where spoken lines go for the chat overlay page (nullptr = local device only, as before).
	ChatSpeaker(const std::string &node_path, const std::string &log_file, VoiceHub *hub = nullptr);
	~ChatSpeaker(); // cuts off the current line and stops the helper
	ChatSpeaker(const ChatSpeaker &) = delete;
	ChatSpeaker &operator=(const ChatSpeaker &) = delete;

	// The installed voices, sorted by language and accent. Empty when no voice pack is installed.
	static std::vector<TtsVoice> voices();

	// volume 0..100, rate -5..5 (the dock's sliders); voice_id from voices().
	// volume applies to the local copy only; the overlay page has its own volume.
	void say(const std::string &utf8, const std::string &voice_id, int volume, int rate, const SayMeta &meta = SayMeta());
	// Drops the waiting lines and cuts off the one being spoken.
	void interrupt();
	// Why the last line could not be spoken ("" when the helper is running).
	std::string problem() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace oms
