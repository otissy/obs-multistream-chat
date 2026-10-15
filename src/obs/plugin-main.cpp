/*
 * obs-multistream-chat: send the stream OBS already encodes to several RTMP(S) destinations, with a
 * dock for the streamer and a browser-source overlay for the audience. No platform APIs.
 */
#include "dock.hpp"
#include "plugin.hpp"
#include "random.hpp"
#include "redact.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/base.h>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QTimer>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multistream-chat", "en-US")

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0"
#endif

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Stream to several RTMP(S) destinations at once and show the connected platforms on stream. "
	       "No platform APIs.";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS Multistream Chat";
}

namespace oms {

namespace {

PluginState *g_state = nullptr;
MultistreamDock *g_dock = nullptr;
bool g_dock_registered = false;     // obs_frontend_add_dock_by_id returned true
bool g_exiting = false;
bool g_dock_retry_allowed = true;   // one more try on OBS_FRONTEND_EVENT_FINISHED_LOADING
QDockWidget *g_fallback = nullptr;  // our own floating window when OBS refused the dock

void log_safe(int level, const std::string &msg)
{
	std::vector<std::string> none;
	blog(level, "[obs-multistream-chat] %s", redact(msg, none).c_str());
}

std::string module_file(const char *rel)
{
	char *p = obs_module_file(rel);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

AssetMap load_assets()
{
	AssetMap m;
	struct Item {
		const char *url;
		const char *file;
		const char *type;
	};
	std::vector<Item> items = {{"/overlay.html", "overlay/overlay.html", "text/html; charset=utf-8"},
				   {"/overlay.js", "overlay/overlay.js", "text/javascript; charset=utf-8"},
				   {"/chat.html", "overlay/chat.html", "text/html; charset=utf-8"},
				   {"/chat.js", "overlay/chat.js", "text/javascript; charset=utf-8"},
				   {"/icons/generic.svg", "overlay/icons/generic.svg", "image/svg+xml"}};
	static std::vector<std::string> urls; // keeps the c-strings below alive
	urls.clear();
	const std::vector<PlatformPreset> &presets = platform_presets();
	for (size_t i = 0; i < presets.size(); ++i) {
		std::string icon = presets[i].icon;
		if (icon == "generic")
			continue;
		urls.push_back("/icons/" + icon + ".svg");
	}
	urls.push_back("/icons/x.svg"); // chat platforms that are not RTMP presets
	{
		// the chat overlay icons that no RTMP preset already added
		const char *chat_icons[] = {"twitch", "youtube", "kick", "tiktok"};
		for (size_t i = 0; i < 4; ++i) {
			std::string u = std::string("/icons/") + chat_icons[i] + ".svg";
			bool have = false;
			for (size_t k = 0; k < urls.size(); ++k)
				if (urls[k] == u)
					have = true;
			if (!have)
				urls.push_back(u);
		}
	}
	for (size_t i = 0; i < items.size(); ++i) {
		std::string path = module_file(items[i].file);
		Asset a;
		a.content_type = items[i].type;
		if (path.empty() || !read_file(path, a.body)) {
			log_safe(LOG_WARNING, std::string("overlay asset missing: ") + items[i].file);
			continue;
		}
		m[items[i].url] = a;
	}
	for (size_t i = 0; i < urls.size(); ++i) {
		std::string path = module_file(("overlay" + urls[i]).c_str());
		Asset a;
		a.content_type = "image/svg+xml";
		if (path.empty() || !read_file(path, a.body)) {
			log_safe(LOG_WARNING, "overlay icon missing: " + urls[i]);
			continue;
		}
		m[urls[i]] = a;
	}
	return m;
}

void poll_reader()
{
	PluginState &st = *g_state;
	if (!st.reader_active)
		return;
	if (st.reader.running())
		return;
	st.reader_active = false;
	st.chat_hub.set_token(""); // a stopped reader's token is dead
	int code = st.reader.exit_code();
	st.chat_hub.mark_all_sources("stopped", "reader stopped");
	st.chat_notice = code == 0 ? std::string() : describe_reader_exit(code);
	log_safe(LOG_INFO, "chat reader ended with code " + std::to_string(code));
}

void publish_state()
{
	PluginState &st = *g_state;
	if (st.mgr)
		st.hub.publish(st.mgr->overlay_entries(), st.mgr->main_live());
}

// ---- the dock ------------------------------------------------------------------------------
// WHY THIS IS NOT IN obs_module_load: the plugins whose docks do show up here (obs-track-monitor,
// casterview) register in obs_module_post_load ("Called when all modules have finished loading",
// obs-module.h) and log "dock registered" after the module list; 0.1.0 to 0.3.0 registered from
// obs_module_load and the return value was ignored, so a refusal was silent and no dock appeared.
// Every outcome is logged now, and a failure is retried once on FINISHED_LOADING.
QWidget *main_window()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

void create_dock(const char *when)
{
	if (g_exiting || g_dock_registered || g_fallback)
		return;
	QWidget *mw = main_window();
	if (!mw) {
		log_safe(LOG_WARNING, std::string("frontend main window not ready (") + when + "), will retry when OBS finishes loading");
		return;
	}
	if (!g_dock)
		g_dock = new MultistreamDock(mw);
	bool ok = obs_frontend_add_dock_by_id("obs-multistream-chat", obs_module_text("Dock.Title"), g_dock);
	if (ok) {
		g_dock_registered = true;
		log_safe(LOG_INFO, std::string("dock registered (") + when + "): View > Docks > OBS Multistream Chat");
	} else {
		log_safe(LOG_WARNING, std::string("dock registration returned false (") + when + "): " +
					      (g_dock_retry_allowed ? "will retry once when OBS finishes loading; " : "") +
					      "Tools > OBS Multistream Chat still opens the panel");
	}
}

// Tools > OBS Multistream Chat: always works, even when OBS refused the dock.
void show_panel()
{
	if (!g_dock && !g_exiting)
		create_dock("tools menu");
	if (!g_dock) {
		log_safe(LOG_ERROR, "cannot show the panel: the frontend main window does not exist");
		return;
	}
	QDockWidget *host = qobject_cast<QDockWidget *>(g_dock->parentWidget());
	if (!host) {
		if (!g_fallback) {
			g_fallback = new QDockWidget(QString::fromUtf8(obs_module_text("Dock.Title")), main_window());
			g_fallback->setObjectName("obs-multistream-chat-fallback");
			g_fallback->setWidget(g_dock);
			g_fallback->setFloating(true);
			g_fallback->resize(760, 520);
			log_safe(LOG_INFO, "panel opened in its own floating window (the OBS dock was not registered)");
		}
		host = g_fallback;
	}
	host->show();
	host->raise();
	host->activateWindow();
}

void install_tools_entry()
{
	void *a = obs_frontend_add_tools_menu_qaction(obs_module_text("Dock.Title"));
	if (!a) {
		log_safe(LOG_WARNING, "could not add Tools > OBS Multistream Chat");
		return;
	}
	QObject::connect(static_cast<QAction *>(a), &QAction::triggered, []() { show_panel(); });
	log_safe(LOG_INFO, "Tools > OBS Multistream Chat added");
}

void on_tick()
{
	if (!g_state || !g_state->mgr)
		return;
	g_state->mgr->tick();
	poll_reader();
	publish_state();
	if (g_dock)
		g_dock->refresh();
}

void on_frontend_event(enum obs_frontend_event ev, void *)
{
	if (!g_state || !g_state->mgr)
		return;
	switch (ev) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED: g_state->mgr->on_main_stream_started(); break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING: g_state->mgr->on_main_stream_stopping(); break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED: g_state->mgr->on_main_stream_stopped(); break;
	case OBS_FRONTEND_EVENT_EXIT:
		if (g_state->timer)
			g_state->timer->stop(); // the dock (and the timer parented to it) is about to be destroyed
		g_state->timer = nullptr;
		g_dock = nullptr;
		g_fallback = nullptr;
		g_exiting = true;
		g_state->mgr->shutdown();
		g_state->reader.stop_blocking();
		g_state->reader_active = false;
		g_state->chat_hub.set_token("");
		g_state->chat_hub.shutdown();
		if (g_state->server)
			g_state->server->stop();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		if (!g_dock_registered && !g_fallback && g_dock_retry_allowed) {
			g_dock_retry_allowed = false;
			create_dock("finished loading");
		}
		break;
	default: break;
	}
	publish_state();
	if (g_dock)
		g_dock->refresh();
}

} // namespace

PluginState &plugin()
{
	return *g_state;
}

std::string plugin_chat_url()
{
	if (!g_state || !g_state->server || !g_state->server->running())
		return std::string();
	return "http://127.0.0.1:" + std::to_string(g_state->server->port()) + "/chat.html";
}

void plugin_apply_chat_filter()
{
	ChatFilter f;
	const ChatConfig &c = g_state->cfg.chat;
	f.words = c.blocked_words;
	f.users = c.blocked_users;
	f.hide_commands = c.hide_commands;
	f.hide_bots = c.hide_bots;
	g_state->chat_hub.set_filter(f);
}

bool plugin_reader_running()
{
	return g_state && g_state->reader_active && g_state->reader.running();
}

void plugin_stop_reader()
{
	if (!g_state)
		return;
	g_state->reader.request_stop();
	g_state->chat_notice.clear();
}

bool plugin_start_reader(std::string &err)
{
	PluginState &st = *g_state;
	if (st.reader_active && st.reader.running()) {
		err = "the chat reader is already running";
		return false;
	}
	if (!st.server || !st.server->running()) {
		err = "The local server is not running, so the chat reader has nowhere to send messages.";
		return false;
	}
	bool any = false;
	for (size_t i = 0; i < st.cfg.chat.sources.size(); ++i)
		any = any || st.cfg.chat.sources[i].enabled;
	if (!any) {
		err = "Add and enable at least one chat source first.";
		return false;
	}
	std::string js = module_file("reader/index.js");
	if (js.empty()) {
		err = "reader/index.js is missing from the plugin folder (reinstall the plugin).";
		return false;
	}
	size_t slash = st.config_path.find_last_of("/\\");
	std::string dir = slash == std::string::npos ? std::string() : st.config_path.substr(0, slash + 1);
	if (dir.empty()) {
		err = "No plugin config folder.";
		return false;
	}
	std::string cfg_file = dir + "chat-reader.json";
	std::string werr;
	if (!write_file_atomic(cfg_file, build_reader_config(st.cfg, st.server->port()), werr)) {
		err = "Could not write the reader config: " + werr;
		return false;
	}
	std::string token = random_token_hex(32);
	if (token.empty()) {
		err = "No secure random numbers available, not starting the chat reader.";
		return false;
	}
	plugin_apply_chat_filter();
	st.chat_hub.set_token(token);
	if (!st.reader.start(st.cfg.chat.node_path, js, cfg_file, dir + "chat-reader.log", token, err)) {
		st.chat_hub.set_token("");
		return false;
	}
	token.assign(token.size(), '\0');
	st.reader_active = true;
	st.chat_notice.clear();
	st.chat_hub.mark_all_sources("starting", "");
	log_safe(LOG_INFO, "chat reader started");
	return true;
}

bool plugin_save_config(std::string &err)
{
	PluginState &st = *g_state;
	if (st.read_only) {
		err = "config.json was written by a newer obs-multistream-chat and is read-only";
		return false;
	}
	if (!st.protector) {
		err = "stream keys cannot be stored on this system (no Windows DPAPI)";
		return false;
	}
	if (!seal_keys(st.cfg, *st.protector, err))
		return false;
	return save_config_file(st.config_path, st.cfg, err);
}

std::string plugin_overlay_url()
{
	if (!g_state || !g_state->server || !g_state->server->running())
		return std::string();
	return "http://127.0.0.1:" + std::to_string(g_state->server->port()) + "/overlay.html";
}

} // namespace oms

bool obs_module_load(void)
{
	using namespace oms;
	g_state = new PluginState();
	PluginState &st = *g_state;

	st.protector = make_dpapi_protector();
	if (!st.protector) {
		st.notice = "Windows DPAPI is unavailable: stream keys cannot be saved.";
		st.protector.reset(new InsecureTestProtector()); // only so the code paths have a protector; saving is refused below
	}
	char *cp = obs_module_config_path("config.json");
	st.config_path = cp ? cp : "";
	bfree(cp);

	bool real_protector = make_dpapi_protector() != nullptr;
	LoadResult lr = load_config_file(st.config_path, *st.protector, st.clock);
	st.cfg = lr.config;
	st.read_only = lr.read_only || !real_protector;
	if (!lr.message.empty())
		st.notice = lr.message;
	for (size_t i = 0; i < lr.warnings.size(); ++i)
		log_safe(LOG_WARNING, lr.warnings[i]);
	if (!lr.warnings.empty() && st.notice.empty())
		st.notice = std::to_string(lr.warnings.size()) + " destination(s) in config.json were skipped; see the OBS log.";
	log_safe(LOG_INFO, "config loaded: " + std::to_string(st.cfg.destinations.size()) + " destination(s)");

	st.mgr.reset(new OutputManager(st.cfg, *st.protector, st.clock));

	ServerOptions so;
	so.preferred_port = st.cfg.server.port ? st.cfg.server.port : st.cfg.server.last_port;
	plugin_apply_chat_filter();
	st.server.reset(new HttpServer(st.hub, load_assets(), so, &st.chat_hub));
	std::string err;
	if (!st.server->start(err)) {
		st.notice = "Overlay server could not start: " + err;
		log_safe(LOG_ERROR, st.notice);
	} else {
		log_safe(LOG_INFO, "overlay server on 127.0.0.1:" + std::to_string(st.server->port()));
		if (st.server->port() != st.cfg.server.last_port) {
			st.cfg.server.last_port = st.server->port();
			lr.needs_save = true;
		}
		if (st.cfg.server.port && st.server->port_fell_back())
			st.notice = "Configured port " + std::to_string(st.cfg.server.port) + " is taken; using " +
				    std::to_string(st.server->port()) + ". Update the browser source URL.";
	}
	if (lr.needs_save && !st.read_only) {
		if (!plugin_save_config(err))
			log_safe(LOG_WARNING, "could not save config: " + err);
	}

	// The dock itself is created in obs_module_post_load (see create_dock). The tick timer does not depend on it.
	st.timer = new QTimer(qApp);
	QObject::connect(st.timer, &QTimer::timeout, []() { on_tick(); });
	st.timer->start(500);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	publish_state();
	if (st.cfg.chat.start_with_obs && !st.read_only) {
		std::string cerr;
		if (!plugin_start_reader(cerr))
			st.chat_notice = cerr; // never fatal: the plugin works without chat
	}
	blog(LOG_INFO, "[obs-multistream-chat] version %s loaded", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	using namespace oms;
	if (!g_state)
		return;
	create_dock("post load");
	install_tools_entry();
	if (g_dock)
		g_dock->refresh();
}

void obs_module_unload(void)
{
	using namespace oms;
	if (!g_state)
		return;
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (g_state->timer)
		g_state->timer->stop();
	if (g_state->mgr)
		g_state->mgr->shutdown();
	g_state->reader.stop_blocking();
	g_state->chat_hub.set_token("");
	g_state->chat_hub.shutdown();
	g_state->hub.shutdown();
	if (g_state->server)
		g_state->server->stop();
	g_dock = nullptr; // the frontend owns and destroys the dock widget (or our fallback window, a child of the main window)
	delete g_state->timer;
	g_state->timer = nullptr;
	g_state->mgr.reset();
	g_state->server.reset();
	delete g_state;
	g_state = nullptr;
}
