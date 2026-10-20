// Chat voice reader. One worker thread owns COM and a single SAPI voice and speaks queued lines in order.
//   - each line is spoken to the end (0.4.x released the voice right after an async Speak, so nothing was heard)
//   - chat text is never parsed as SAPI XML, so a message cannot change the volume or insert silences
//   - when chat outruns the voice the oldest waiting lines are dropped; long lines are cut
// Windows only; elsewhere voices() is empty and say() does nothing.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace oms {

struct TtsVoice {
	std::string id;   // SAPI token id, "" = the Windows default voice
	std::string name; // as Windows names it, e.g. "Microsoft Hazel - English (United Kingdom)"
};

class ChatSpeaker {
public:
	static constexpr size_t kMaxQueued = 5;
	static constexpr size_t kMaxChars = 300;

	ChatSpeaker();
	~ChatSpeaker(); // cuts off the current line and joins the worker
	ChatSpeaker(const ChatSpeaker &) = delete;
	ChatSpeaker &operator=(const ChatSpeaker &) = delete;

	// Installed voices: the SAPI 5 desktop voices plus the OneCore voices that Windows adds per language
	// (Settings > Time & language > Speech > Add voices). Sorted by name, duplicates removed.
	static std::vector<TtsVoice> voices();

	// volume 0..100, rate -10..10; voice_id from voices(), "" for the default voice.
	void say(const std::string &utf8, const std::string &voice_id, int volume, int rate);
	// Drops the waiting lines and cuts off the one being spoken.
	void interrupt();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace oms
