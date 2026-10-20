// The voice helper (voice/index.js) as the plugin sees it: decode the frames it writes on stdout,
// write the request lines it reads on stdin, and list the catalogue voices whose packs are installed.
// The frame layout and limits match voice/lib/frames.js; the catalogue rules match voice/lib/catalogue.js.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace oms {
namespace voice {

constexpr size_t kHeader = 20;
constexpr uint32_t kMaxPayload = 16u * 1024u * 1024u;

enum class FrameType : uint8_t { Audio = 1, Done = 2, Error = 3, Ready = 4 };

struct Frame {
	FrameType type = FrameType::Done;
	uint32_t id = 0;
	uint32_t sample_rate = 0;
	std::string payload; // Audio: 16-bit mono PCM, little endian; Error / Ready: UTF-8
};

class FrameDecoder {
public:
	// Appends bytes and moves every complete frame to out. Returns false on a bad magic or an
	// oversized payload; after that the stream cannot be trusted and every call returns false.
	bool push(const char *data, size_t n, std::vector<Frame> &out);
	bool failed() const { return failed_; }

private:
	std::string buf_;
	bool failed_ = false;
};

struct Pack {
	std::string id;
	std::string engine;                 // "kokoro" or "vits"
	std::vector<std::string> required;  // files and folders inside the pack folder that must exist
	std::string usage;                  // "" or a label such as "non-commercial" / "licence unclear"
};

struct Voice {
	std::string id;
	std::string name;
	std::string locale;   // en-GB
	std::string language; // English
	std::string accent;   // Scottish (may be empty)
	std::string gender;   // female / male / ""
	std::string pack;
	std::string usage; // the pack's usage label, shown with the name
	int sid = 0;
};

struct Catalogue {
	std::vector<Pack> packs;
	std::vector<Voice> voices;
};

// Strict: a bad id, an unknown pack, a duplicate or a path leaving its pack folder fails the whole file.
bool parse_catalogue(const std::string &text, Catalogue &out, std::string &err);

// "English (Scottish): Alba, female", plus " [non-commercial]" when the pack carries a usage label
std::string display_name(const Voice &v);

// The voices whose pack `installed` accepts, sorted by language, accent, then name.
std::vector<Voice> installed_voices(const Catalogue &c, const std::function<bool(const Pack &)> &installed);

// One request line each, newline included.
std::string say_request(uint32_t id, const std::string &voice_id, const std::string &text, double speed);
std::string flush_request();

} // namespace voice
} // namespace oms
