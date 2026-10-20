// Plays the voice helper's audio on the default output device (waveOut), where the old SAPI reader
// played too. Everything is resampled to one 48 kHz mono stream, so lines from voices with different
// sample rates (Kokoro 24 kHz, Piper 16 / 22.05 kHz) queue back to back without reopening the device.
// Thread-safe; Windows only (elsewhere it discards the audio).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oms {

constexpr uint32_t kVoiceOutRate = 48000;

// Linear-interpolation resample of 16-bit mono PCM (little endian bytes) to kVoiceOutRate, scaled by
// volume (0..100). Pure; exposed for tests.
std::vector<int16_t> voice_resample(const std::string &pcm16le, uint32_t rate, int volume);

class VoicePlayer {
public:
	VoicePlayer();
	~VoicePlayer();
	VoicePlayer(const VoicePlayer &) = delete;
	VoicePlayer &operator=(const VoicePlayer &) = delete;

	void play(const std::string &pcm16le, uint32_t rate); // queues; opens the device on first use
	void stop();                                         // cuts everything queued or playing
	void set_volume(int volume);                         // 0..100, for chunks queued from now on

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace oms
