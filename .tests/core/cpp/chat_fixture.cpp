// Test fixture: the REAL HttpServer + ChatHub serving the REAL data/overlay chat assets, so node
// and headless Chrome can drive the reader and the overlay end to end.
//   usage: oms_chat_fixture <overlay-dir>      (token from env OMSC_CHAT_TOKEN, like the plugin)
// prints "PORT <n>", then reads stdin lines:
//   filter words=a,b users=x,y commands=0|1 bots=0|1
//   status          -> prints the /chat/status.json body
//   quit
#include "../../../src/core/chat_hub.hpp"
#include "../../../src/core/http_server.hpp"

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
		     {"/icons/twitch.svg", "icons/twitch.svg", "image/svg+xml"},
		     {"/icons/youtube.svg", "icons/youtube.svg", "image/svg+xml"},
		     {"/icons/kick.svg", "icons/kick.svg", "image/svg+xml"},
		     {"/icons/tiktok.svg", "icons/tiktok.svg", "image/svg+xml"},
		     {"/icons/x.svg", "icons/x.svg", "image/svg+xml"},
		     {"/icons/generic.svg", "icons/generic.svg", "image/svg+xml"}};
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
	HttpServer srv(state, assets, ServerOptions(), &hub);
	std::string err;
	if (!srv.start(err)) {
		std::cerr << err << "\n";
		return 4;
	}
	std::cout << "PORT " << srv.port() << std::endl;
	std::string line;
	while (std::getline(std::cin, line)) {
		if (line == "quit")
			break;
		if (line == "status") {
			std::cout << hub.status_json() << std::endl;
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
	srv.stop();
	return 0;
}
