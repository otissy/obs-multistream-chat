// Test fixture: the REAL HttpServer + ChatHub serving the REAL data/overlay chat assets, so node
// and headless Chrome can drive the reader and the overlay end to end.
//   usage: oms_chat_fixture <overlay-dir>      (token from env OMSC_CHAT_TOKEN, like the plugin;
//          overlay access key from env OMSC_ACCESS_KEY, none when unset)
// prints "PORT <n>", then reads stdin lines:
//   filter words=a,b users=x,y commands=0|1 bots=0|1
//   status          -> prints the /chat/status.json body
//   dest <platform> <handle> <state>   adds a destination to the overlay state (prints OK)
//   dests-clear     empties the overlay state (prints OK)
//   tone <author> <platform> <rate> <ms> <hz> [amp]  -> pushes a sine clip to the VoiceHub, prints "CLIP <id>"
//   wav <author> <platform> <path>                  -> pushes a 16-bit mono WAV file (a real voice line), "CLIP <id>"
//   vflush          -> VoiceHub::flush (the dock's interrupt), prints "OK"
//   vstatus         -> {"listeners":n,"leader":cid,"local":bool,"clips":n}
//   quit
#include "../../../src/core/chat_hub.hpp"
#include "../../../src/core/http_server.hpp"
#include "../../../src/core/voice_hub.hpp"

#include <cmath>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace oms;

static bool slurp(const std::string &p, std::string &o)
{
	std::ifstream f(p, std::ios::binary);
	if (!f)
		return false;
	std::ostringstream s;
	s << f.rdbuf();
	o = s.str();
	return true;
}

static std::vector<std::string> csv(const std::string &s)
{
	std::vector<std::string> out;
	std::stringstream ss(s);
	std::string item;
	while (std::getline(ss, item, ','))
		if (!item.empty())
			out.push_back(item);
	return out;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	std::string dir = argv[1];
	AssetMap assets;
	struct F {
		const char *url;
		const char *file;
		const char *type;
	};
	F files[] = {{"/chat.html", "chat.html", "text/html; charset=utf-8"},
		     {"/chat.js", "chat.js", "text/javascript; charset=utf-8"},
		     {"/chat-voice.js", "chat-voice.js", "text/javascript; charset=utf-8"},
		     {"/icons/twitch.svg", "icons/twitch.svg", "image/svg+xml"},
		     {"/icons/youtube.svg", "icons/youtube.svg", "image/svg+xml"},
		     {"/icons/kick.svg", "icons/kick.svg", "image/svg+xml"},
		     {"/icons/tiktok.svg", "icons/tiktok.svg", "image/svg+xml"},
		     {"/icons/x.svg", "icons/x.svg", "image/svg+xml"},
		     {"/icons/generic.svg", "icons/generic.svg", "image/svg+xml"},
		     {"/overlay.html", "overlay.html", "text/html; charset=utf-8"},
		     {"/overlay.js", "overlay.js", "text/javascript; charset=utf-8"}};
	for (const F &f : files) {
		Asset a;
		a.content_type = f.type;
		if (!slurp(dir + "/" + f.file, a.body)) {
			std::cerr << "missing " << f.file << "\n";
			return 3;
		}
		assets[f.url] = a;
	}
	const char *tok = std::getenv("OMSC_CHAT_TOKEN");
	SystemClock clock;
	ChatHub hub(clock);
	hub.set_token(tok ? tok : "");
	StateHub state;
	ServerOptions so;
	const char *key = std::getenv("OMSC_ACCESS_KEY");
	so.access_key = key ? key : "";
	HttpServer srv(state, assets, so, &hub);
	VoiceHub voice;
	srv.set_voice_hub(&voice);
	std::string err;
	if (!srv.start(err)) {
		std::cerr << err << "\n";
		return 4;
	}
	std::cout << "PORT " << srv.port() << std::endl;
	std::string line;
	std::vector<OverlayEntry> dests;
	while (std::getline(std::cin, line)) {
		if (line == "quit")
			break;
		if (line.compare(0, 5, "dest ") == 0) {
			std::stringstream ss(line.substr(5));
			OverlayEntry e;
			std::string st;
			ss >> e.platform >> e.handle >> st;
			e.id = "d" + std::to_string(dests.size() + 1);
			const State all[] = {State::Idle, State::Connecting, State::Live, State::Degraded, State::Reconnecting, State::Failed};
			for (State s : all)
				if (st == state_name(s))
					e.state = s;
			dests.push_back(e);
			state.publish(dests, true);
			std::cout << "OK" << std::endl;
			continue;
		}
		if (line == "dests-clear") {
			dests.clear();
			state.publish(dests, false);
			std::cout << "OK" << std::endl;
			continue;
		}
		if (line == "status") {
			std::cout << hub.status_json() << std::endl;
			continue;
		}
		if (line.compare(0, 5, "tone ") == 0) {
			std::stringstream ss(line.substr(5));
			std::string author, platform;
			unsigned rate = 0, ms = 0;
			double hz = 0, amp = 0.5;
			ss >> author >> platform >> rate >> ms >> hz;
			if (!(ss >> amp))
				amp = 0.5;
			std::string pcm;
			size_t n = static_cast<size_t>(rate) * ms / 1000;
			for (size_t i = 0; i < n; ++i) {
				int v = static_cast<int>(std::lround(amp * 32767.0 * std::sin(2.0 * 3.14159265358979 * hz * static_cast<double>(i) / rate)));
				pcm.push_back(static_cast<char>(v & 0xff));
				pcm.push_back(static_cast<char>((v >> 8) & 0xff));
			}
			std::cout << "CLIP " << voice.push(author, platform, rate, pcm) << std::endl;
			continue;
		}
		if (line.compare(0, 4, "wav ") == 0) {
			std::stringstream ss(line.substr(4));
			std::string author, platform, path;
			ss >> author >> platform;
			std::getline(ss >> std::ws, path);
			std::string w;
			uint64_t id = 0;
			// plain 44-byte PCM header (what voice/index.js and make_wav write); anything else is refused
			if (slurp(path, w) && w.size() > 44 && w.compare(0, 4, "RIFF") == 0 && w.compare(36, 4, "data") == 0 &&
			    static_cast<unsigned char>(w[22]) == 1 && static_cast<unsigned char>(w[34]) == 16) {
				uint32_t rate = 0;
				for (int i = 3; i >= 0; --i)
					rate = (rate << 8) | static_cast<unsigned char>(w[24 + i]);
				id = voice.push(author, platform, rate, w.substr(44));
			}
			std::cout << "CLIP " << id << std::endl;
			continue;
		}
		if (line == "vflush") {
			voice.flush();
			std::cout << "OK" << std::endl;
			continue;
		}
		if (line == "vstatus") {
			std::cout << "{\"listeners\":" << voice.listener_count() << ",\"leader\":" << voice.leader()
				  << ",\"local\":" << (voice.local_wanted() ? "true" : "false") << ",\"clips\":" << voice.clip_count() << "}"
				  << std::endl;
			continue;
		}
		if (line.compare(0, 7, "filter ") == 0) {
			ChatFilter f;
			std::stringstream ss(line.substr(7));
			std::string kv;
			while (ss >> kv) {
				size_t eq = kv.find('=');
				if (eq == std::string::npos)
					continue;
				std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
				if (k == "words")
					f.words = csv(v);
				else if (k == "users")
					f.users = csv(v);
				else if (k == "commands")
					f.hide_commands = v == "1";
				else if (k == "bots")
					f.hide_bots = v == "1";
			}
			hub.set_filter(f);
			std::cout << "OK" << std::endl;
		}
	}
	hub.shutdown();
	voice.shutdown();
	srv.stop();
	return 0;
}
