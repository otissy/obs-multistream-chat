// Process-wide plugin state, owned by plugin-main.cpp, read by the dock.
#pragma once
#include "config.hpp"
#include "http_server.hpp"
#include "output_manager.hpp"
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
	std::unique_ptr<HttpServer> server;
	std::unique_ptr<OutputManager> mgr;
	QTimer *timer = nullptr;
};

PluginState &plugin();

// Seals pending plaintext keys and writes config.json atomically. false + err on failure.
bool plugin_save_config(std::string &err);
// "http://127.0.0.1:<port>/overlay.html" or "" when the server is not running.
std::string plugin_overlay_url();

} // namespace oms
