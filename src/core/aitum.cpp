#include "aitum.hpp"
#include "json.hpp"
#include "secret.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace oms {

namespace {

std::string str_of(const json::Value &o, const char *k)
{
	const json::Value *v = o.find(k);
	return (v && v->is_string()) ? v->as_string() : std::string();
}

// number -> int when it is a whole number inside [lo, hi]; else def.
int int_of(const json::Value &o, const char *k, int def, int lo, int hi)
{
	const json::Value *v = o.find(k);
	if (!v || !v->is_number())
		return def;
	double n = v->as_number();
	if (!(n >= lo && n <= hi) || std::floor(n) != n)
		return def;
	return static_cast<int>(n);
}

bool bool_of(const json::Value &o, const char *k, bool def)
{
	const json::Value *v = o.find(k);
	return (v && v->is_bool()) ? v->as_bool() : def;
}

std::string clean(const std::string &s, size_t max)
{
	std::string o;
	for (size_t i = 0; i < s.size() && o.size() < max; ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c >= 0x20 && c != 0x7F)
			o.push_back(static_cast<char>(c));
	}
	return o;
}

// The same key may sit on the output or on its first video encoder entry.
const json::Value *find_in(const json::Value &out, const json::Value *enc, const char *key)
{
	const json::Value *v = out.find(key);
	if (!v && enc)
		v = enc->find(key);
	return v;
}

bool key_is_mapped(const std::string &k)
{
	return k == "bitrate" || k == "keyint_sec" || k == "preset" || k == "profile" || k == "rate_control";
}

bool ident_ok(const std::string &k)
{
	if (k.empty() || k.size() > 40)
		return false;
	for (size_t i = 0; i < k.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(k[i]);
		if (!(std::islower(c) || std::isdigit(c) || c == '_'))
			return false;
	}
	return true;
}

std::string num_text(double n)
{
	char buf[40];
	if (std::floor(n) == n && std::fabs(n) < 1e15)
		std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
	else
		std::snprintf(buf, sizeof buf, "%.6g", n);
	return buf;
}

void read_settings(const json::Value &settings, AitumOutput &o)
{
	if (!settings.is_object())
		return;
	o.bitrate_kbps = int_of(settings, "bitrate", 0, 0, 1000000);
	o.keyint_sec = int_of(settings, "keyint_sec", -1, 0, 100);
	o.preset = clean(str_of(settings, "preset"), 32);
	o.profile = clean(str_of(settings, "profile"), 32);
	o.rate_control = clean(str_of(settings, "rate_control"), 32);
	for (size_t i = 0; i < settings.size() && o.extra.size() < kMaxExtraSettings; ++i) {
		const std::string &k = settings.key_at(i);
		const json::Value &v = settings.value_at(i);
		if (key_is_mapped(k) || !ident_ok(k))
			continue;
		ExtraSetting e;
		e.key = k;
		if (v.is_bool()) {
			e.type = 'b';
			e.value = v.as_bool() ? "true" : "false";
		} else if (v.is_number()) {
			double n = v.as_number();
			e.type = std::floor(n) == n ? 'i' : 'd';
			e.value = num_text(n);
		} else if (v.is_string() && v.as_string().size() <= 128 && !has_control_chars(v.as_string())) {
			e.type = 's';
			e.value = v.as_string();
		} else {
			continue; // nested, null or hostile: not a flat scalar
		}
		o.extra.push_back(e);
	}
}

} // namespace

AitumConfig parse_aitum(const std::string &text)
{
	AitumConfig c;
	if (text.size() > kMaxAitumFileBytes) {
		c.status = AitumStatus::TooBig;
		c.message = "Aitum's aitum.json is larger than 4 MB: not read";
		return c;
	}
	json::ParseResult pr = json::parse(text, kMaxAitumFileBytes, 32);
	if (!pr.ok || !pr.value.is_object()) {
		c.status = AitumStatus::Malformed;
		c.message = "Aitum's aitum.json is not a valid JSON object";
		return c;
	}
	const json::Value &root = pr.value;
	const json::Value *outs = root.find("outputs");
	const json::Value *cans = root.find("canvas");
	if (!outs || !outs->is_array() || !cans || !cans->is_array()) {
		c.status = AitumStatus::UnknownShape;
		c.message = std::string("aitum.json does not have the layout of Aitum Stream Suite ") + kAitumInspectedVersion +
			    " (\"outputs\" and \"canvas\" lists): its settings are not used";
		return c;
	}
	for (size_t i = 0; i < cans->size(); ++i) {
		const json::Value &v = cans->at(i);
		if (!v.is_object())
			continue;
		AitumCanvas k;
		k.name = clean(str_of(v, "name"), 128);
		k.width = int_of(v, "width", 0, 0, 16384);
		k.height = int_of(v, "height", 0, 0, 16384);
		if (!k.name.empty())
			c.canvases.push_back(k);
	}
	for (size_t i = 0; i < outs->size(); ++i) {
		const json::Value &v = outs->at(i);
		if (!v.is_object() || str_of(v, "type") != "stream") {
			++c.skipped_outputs;
			continue;
		}
		AitumOutput o;
		o.name = clean(str_of(v, "name"), 128);
		if (o.name.empty()) {
			++c.skipped_outputs;
			c.notes.push_back("an Aitum stream output without a name was skipped");
			continue;
		}
		o.enabled = bool_of(v, "enabled", false);
		const json::Value *encs = v.find("video_encoders");
		const json::Value *enc = (encs && encs->is_array() && encs->size() > 0 && encs->at(0).is_object()) ? &encs->at(0) : nullptr;
		o.canvas = clean(str_of(v, "canvas"), 128);
		if (o.canvas.empty() && enc)
			o.canvas = clean(str_of(*enc, "canvas"), 128);
		if (enc) {
			o.video_encoder = clean(str_of(*enc, "video_encoder"), 64);
			const json::Value *vs = enc->find("video_encoder_settings");
			if (vs)
				read_settings(*vs, o);
			o.scale = bool_of(*enc, "scale", false);
			o.width = int_of(*enc, "width", 0, 0, 16384);
			o.height = int_of(*enc, "height", 0, 0, 16384);
			o.scale_type = int_of(*enc, "scale_type", -1, 0, 5);
			o.shares_other_encoder = !str_of(*enc, "output_video_encoder").empty();
		}
		o.audio_encoder = clean(str_of(v, "audio_encoder"), 64);
		const json::Value *as = v.find("audio_encoder_settings");
		if (as && as->is_object())
			o.audio_bitrate_kbps = int_of(*as, "bitrate", 0, 0, 10000);
		o.audio_tracks_mask = int_of(v, "audio_tracks", 0, 0, 63);
		// keys seen only as strings in the plugin binary (unverified placement): look on both objects
		const json::Value *sv = find_in(v, enc, "stream_server");
		if (sv && sv->is_string() && !sv->as_string().empty()) {
			o.server = sv->as_string();
			o.from_unverified_keys = true;
		}
		const json::Value *kv = find_in(v, enc, "stream_key");
		if (kv && kv->is_string() && !kv->as_string().empty()) {
			o.key = kv->as_string();
			o.from_unverified_keys = true;
		}
		const json::Value *cd = find_in(v, enc, "custom_delay");
		const json::Value *ds = find_in(v, enc, "delay_sec");
		const json::Value *dp = find_in(v, enc, "delay_preserve");
		if (cd || ds || dp) {
			o.from_unverified_keys = true;
			o.custom_delay = cd && cd->is_bool() && cd->as_bool();
			if (ds && ds->is_number() && ds->as_number() >= 0 && ds->as_number() <= 3600 && std::floor(ds->as_number()) == ds->as_number())
				o.delay_sec = static_cast<int>(ds->as_number());
			o.delay_preserve = dp && dp->is_bool() && dp->as_bool();
		}
		c.outputs.push_back(o);
	}
	c.status = AitumStatus::Ok;
	return c;
}

void wipe(AitumConfig &c)
{
	for (size_t i = 0; i < c.outputs.size(); ++i) {
		secure_zero(c.outputs[i].key);
		secure_zero(c.outputs[i].server);
	}
}

const AitumOutput *find_aitum_output(const AitumConfig &c, const std::string &name)
{
	for (size_t i = 0; i < c.outputs.size(); ++i)
		if (c.outputs[i].name == name)
			return &c.outputs[i];
	return nullptr;
}

bool apply_aitum(const Destination &base, const AitumOutput &o, Destination &out, std::string &key_out,
		 std::vector<std::string> &notes)
{
	out = base;
	key_out.clear();
	out.encoder = "own"; // an Aitum output always has an encoder of its own
	out.canvas = o.canvas;
	if (!o.video_encoder.empty() && is_valid_token(o.video_encoder, 64)) {
		out.video_encoder = o.video_encoder;
	} else {
		out.video_encoder = "obs_x264";
		notes.push_back("the Aitum output names no video encoder: using obs_x264");
	}
	if (o.bitrate_kbps >= 300 && o.bitrate_kbps <= 51000) {
		out.bitrate_kbps = o.bitrate_kbps;
	} else {
		out.bitrate_kbps = 4500;
		notes.push_back("the Aitum output has no usable bitrate: using 4500 kbps");
	}
	if (o.keyint_sec >= 0 && o.keyint_sec <= 10)
		out.keyint_sec = o.keyint_sec;
	else if (o.keyint_sec > 10)
		notes.push_back("keyframe interval above 10 s not mapped: kept ours");
	if (out.video_encoder == "obs_x264") {
		if (!o.preset.empty() && is_valid_token(o.preset, 32))
			out.preset = o.preset;
	} else {
		out.preset = (!o.preset.empty() && is_valid_token(o.preset, 32)) ? o.preset : "";
	}
	out.profile = (!o.profile.empty() && is_valid_token(o.profile, 32)) ? o.profile : "";
	out.rate_control = (!o.rate_control.empty() && is_valid_token(o.rate_control, 32)) ? o.rate_control : out.rate_control;
	if (out.video_encoder != "obs_x264" && o.rate_control.empty())
		out.rate_control = "";
	out.venc_extra = o.extra;
	out.out_width = 0;
	out.out_height = 0;
	if (o.scale && o.width >= 2 && o.height >= 2) {
		out.out_width = o.width & ~1;
		out.out_height = o.height & ~1;
	}
	if (o.scale_type >= 0)
		out.scale_type = o.scale_type;
	if (o.audio_bitrate_kbps >= 64 && o.audio_bitrate_kbps <= 320)
		out.audio_bitrate_kbps = o.audio_bitrate_kbps;
	if (o.audio_tracks_mask != 0) {
		int first = 0, count = 0;
		for (int b = 0; b < kMaxAudioTracks; ++b)
			if (o.audio_tracks_mask & (1 << b)) {
				if (!first)
					first = b + 1;
				++count;
			}
		out.audio_track = first;
		if (count > 1)
			notes.push_back("Aitum lists several audio tracks; a stream carries one, using the lowest");
	}
	out.delay_sec = 0;
	out.delay_preserve = false;
	if (o.custom_delay && o.delay_sec > 0) {
		out.delay_sec = o.delay_sec;
		out.delay_preserve = o.delay_preserve;
	}
	if (!o.server.empty()) {
		Destination probe = base;
		probe.url = o.server;
		bool ok = true;
		std::vector<ValidationIssue> iss = validate_destination(probe, false);
		for (size_t i = 0; i < iss.size(); ++i)
			if (iss[i].field == "url")
				ok = false;
		if (ok)
			out.url = o.server;
		else
			notes.push_back("the Aitum server address is not a valid rtmp(s) URL: kept ours");
	} else {
		notes.push_back("this Aitum output has no server of its own: URL and key stay as you set them here");
	}
	if (!o.key.empty())
		key_out = o.key;
	if (o.from_unverified_keys)
		notes.push_back("server, key or delay came from settings names that were not present in the inspected Aitum files: unverified");
	if (o.shares_other_encoder)
		notes.push_back("Aitum shares another output's encoder; here this destination gets its own copy of the settings");
	if (out.canvas.empty())
		notes.push_back("the Aitum output names no canvas: streaming the main canvas with its own encoder");
	std::vector<ValidationIssue> iss = validate_destination(out, false);
	if (!iss.empty()) {
		notes.push_back("the mapped settings do not validate (" + iss[0].field + "): " + iss[0].message);
		secure_zero(key_out);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------

std::string normalize_target_url(const std::string &url)
{
	std::string u = url;
	size_t q = u.find_first_of("?#");
	if (q != std::string::npos)
		u.erase(q);
	size_t sch = u.find("://");
	if (sch == std::string::npos)
		return std::string();
	std::string scheme, rest = u.substr(sch + 3);
	for (size_t i = 0; i < sch; ++i)
		scheme.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(u[i]))));
	size_t slash = rest.find('/');
	std::string host = rest.substr(0, slash), path = slash == std::string::npos ? std::string() : rest.substr(slash);
	for (size_t i = 0; i < host.size(); ++i)
		host[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(host[i])));
	if ((scheme == "rtmp" && host.size() > 5 && host.compare(host.size() - 5, 5, ":1935") == 0) ||
	    (scheme == "rtmps" && host.size() > 4 && host.compare(host.size() - 4, 4, ":443") == 0))
		host.erase(host.rfind(':'));
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	return scheme + "://" + host + path;
}

bool same_target(const TargetRef &a, const TargetRef &b)
{
	if (a.key.empty() || b.key.empty() || a.key != b.key)
		return false;
	std::string ua = normalize_target_url(a.url), ub = normalize_target_url(b.url);
	return !ua.empty() && ua == ub;
}

std::vector<std::string> find_double_streaming(const std::vector<TargetRef> &t)
{
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (size_t i = 0; i < t.size(); ++i)
		for (size_t j = i + 1; j < t.size(); ++j) {
			if (t[i].where == t[j].where || !same_target(t[i], t[j]))
				continue;
			std::string m = t[i].where + " and " + t[j].where + " send to the same server with the same key: the stream would go out twice";
			if (seen.insert(m).second)
				out.push_back(m);
		}
	return out;
}

} // namespace oms
