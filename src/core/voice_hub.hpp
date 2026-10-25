// Chat voice on stream: the spoken chat lines go to the chat overlay page (the browser source the
// streamer already has), which plays them, so OBS hears them as that source's audio.
//   plugin (voice helper PCM) --push--> VoiceHub --(SSE voice/events + GET voice/clip/<id>.wav)--> chat page
//   - bounded: at most max_clips clips and max_bytes of PCM; the oldest go first. A clip that is gone
//     when the page asks for it is a 404, and the page skips it.
//   - several overlay pages may listen (the same URL in two browser sources, a preview in a browser):
//     exactly one is the leader and speaks, see choose_leader.
//   - "also play locally": the leader page says whether the plugin should keep playing the line on
//     the Windows default device too. With no page listening at all, the plugin plays locally, as
//     before this feature (so the voice never goes silent just because no overlay is open).
// Portable, no OBS. Thread-safe (the voice helper's reader thread pushes, HTTP threads read).
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace oms {

struct VoiceClip {
	uint64_t id = 0;
	std::string author;   // who wrote the chat line (for the page's "skip these authors")
	std::string platform; // chat platform id
	uint32_t rate = 0;    // sample rate of pcm16le (mono, 16-bit little endian)
	size_t samples = 0;   // pcm16le.size() / 2 (kept when since() leaves the samples out)
	std::string pcm16le;
};

struct VoiceListener {
	uint64_t cid = 0;    // connection number, increasing: a higher one connected later
	bool obs = false;    // the page runs inside OBS (window.obsstudio exists)
	bool active = true;  // OBS says the source is shown on program (unknown counts as active)
	bool local = false;  // the page wants the plugin to also play the line locally
};

// The listener that speaks: shown on program beats hidden, inside OBS beats an ordinary browser,
// and among equals the newest connection wins (a reloaded page takes over at once). 0 when none.
uint64_t choose_leader(const std::vector<VoiceListener> &listeners);

// RIFF/WAVE (PCM, mono, 16-bit) around pcm16le. An odd trailing byte is dropped.
std::string make_wav(const std::string &pcm16le, uint32_t rate);

// One clip's announcement for the page (JSON object, single line): id, author, platform, rate, ms.
std::string voice_clip_json(const VoiceClip &c);

class VoiceHub {
public:
	struct Limits {
		size_t max_clips = 32;
		size_t max_bytes = 32u << 20;     // all clips together
		size_t max_clip_bytes = 8u << 20; // one clip (about 87 s at 48 kHz); larger ones are refused
	};
	VoiceHub();
	explicit VoiceHub(const Limits &l);

	// Adds a spoken line. Returns its id, or 0 when refused: no samples, rate outside 8000..192000,
	// larger than max_clip_bytes. Trims the oldest clips to stay inside the limits.
	uint64_t push(const std::string &author, const std::string &platform, uint32_t rate, const std::string &pcm16le);
	// The clip as a WAV file; false when it is not (or no longer) held.
	bool clip_wav(uint64_t id, std::string &wav) const;
	// Clips with id > after, oldest first: metadata only, pcm16le left empty (samples is set).
	std::vector<VoiceClip> since(uint64_t after) const;
	uint64_t last_id() const;
	size_t clip_count() const;
	size_t bytes() const;
	// Drops every held clip and tells the pages to stop the current line (the dock's interrupt).
	void flush();
	uint64_t flush_generation() const;

	uint64_t add_listener(bool obs, bool active, bool local); // returns the listener's cid
	void remove_listener(uint64_t cid);
	uint64_t leader() const;
	size_t listener_count() const;
	// Should the plugin also play lines on the local device? No listener: true. Else the leader's wish.
	bool local_wanted() const;

	// Changes whenever a clip arrives, a flush happens or the set of listeners changes.
	uint64_t version() const;
	// Waits up to ms for version() to differ from seen. true when it did.
	bool wait_for_change(uint64_t seen, int ms) const;
	void shutdown();
	bool is_shutdown() const;

	const Limits &limits() const { return limits_; }

private:
	void bump_locked();
	Limits limits_;
	mutable std::mutex m_;
	mutable std::condition_variable cv_;
	std::deque<VoiceClip> clips_;
	size_t bytes_ = 0;
	uint64_t next_id_ = 1;
	uint64_t next_cid_ = 1;
	uint64_t flush_gen_ = 0;
	uint64_t version_ = 1;
	bool shutdown_ = false;
	std::vector<VoiceListener> listeners_;
};

// Where one spoken piece goes. preview (the dock's test buttons) and no hub: local device only.
// Otherwise to the overlay page, plus the local device when local_wanted() (no page listening, or
// the speaking page asked for a local copy).
struct VoiceRoute {
	bool page = false;
	bool local = false;
};
VoiceRoute route_voice_clip(bool preview, const VoiceHub *hub);

// A browser source URL that is this install's chat overlay with the voice switched on:
// "http://127.0.0.1:<port>/k/<key>/chat.html?...voice=1..." (localhost too). Used to find sources
// whose "Control audio via OBS" is off, so the voice would go to the desktop device instead of a track.
bool is_speaking_chat_url(const std::string &url, const std::string &access_key);

} // namespace oms
