// Process-wide plugin state, owned by plugin-main.cpp, read by the dock.
#pragma once
#include "chat_hub.hpp"
#include "config.hpp"
#include "http_server.hpp"
#include "output_manager.hpp"
#include "reader_process.hpp"
#include "secret.hpp"
#include "state_feed.hpp"

#include <memory>
#include <string>
#include <vector>

class QTimer;

namespace oms {

struct PluginState {
	Config cfg;
	std::unique_ptr<SecretProtector> protector;
	std::string config_path;
	bool read_only = false;           // config written by a newer version: never save
	std::string notice;               // shown in the dock (config problems, no DPAPI, server errors)
	SystemClock clock;
	StateHub hub;
	ChatHub chat_hub{clock};          // merge point for the chat reader (loopback only)
	ReaderProcess reader;             // the hidden node + Chrome child, only ever started by the user
	bool reader_active = false;       // we started it and have not seen it end yet
	std::string chat_notice;          // shown in the Chat tab
	std::unique_ptr<HttpServer> server;
	std::unique_ptr<OutputManager> mgr;
	QTimer *timer = nullptr;
};

PluginState &plugin();

// Seals pending plaintext keys and writes config.json atomically. false + err on failure.
bool plugin_save_config(std::string &err);
// "http://127.0.0.1:<port>/overlay.html" or "" when the server is not running.
std::string plugin_overlay_url();
// "http://127.0.0.1:<port>/chat.html" or "".
std::string plugin_chat_url();
// Chat reader control (UI thread). start refuses with a sentence in err.
bool plugin_start_reader(std::string &err);
void plugin_stop_reader();
bool plugin_reader_running();
void plugin_apply_chat_filter(); // pushes blocklist + switches from the config into the hub

} // namespace oms
