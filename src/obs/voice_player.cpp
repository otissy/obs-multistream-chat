#include "voice_player.hpp"

#include <obs-module.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

namespace oms {

std::vector<int16_t> voice_resample(const std::string &pcm16le, uint32_t rate, int volume)
{
	size_t n = pcm16le.size() / 2;
	std::vector<int16_t> out;
	if (n == 0 || rate < 8000 || rate > 192000)
		return out;
	double gain = std::clamp(volume, 0, 100) / 100.0;
	auto at = [&](size_t i) {
		return static_cast<int16_t>(static_cast<uint16_t>(static_cast<unsigned char>(pcm16le[2 * i])) |
					    (static_cast<uint16_t>(static_cast<unsigned char>(pcm16le[2 * i + 1])) << 8));
	};
	size_t m = static_cast<size_t>(std::llround(static_cast<double>(n) * kVoiceOutRate / rate));
	out.resize(m);
	double step = static_cast<double>(rate) / kVoiceOutRate;
	for (size_t j = 0; j < m; ++j) {
		double pos = j * step;
		size_t i = static_cast<size_t>(pos);
		double frac = pos - static_cast<double>(i);
		double a = at(std::min(i, n - 1));
		double b = at(std::min(i + 1, n - 1));
		double v = (a + (b - a) * frac) * gain;
		out[j] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
	}
	return out;
}

#ifdef _WIN32

namespace {

struct Chunk {
	WAVEHDR hdr;
	std::vector<int16_t> samples;
};

} // namespace

struct VoicePlayer::Impl {
	std::mutex mu;
	HWAVEOUT out = nullptr;
	bool open_failed = false;
	std::deque<Chunk *> queued;
	std::atomic<int> volume{85};

	bool open()
	{
		if (out)
			return true;
		if (open_failed)
			return false;
		WAVEFORMATEX fmt;
		ZeroMemory(&fmt, sizeof fmt);
		fmt.wFormatTag = WAVE_FORMAT_PCM;
		fmt.nChannels = 1;
		fmt.nSamplesPerSec = kVoiceOutRate;
		fmt.wBitsPerSample = 16;
		fmt.nBlockAlign = 2;
		fmt.nAvgBytesPerSec = kVoiceOutRate * 2;
		MMRESULT r = waveOutOpen(&out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
		if (r != MMSYSERR_NOERROR) {
			out = nullptr;
			open_failed = true; // logged once; a later stop() lets it try again
			blog(LOG_WARNING, "[obs-multistream-chat] voice: could not open the output device (waveOut %u)",
			     static_cast<unsigned>(r));
			return false;
		}
		return true;
	}

	// frees the chunks the device has finished with (in order: waveOut completes them in order)
	void reap()
	{
		while (!queued.empty() && (queued.front()->hdr.dwFlags & WHDR_DONE)) {
			Chunk *c = queued.front();
			queued.pop_front();
			waveOutUnprepareHeader(out, &c->hdr, sizeof c->hdr);
			delete c;
		}
	}
};

VoicePlayer::VoicePlayer() : impl_(new Impl) {}

VoicePlayer::~VoicePlayer()
{
	stop();
	std::lock_guard<std::mutex> lk(impl_->mu);
	if (impl_->out)
		waveOutClose(impl_->out);
}

void VoicePlayer::play(const std::string &pcm16le, uint32_t rate)
{
	std::vector<int16_t> s = voice_resample(pcm16le, rate, impl_->volume.load());
	if (s.empty())
		return;
	std::lock_guard<std::mutex> lk(impl_->mu);
	if (!impl_->open())
		return;
	impl_->reap();
	Chunk *c = new Chunk;
	c->samples = std::move(s);
	ZeroMemory(&c->hdr, sizeof c->hdr);
	c->hdr.lpData = reinterpret_cast<LPSTR>(c->samples.data());
	c->hdr.dwBufferLength = static_cast<DWORD>(c->samples.size() * sizeof(int16_t));
	if (waveOutPrepareHeader(impl_->out, &c->hdr, sizeof c->hdr) != MMSYSERR_NOERROR) {
		delete c;
		return;
	}
	if (waveOutWrite(impl_->out, &c->hdr, sizeof c->hdr) != MMSYSERR_NOERROR) {
		waveOutUnprepareHeader(impl_->out, &c->hdr, sizeof c->hdr);
		delete c;
		return;
	}
	impl_->queued.push_back(c);
}

void VoicePlayer::stop()
{
	std::lock_guard<std::mutex> lk(impl_->mu);
	impl_->open_failed = false;
	if (!impl_->out)
		return;
	waveOutReset(impl_->out); // marks every queued buffer done
	impl_->reap();
}

void VoicePlayer::set_volume(int volume)
{
	impl_->volume = std::clamp(volume, 0, 100);
}

#else // !_WIN32

struct VoicePlayer::Impl {};
VoicePlayer::VoicePlayer() : impl_(new Impl) {}
VoicePlayer::~VoicePlayer() = default;
void VoicePlayer::play(const std::string &, uint32_t) {}
void VoicePlayer::stop() {}
void VoicePlayer::set_volume(int) {}

#endif

} // namespace oms
