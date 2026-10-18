#include "config.hpp"
#include "chat_targets.hpp"
#include "json.hpp"

#include <cctype>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace oms {

namespace {

std::string str_of(const json::Value &obj, const char *key, const std::string &def = std::string())
{
	const json::Value *v = obj.find(key);
	return (v && v->is_string()) ? v->as_string() : def;
}

bool bool_of(const json::Value &obj, const char *key, bool def)
{
	const json::Value *v = obj.find(key);
	return (v && v->is_bool()) ? v->as_bool() : def;
}

int int_of(const json::Value &obj, const char *key, int def, int lo, int hi, bool *clamped = nullptr)
{
	const json::Value *v = obj.find(key);
	if (!v || !v->is_number())
		return def;
	double n = v->as_number();
	if (n < lo || n > hi) {
		if (clamped)
			*clamped = true;
		return def;
	}
	return static_cast<int>(n);
}

std::string label_of(const Destination &d, size_t index)
{
	if (!d.name.empty() && !has_control_chars(d.name) && d.name.size() <= 64)
		return "\"" + d.name + "\"";
	return "#" + std::to_string(index + 1);
}

// Seals d.key if set. A failure leaves the destination without a key (the user must re-enter it).
bool seal_one(Destination &d, const SecretProtector &p)
{
	if (d.key.empty())
		return true;
	std::string token;
	bool ok = p.protect(d.key, token);
	secure_zero(d.key);
	if (!ok)
		return false;
	d.key_token = token;
	return true;
}

void add_dest(const Destination &parsed, size_t index, LoadResult &r, std::set<std::string> &ids)
{
	Destination d = parsed;
	if (!is_valid_id(d.id) || ids.count(d.id)) {
		unsigned seed = static_cast<unsigned>(index + 1);
		do {
			d.id = make_id(seed++);
		} while (ids.count(d.id));
	}
	std::vector<ValidationIssue> issues = validate_destination(d, false);
	if (!issues.empty()) {
		r.warnings.push_back("destination " + label_of(d, index) + " skipped: " + issues[0].field + ": " +
				     issues[0].message);
		secure_zero(d.key);
		return;
	}
	if (r.config.destinations.size() >= static_cast<size_t>(kMaxDestinations)) {
		r.warnings.push_back("destination " + label_of(d, index) + " skipped: more than " +
				     std::to_string(kMaxDestinations) + " destinations");
		secure_zero(d.key);
		return;
	}
	ids.insert(d.id);
	r.config.destinations.push_back(d);
}

void read_server(const json::Value &root, LoadResult &r)
{
	const json::Value *srv = root.find("server");
	if (!srv || !srv->is_object())
		return;
	bool clamped = false;
	r.config.server.port = int_of(*srv, "port", 0, 0, 65535, &clamped);
	r.config.server.last_port = int_of(*srv, "last_port", 0, 0, 65535, &clamped);
	if (clamped)
		r.warnings.push_back("server port out of range: using automatic port");
}

std::string clean_list_entry(const std::string &s)
{
	std::string o;
	for (size_t i = 0; i < s.size() && o.size() < 60; ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c >= 0x20 && c != 0x7f)
			o.push_back(static_cast<char>(c));
	}
	size_t a = 0, b = o.size();
	while (a < b && o[a] == ' ')
		++a;
	while (b > a && o[b - 1] == ' ')
		--b;
	return o.substr(a, b - a);
}

void read_string_list(const json::Value &obj, const char *key, std::vector<std::string> &out, LoadResult &r)
{
	const json::Value *v = obj.find(key);
	if (!v)
		return;
	if (!v->is_array()) {
		r.warnings.push_back(std::string("chat.") + key + " is not a list: ignored");
		return;
	}
	for (size_t i = 0; i < v->size() && out.size() < kMaxChatListEntries; ++i) {
		if (!v->at(i).is_string())
			continue;
		std::string e = clean_list_entry(v->at(i).as_string());
		if (!e.empty())
			out.push_back(e);
	}
}

void read_chat(const json::Value &root, LoadResult &r)
{
	const json::Value *c = root.find("chat");
	if (!c)
		return;
	if (!c->is_object()) {
		r.warnings.push_back("\"chat\" is not an object: ignored");
		return;
	}
	ChatConfig &cc = r.config.chat;
	cc.start_with_obs = bool_of(*c, "start_with_obs", false);
	cc.auto_start_with_stream = bool_of(*c, "auto_start_with_stream", true);
	cc.hide_commands = bool_of(*c, "hide_commands", false);
	cc.hide_bots = bool_of(*c, "hide_bots", false);
	cc.quiet_minutes = int_of(*c, "quiet_minutes", 5, 1, 120);
	std::string ua = str_of(*c, "user_agent", "chrome");
	if (ua == "chrome" || ua == "headless")
		cc.user_agent = ua;
	else
		r.warnings.push_back("chat.user_agent must be \"chrome\" or \"headless\": using \"chrome\"");
	std::string np = str_of(*c, "node_path");
	if (valid_node_path(np))
		cc.node_path = np;
	else
		r.warnings.push_back("chat.node_path is not an absolute path to node.exe: ignored");
	read_string_list(*c, "blocked_words", cc.blocked_words, r);
	read_string_list(*c, "blocked_users", cc.blocked_users, r);
	const json::Value *arr = c->find("sources");
	if (!arr)
		return;
	if (!arr->is_array()) {
		r.warnings.push_back("chat.sources is not a list: ignored");
		return;
	}
	std::set<std::string> ids;
	for (size_t i = 0; i < arr->size(); ++i) {
		const json::Value &o = arr->at(i);
		if (!o.is_object()) {
			r.warnings.push_back("chat source #" + std::to_string(i + 1) + " skipped: not an object");
			continue;
		}
		ChatSource s;
		s.id = str_of(o, "id");
		s.platform = str_of(o, "platform");
		s.enabled = bool_of(o, "enabled", true);
		std::string target = str_of(o, "target");
		std::string norm, err;
		if (!normalize_chat_target(s.platform, target, norm, err) || norm != target) {
			r.warnings.push_back("chat source #" + std::to_string(i + 1) + " skipped: " +
					     (err.empty() ? std::string("target is not normalized") : err));
			continue;
		}
		s.target = norm;
		if (!is_valid_id(s.id) || ids.count(s.id)) {
			unsigned seed = static_cast<unsigned>(i + 1);
			do {
				s.id = "c" + std::to_string(seed++);
			} while (ids.count(s.id));
		}
		if (r.config.chat.sources.size() >= static_cast<size_t>(kMaxChatSources)) {
			r.warnings.push_back("chat source #" + std::to_string(i + 1) + " skipped: more than " +
					     std::to_string(kMaxChatSources) + " chat sources");
			continue;
		}
		ids.insert(s.id);
		r.config.chat.sources.push_back(s);
	}
}

void read_extras(const json::Value &o, Destination &d, size_t index, LoadResult &r)
{
	const json::Value *arr = o.find("venc_extra");
	if (!arr)
		return;
	if (!arr->is_array()) {
		r.warnings.push_back("destination " + label_of(d, index) + ": venc_extra is not a list: ignored");
		return;
	}
	for (size_t i = 0; i < arr->size() && d.venc_extra.size() < kMaxExtraSettings; ++i) {
		const json::Value &e = arr->at(i);
		if (!e.is_object())
			continue;
		ExtraSetting x;
		x.key = str_of(e, "key");
		std::string t = str_of(e, "type");
		x.type = t.size() == 1 ? t[0] : '?';
		x.value = str_of(e, "value");
		d.venc_extra.push_back(x); // validated (and the destination skipped) by validate_destination
	}
}

void read_v4_fields(const json::Value &o, Destination &d, size_t index, LoadResult &r)
{
	d.canvas = str_of(o, "canvas");
	d.video_encoder = str_of(o, "video_encoder", "obs_x264");
	d.keyint_sec = int_of(o, "keyint_sec", 2, 0, 10);
	d.preset = str_of(o, "preset", "veryfast");
	d.profile = str_of(o, "profile");
	d.rate_control = str_of(o, "rate_control", "CBR");
	d.out_width = int_of(o, "out_width", 0, 0, 8192);
	d.out_height = int_of(o, "out_height", 0, 0, 8192);
	d.scale_type = int_of(o, "scale_type", 3, 0, 5);
	d.fps_div = int_of(o, "fps_div", 1, 1, 4);
	d.audio_track = int_of(o, "audio_track", 0, 0, kMaxAudioTracks);
	d.audio_bitrate_kbps = int_of(o, "audio_bitrate_kbps", 160, 64, 320);
	d.delay_sec = int_of(o, "delay_sec", 0, 0, 3600);
	d.delay_preserve = bool_of(o, "delay_preserve", false);
	const json::Value *a = o.find("aitum");
	if (a && a->is_object()) {
		d.aitum_mode = str_of(*a, "mode", "off");
		d.aitum_output = str_of(*a, "output");
	}
	read_extras(o, d, index, r);
}

void parse_v2(const json::Value &root, int schema, const SecretProtector &p, LoadResult &r)
{
	r.config.auto_start_with_stream = bool_of(root, "auto_start_with_stream", true);
	read_server(root, r);
	read_chat(root, r);
	const json::Value *arr = root.find("destinations");
	if (!arr)
		return;
	if (!arr->is_array()) {
		r.warnings.push_back("\"destinations\" is not a list: ignored");
		return;
	}
	std::set<std::string> ids;
	for (size_t i = 0; i < arr->size(); ++i) {
		const json::Value &o = arr->at(i);
		if (!o.is_object()) {
			r.warnings.push_back("destination #" + std::to_string(i + 1) + " skipped: not an object");
			continue;
		}
		Destination d;
		d.id = str_of(o, "id");
		d.name = str_of(o, "name");
		d.platform = str_of(o, "platform", "custom");
		d.url = str_of(o, "url");
		d.handle = str_of(o, "handle");
		d.enabled = bool_of(o, "enabled", true);
		d.encoder = str_of(o, "encoder", "shared");
		d.bitrate_kbps = int_of(o, "bitrate_kbps", 0, 0, 51000);
		d.auto_start = bool_of(o, "auto_start", true);
		if (schema < 4) {
			// 0.2.0 "x264" mode == own encoder obs_x264 / veryfast / keyint 2 / CBR (the new defaults)
			if (d.encoder == "x264")
				d.encoder = "own";
		} else {
			read_v4_fields(o, d, i, r);
		}
		d.key_token = str_of(o, "key_enc");
		if (d.key_token.size() > 4096)
			d.key_token.clear();
		d.key = str_of(o, "key"); // hand-edited plaintext convenience: sealed below
		if (!d.key.empty()) {
			if (!seal_one(d, p))
				r.warnings.push_back("destination " + label_of(d, i) + ": key could not be encrypted, re-enter it");
			r.needs_save = true;
		}
		add_dest(d, i, r, ids);
	}
}

// Schema 1: {"schema":1,"targets":[{"title","platform","server","key","shown_name","reuse_encoder","enabled"}]}
// A hand-authored import format; it never shipped, but keys in it are plaintext, so migration seals them.
void parse_v1(const json::Value &root, const SecretProtector &p, LoadResult &r)
{
	read_server(root, r);
	const json::Value *arr = root.find("targets");
	if (!arr || !arr->is_array())
		return;
	std::set<std::string> ids;
	for (size_t i = 0; i < arr->size(); ++i) {
		const json::Value &o = arr->at(i);
		if (!o.is_object()) {
			r.warnings.push_back("target #" + std::to_string(i + 1) + " skipped: not an object");
			continue;
		}
		Destination d;
		d.id = make_id(static_cast<unsigned>(i + 1));
		d.name = str_of(o, "title");
		d.platform = str_of(o, "platform", "custom");
		d.url = str_of(o, "server");
		d.handle = str_of(o, "shown_name");
		d.enabled = bool_of(o, "enabled", true);
		bool reuse = bool_of(o, "reuse_encoder", true);
		d.encoder = reuse ? "shared" : "own";
		d.bitrate_kbps = reuse ? 0 : 2500;
		d.auto_start = true;
		d.key = str_of(o, "key");
		if (!seal_one(d, p))
			r.warnings.push_back("target " + label_of(d, i) + ": key could not be encrypted, re-enter it");
		add_dest(d, i, r, ids);
	}
}

} // namespace

LoadResult parse_config(const std::string &text, const SecretProtector &protector)
{
	LoadResult r;
	json::ParseResult pr = json::parse(text);
	if (!pr.ok || !pr.value.is_object()) {
		r.status = LoadStatus::Malformed;
		r.message = pr.ok ? "config.json is not a JSON object" : "config.json is not valid JSON (" + pr.error + ")";
		return r;
	}
	const json::Value *sv = pr.value.find("schema");
	int schema = (sv && sv->is_number()) ? static_cast<int>(sv->as_number()) : 0;
	if (schema > kConfigSchema) {
		r.status = LoadStatus::TooNew;
		r.read_only = true;
		r.message = "config.json was written by a newer obs-multistream-chat (schema " + std::to_string(schema) +
			    "); it is loaded read-only and will not be overwritten";
		return r;
	}
	if (schema < 1) {
		r.status = LoadStatus::Malformed;
		r.message = "config.json has no valid \"schema\" number";
		return r;
	}
	if (schema == 1) {
		parse_v1(pr.value, protector, r);
		r.status = LoadStatus::Migrated;
		r.needs_save = true;
		r.message = "config.json was upgraded from schema 1; stream keys are now encrypted";
		return r;
	}
	parse_v2(pr.value, schema, protector, r);
	if (schema < kConfigSchema) {
		// 2 -> 3 added the "chat" block, 3 -> 4 added canvas / audio track / encoder options / Aitum link:
		// absent fields take their defaults, "x264" becomes "own"; saved as the current schema
		r.status = LoadStatus::Migrated;
		r.needs_save = true;
		return r;
	}
	r.status = LoadStatus::Ok;
	return r;
}

bool valid_node_path(const std::string &p)
{
	if (p.empty())
		return true;
	if (p.size() > 260)
		return false;
	for (size_t i = 0; i < p.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(p[i]);
		if (c < 0x20 || c == 0x7f || c == '"' || c == '<' || c == '>' || c == '|' || c == '*' || c == '?')
			return false;
	}
	bool drive = p.size() > 3 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
	bool unc = p.compare(0, 2, "\\\\") == 0;
	if (!drive && !unc)
		return false;
	std::string lower;
	for (size_t i = 0; i < p.size(); ++i)
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(p[i]))));
	const std::string tail = "node.exe";
	if (lower.size() < tail.size() + 1 || lower.compare(lower.size() - tail.size(), tail.size(), tail) != 0)
		return false;
	char before = lower[lower.size() - tail.size() - 1];
	return before == '\\' || before == '/';
}

std::string build_reader_config(const Config &cfg, int port)
{
	json::Value root = json::Value::object();
	root.set("v", json::Value::number(1));
	root.set("port", json::Value::number(port));
	root.set("quiet_minutes", json::Value::number(cfg.chat.quiet_minutes));
	root.set("user_agent", json::Value::string(cfg.chat.user_agent));
	json::Value arr = json::Value::array();
	for (size_t i = 0; i < cfg.chat.sources.size(); ++i) {
		const ChatSource &s = cfg.chat.sources[i];
		if (!s.enabled)
			continue;
		json::Value o = json::Value::object();
		o.set("id", json::Value::string(s.id));
		o.set("platform", json::Value::string(s.platform));
		o.set("target", json::Value::string(s.target));
		arr.push(o);
	}
	root.set("sources", arr);
	return json::dump(root, true) + "\n";
}

bool read_file(const std::string &path, std::string &out)
{
	std::ifstream f(fs::u8path(path), std::ios::binary);
	if (!f)
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	if (f.bad())
		return false;
	out = ss.str();
	return true;
}

bool write_file_atomic(const std::string &path, const std::string &data, std::string &err)
{
	std::error_code ec;
	fs::path target = fs::u8path(path);
	if (target.has_parent_path()) {
		fs::create_directories(target.parent_path(), ec);
		if (ec) {
			err = "cannot create directory: " + ec.message();
			return false;
		}
	}
	fs::path tmp = target;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) {
			err = "cannot open temporary file for writing";
			return false;
		}
		f.write(data.data(), static_cast<std::streamsize>(data.size()));
		f.flush();
		if (!f) {
			err = "write failed";
			return false;
		}
	}
	fs::rename(tmp, target, ec);
	if (ec) {
		std::error_code ignore;
		fs::remove(tmp, ignore);
		err = "cannot replace config file: " + ec.message();
		return false;
	}
	return true;
}

LoadResult load_config_file(const std::string &path, const SecretProtector &protector, const Clock &clock)
{
	std::error_code ec;
	fs::path p = fs::u8path(path);
	if (!fs::exists(p, ec)) {
		LoadResult r;
		r.status = LoadStatus::Created;
		return r;
	}
	std::string text;
	if (!read_file(path, text)) {
		LoadResult r;
		r.status = LoadStatus::IoError;
		r.message = "config.json exists but could not be read";
		return r;
	}
	LoadResult r = parse_config(text, protector);
	if (r.status == LoadStatus::Malformed) {
		fs::path bad = p;
		bad += ".bad-" + std::to_string(clock.now_ms());
		fs::rename(p, bad, ec);
		if (ec)
			r.message += " (could not move it aside: " + ec.message() + ")";
		else
			r.message += "; the file was kept as " + bad.filename().u8string();
	}
	return r;
}

bool seal_keys(Config &cfg, const SecretProtector &protector, std::string &err)
{
	bool all = true;
	for (size_t i = 0; i < cfg.destinations.size(); ++i) {
		if (!seal_one(cfg.destinations[i], protector)) {
			all = false;
			err = "could not encrypt the stream key for \"" + cfg.destinations[i].name + "\"";
		}
	}
	return all;
}

bool serialize_config(const Config &cfg, std::string &out, std::string &err)
{
	json::Value root = json::Value::object();
	root.set("schema", json::Value::number(kConfigSchema));
	json::Value srv = json::Value::object();
	srv.set("port", json::Value::number(cfg.server.port));
	srv.set("last_port", json::Value::number(cfg.server.last_port));
	root.set("server", srv);
	root.set("auto_start_with_stream", json::Value::boolean(cfg.auto_start_with_stream));
	json::Value arr = json::Value::array();
	for (size_t i = 0; i < cfg.destinations.size(); ++i) {
		const Destination &d = cfg.destinations[i];
		if (!d.key.empty()) {
			err = "refusing to save: destination \"" + d.name + "\" still holds a plaintext key";
			return false;
		}
		json::Value o = json::Value::object();
		o.set("id", json::Value::string(d.id));
		o.set("name", json::Value::string(d.name));
		o.set("platform", json::Value::string(d.platform));
		o.set("url", json::Value::string(d.url));
		o.set("key_enc", json::Value::string(d.key_token));
		o.set("handle", json::Value::string(d.handle));
		o.set("enabled", json::Value::boolean(d.enabled));
		o.set("encoder", json::Value::string(d.encoder));
		o.set("bitrate_kbps", json::Value::number(d.bitrate_kbps));
		o.set("auto_start", json::Value::boolean(d.auto_start));
		o.set("canvas", json::Value::string(d.canvas));
		o.set("video_encoder", json::Value::string(d.video_encoder));
		o.set("keyint_sec", json::Value::number(d.keyint_sec));
		o.set("preset", json::Value::string(d.preset));
		o.set("profile", json::Value::string(d.profile));
		o.set("rate_control", json::Value::string(d.rate_control));
		o.set("out_width", json::Value::number(d.out_width));
		o.set("out_height", json::Value::number(d.out_height));
		o.set("scale_type", json::Value::number(d.scale_type));
		o.set("fps_div", json::Value::number(d.fps_div));
		o.set("audio_track", json::Value::number(d.audio_track));
		o.set("audio_bitrate_kbps", json::Value::number(d.audio_bitrate_kbps));
		o.set("delay_sec", json::Value::number(d.delay_sec));
		o.set("delay_preserve", json::Value::boolean(d.delay_preserve));
		json::Value ex = json::Value::array();
		for (size_t k = 0; k < d.venc_extra.size(); ++k) {
			json::Value e = json::Value::object();
			e.set("key", json::Value::string(d.venc_extra[k].key));
			e.set("type", json::Value::string(std::string(1, d.venc_extra[k].type)));
			e.set("value", json::Value::string(d.venc_extra[k].value));
			ex.push(e);
		}
		o.set("venc_extra", ex);
		json::Value au = json::Value::object();
		au.set("mode", json::Value::string(d.aitum_mode));
		au.set("output", json::Value::string(d.aitum_output));
		o.set("aitum", au);
		arr.push(o);
	}
	root.set("destinations", arr);
	json::Value chat = json::Value::object();
	chat.set("start_with_obs", json::Value::boolean(cfg.chat.start_with_obs));
	chat.set("auto_start_with_stream", json::Value::boolean(cfg.chat.auto_start_with_stream));
	chat.set("node_path", json::Value::string(cfg.chat.node_path));
	chat.set("quiet_minutes", json::Value::number(cfg.chat.quiet_minutes));
	chat.set("user_agent", json::Value::string(cfg.chat.user_agent));
	chat.set("hide_commands", json::Value::boolean(cfg.chat.hide_commands));
	chat.set("hide_bots", json::Value::boolean(cfg.chat.hide_bots));
	json::Value words = json::Value::array();
	for (size_t i = 0; i < cfg.chat.blocked_words.size(); ++i)
		words.push(json::Value::string(cfg.chat.blocked_words[i]));
	chat.set("blocked_words", words);
	json::Value users = json::Value::array();
	for (size_t i = 0; i < cfg.chat.blocked_users.size(); ++i)
		users.push(json::Value::string(cfg.chat.blocked_users[i]));
	chat.set("blocked_users", users);
	json::Value srcs = json::Value::array();
	for (size_t i = 0; i < cfg.chat.sources.size(); ++i) {
		json::Value o = json::Value::object();
		o.set("id", json::Value::string(cfg.chat.sources[i].id));
		o.set("platform", json::Value::string(cfg.chat.sources[i].platform));
		o.set("target", json::Value::string(cfg.chat.sources[i].target));
		o.set("enabled", json::Value::boolean(cfg.chat.sources[i].enabled));
		srcs.push(o);
	}
	chat.set("sources", srcs);
	root.set("chat", chat);
	out = json::dump(root, true) + "\n";
	return true;
}

bool save_config_file(const std::string &path, const Config &cfg, std::string &err)
{
	std::string text;
	if (!serialize_config(cfg, text, err))
		return false;
	return write_file_atomic(path, text, err);
}

bool unlock_key(const Destination &d, const SecretProtector &protector, std::string &plain)
{
	plain.clear();
	if (d.key_token.empty())
		return false;
	return protector.unprotect(d.key_token, plain);
}

} // namespace oms
