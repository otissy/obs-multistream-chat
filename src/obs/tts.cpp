#include "tts.hpp"

#include <obs-module.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace oms {

#ifdef _WIN32

namespace {

const wchar_t *const kVoiceCategories[] = {
	L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices",         // SAPI 5 desktop voices
	L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices", // per-language voices from Settings
};

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n > 0 ? n : 0, L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

std::string narrow(const wchar_t *w)
{
	if (!w || !*w)
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1)
		return std::string();
	std::string s(n - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
	return s;
}

void enum_voices(const wchar_t *category, std::vector<TtsVoice> &out)
{
	ISpObjectTokenCategory *cat = nullptr;
	if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory,
				    reinterpret_cast<void **>(&cat))))
		return;
	IEnumSpObjectTokens *tokens = nullptr;
	if (SUCCEEDED(cat->SetId(category, FALSE)) && SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &tokens))) {
		ISpObjectToken *tok = nullptr;
		while (tokens->Next(1, &tok, nullptr) == S_OK) {
			wchar_t *id = nullptr;
			wchar_t *name = nullptr;
			if (SUCCEEDED(tok->GetId(&id)) && SUCCEEDED(tok->GetStringValue(nullptr, &name))) {
				TtsVoice v;
				v.id = narrow(id);
				v.name = narrow(name);
				if (!v.id.empty() && !v.name.empty())
					out.push_back(v);
			}
			CoTaskMemFree(id);
			CoTaskMemFree(name);
			tok->Release();
		}
		tokens->Release();
	}
	cat->Release();
}

// "" selects the Windows default voice. A token that will not load falls back to the default, logged once per change.
void select_voice(ISpVoice *voice, const std::wstring &id)
{
	if (id.empty()) {
		voice->SetVoice(nullptr);
		return;
	}
	ISpObjectToken *tok = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken,
				      reinterpret_cast<void **>(&tok));
	if (SUCCEEDED(hr)) {
		hr = tok->SetId(nullptr, id.c_str(), FALSE);
		if (SUCCEEDED(hr))
			hr = voice->SetVoice(tok);
		tok->Release();
	}
	if (FAILED(hr)) {
		blog(LOG_WARNING, "[obs-multistream-chat] voice reader: the chosen voice did not load (0x%08lx), using the "
				  "Windows default voice",
		     static_cast<unsigned long>(hr));
		voice->SetVoice(nullptr);
	}
}

} // namespace

struct ChatSpeaker::Impl {
	struct Line {
		std::wstring text;
		std::wstring voice;
		int volume;
		int rate;
	};

	std::mutex mu;
	std::condition_variable cv;
	std::deque<Line> queue;
	std::atomic<bool> stopping{false};
	std::atomic<bool> cut{false}; // interrupt(): stop the line being spoken
	std::thread worker;

	void run()
	{
		// Every SAPI call happens on this thread, so the voice never crosses an apartment.
		HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		ISpVoice *voice = nullptr;
		HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
					      reinterpret_cast<void **>(&voice));
		if (FAILED(hr)) {
			blog(LOG_WARNING, "[obs-multistream-chat] voice reader: no SAPI voice could be created (0x%08lx)",
			     static_cast<unsigned long>(hr));
			voice = nullptr;
		}
		bool first = true;
		std::wstring current;
		for (;;) {
			Line line;
			{
				std::unique_lock<std::mutex> lk(mu);
				cv.wait(lk, [this] { return stopping.load() || !queue.empty(); });
				if (stopping.load())
					break;
				line = std::move(queue.front());
				queue.pop_front();
				cut = false;
			}
			if (!voice)
				continue;
			if (first || line.voice != current) {
				select_voice(voice, line.voice);
				current = line.voice;
				first = false;
			}
			voice->SetVolume(static_cast<USHORT>(std::clamp(line.volume, 0, 100)));
			voice->SetRate(std::clamp(line.rate, -10, 10));
			hr = voice->Speak(line.text.c_str(), SPF_ASYNC | SPF_IS_NOT_XML, nullptr);
			if (FAILED(hr)) {
				blog(LOG_WARNING, "[obs-multistream-chat] voice reader: Speak failed (0x%08lx)",
				     static_cast<unsigned long>(hr));
				continue;
			}
			// Wait for the end of the line, but stay responsive to interrupt() and shutdown.
			while (voice->WaitUntilDone(100) == S_FALSE) {
				if (cut.load() || stopping.load()) {
					voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
					break;
				}
			}
		}
		if (voice) {
			voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
			voice->Release();
		}
		if (SUCCEEDED(co))
			CoUninitialize();
	}
};

ChatSpeaker::ChatSpeaker() : impl_(new Impl)
{
	impl_->worker = std::thread([this] { impl_->run(); });
}

ChatSpeaker::~ChatSpeaker()
{
	{
		std::lock_guard<std::mutex> lk(impl_->mu);
		impl_->stopping = true;
		impl_->queue.clear();
	}
	impl_->cv.notify_all();
	if (impl_->worker.joinable())
		impl_->worker.join();
}

std::vector<TtsVoice> ChatSpeaker::voices()
{
	std::vector<TtsVoice> out;
	HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	for (const wchar_t *category : kVoiceCategories)
		enum_voices(category, out);
	if (SUCCEEDED(co))
		CoUninitialize();
	std::stable_sort(out.begin(), out.end(), [](const TtsVoice &a, const TtsVoice &b) { return a.name < b.name; });
	out.erase(std::unique(out.begin(), out.end(), [](const TtsVoice &a, const TtsVoice &b) { return a.name == b.name; }),
		  out.end());
	return out;
}

void ChatSpeaker::say(const std::string &utf8, const std::string &voice_id, int volume, int rate)
{
	Impl::Line line;
	line.text = widen(utf8);
	if (line.text.size() > kMaxChars)
		line.text.resize(kMaxChars);
	if (line.text.empty())
		return;
	line.voice = widen(voice_id);
	line.volume = volume;
	line.rate = rate;
	{
		std::lock_guard<std::mutex> lk(impl_->mu);
		while (impl_->queue.size() >= kMaxQueued)
			impl_->queue.pop_front();
		impl_->queue.push_back(std::move(line));
	}
	impl_->cv.notify_one();
}

void ChatSpeaker::interrupt()
{
	std::lock_guard<std::mutex> lk(impl_->mu);
	impl_->queue.clear();
	impl_->cut = true;
}

#else // !_WIN32

struct ChatSpeaker::Impl {};

ChatSpeaker::ChatSpeaker() : impl_(new Impl) {}
ChatSpeaker::~ChatSpeaker() = default;
std::vector<TtsVoice> ChatSpeaker::voices()
{
	return std::vector<TtsVoice>();
}
void ChatSpeaker::say(const std::string &, const std::string &, int, int) {}
void ChatSpeaker::interrupt() {}

#endif

} // namespace oms
