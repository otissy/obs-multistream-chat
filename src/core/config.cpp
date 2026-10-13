#include "config.hpp"
#include "json.hpp"

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

void parse_v2(const json::Value &root, const SecretProtector &p, LoadResult &r)
{
	r.config.auto_start_with_stream = bool_of(root, "auto_start_with_stream", true);
	read_server(root, r);
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
		d.encoder = reuse ? "shared" : "x264";
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
		r.message = "config.json was written by a newer obs-multistream (schema " + std::to_string(schema) +
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
	parse_v2(pr.value, protector, r);
	r.status = LoadStatus::Ok;
	return r;
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
		arr.push(o);
	}
	root.set("destinations", arr);
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
