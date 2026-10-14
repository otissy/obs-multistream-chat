/*
 * obs-multistream-chat: send the stream OBS already encodes to several RTMP(S) destinations, with a
 * dock for the streamer and a browser-source overlay for the audience. No platform APIs.
 */
#include "dock.hpp"
#include "plugin.hpp"
#include "redact.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/base.h>

#include <QApplication>
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

void publish_state()
{
	PluginState &st = *g_state;
	if (st.mgr)
		st.hub.publish(st.mgr->overlay_entries(), st.mgr->main_live());
}

void on_tick()
{
	if (!g_state || !g_state->mgr)
		return;
	g_state->mgr->tick();
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
		g_state->mgr->shutdown();
		if (g_state->server)
			g_state->server->stop();
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
	st.server.reset(new HttpServer(st.hub, load_assets(), so));
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

	g_dock = new MultistreamDock(static_cast<QWidget *>(obs_frontend_get_main_window()));
	obs_frontend_add_dock_by_id("obs-multistream-chat", obs_module_text("Dock.Title"), g_dock);

	st.timer = new QTimer(g_dock);
	QObject::connect(st.timer, &QTimer::timeout, []() { on_tick(); });
	st.timer->start(500);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	publish_state();
	blog(LOG_INFO, "[obs-multistream-chat] version %s loaded", PLUGIN_VERSION);
	return true;
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
	g_state->hub.shutdown();
	if (g_state->server)
		g_state->server->stop();
	g_dock = nullptr; // the frontend owns and destroys the dock widget
	g_state->timer = nullptr;
	g_state->mgr.reset();
	g_state->server.reset();
	delete g_state;
	g_state = nullptr;
}
