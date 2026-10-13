// Test fixture: serves the REAL data/overlay assets from the REAL HttpServer and walks a
// scripted state sequence so node/Chrome tests can drive the overlay end to end.
//   usage: oms_fixture_server <overlay-dir> [step_ms]
// prints "PORT <n>" then reads commands on stdin: "set <platform> <handle> <state>;..." or "quit".
#include "../../../src/core/http_server.hpp"

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

static State parse_state(const std::string &s)
{
	if (s == "connecting") return State::Connecting;
	if (s == "live") return State::Live;
	if (s == "degraded") return State::Degraded;
	if (s == "reconnecting") return State::Reconnecting;
	if (s == "failed") return State::Failed;
	return State::Idle;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	std::string dir = argv[1];
	AssetMap assets;
	struct F { const char *url; const char *file; const char *type; };
	F files[] = {{"/overlay.html", "overlay.html", "text/html; charset=utf-8"},
		     {"/overlay.js", "overlay.js", "text/javascript; charset=utf-8"},
		     {"/icons/twitch.svg", "icons/twitch.svg", "image/svg+xml"},
		     {"/icons/youtube.svg", "icons/youtube.svg", "image/svg+xml"},
		     {"/icons/kick.svg", "icons/kick.svg", "image/svg+xml"},
		     {"/icons/facebook.svg", "icons/facebook.svg", "image/svg+xml"},
		     {"/icons/tiktok.svg", "icons/tiktok.svg", "image/svg+xml"},
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
	StateHub hub;
	HttpServer srv(hub, assets);
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
		if (line.compare(0, 4, "set ") != 0)
			continue;
		std::vector<OverlayEntry> v;
		std::stringstream items(line.substr(4));
		std::string item;
		int n = 0;
		while (std::getline(items, item, ';')) {
			std::stringstream f(item);
			OverlayEntry e;
			std::string st;
			f >> e.platform >> e.handle >> st;
			e.id = "d" + std::to_string(++n);
			e.state = parse_state(st);
			v.push_back(e);
		}
		hub.publish(v, true);
		std::cout << "OK" << std::endl;
	}
	srv.stop();
	return 0;
}
