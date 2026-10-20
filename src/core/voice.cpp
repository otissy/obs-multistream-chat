#include "voice.hpp"

#include "json.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace oms {
namespace voice {

namespace {

uint32_t le32(const std::string &b, size_t at)
{
	return static_cast<uint32_t>(static_cast<unsigned char>(b[at])) |
	       (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8) |
	       (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 2])) << 16) |
	       (static_cast<uint32_t>(static_cast<unsigned char>(b[at + 3])) << 24);
}

bool valid_id(const std::string &s)
{
	if (s.empty() || s.size() > 96)
		return false;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
		if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-')))
			return false;
	}
	return true;
}

// relative, no "..", no root or drive: the plugin only checks that it exists, but never outside the pack
bool safe_rel(const std::string &p)
{
	if (p.empty() || p.size() > 200 || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos)
		return false;
	size_t start = 0;
	for (size_t i = 0; i <= p.size(); ++i) {
		if (i == p.size() || p[i] == '/' || p[i] == '\\') {
			if (p.compare(start, i - start, "..") == 0 && i - start == 2)
				return false;
			start = i + 1;
		}
	}
	return true;
}

std::string str(const json::Value &o, const char *key)
{
	const json::Value *v = o.find(key);
	return v ? v->as_string() : std::string();
}

} // namespace

bool FrameDecoder::push(const char *data, size_t n, std::vector<Frame> &out)
{
	if (failed_)
		return false;
	buf_.append(data, n);
	size_t pos = 0;
	while (buf_.size() - pos >= kHeader) {
		if (std::memcmp(buf_.data() + pos, "OMSV", 4) != 0) {
			failed_ = true;
			break;
		}
		uint32_t len = le32(buf_, pos + 16);
		if (len > kMaxPayload) {
			failed_ = true;
			break;
		}
		if (buf_.size() - pos < kHeader + len)
			break;
		Frame f;
		f.type = static_cast<FrameType>(static_cast<unsigned char>(buf_[pos + 4]));
		f.id = le32(buf_, pos + 8);
		f.sample_rate = le32(buf_, pos + 12);
		f.payload.assign(buf_, pos + kHeader, len);
		out.push_back(std::move(f));
		pos += kHeader + len;
	}
	buf_.erase(0, pos);
	if (failed_)
		buf_.clear();
	return !failed_;
}

bool parse_catalogue(const std::string &text, Catalogue &out, std::string &err)
{
	out = Catalogue();
	json::ParseResult r = json::parse(text, 4u << 20, 16);
	if (!r.ok) {
		err = "catalogue is not valid JSON: " + r.error;
		return false;
	}
	const json::Value &root = r.value;
	const json::Value *schema = root.find("schema");
	const json::Value *packs = root.find("packs");
	const json::Value *voices = root.find("voices");
	if (!schema || schema->as_int() != 1 || !packs || !packs->is_array() || !voices || !voices->is_array()) {
		err = "catalogue: schema must be 1 with packs and voices";
		return false;
	}
	std::set<std::string> pack_ids;
	for (size_t i = 0; i < packs->size(); ++i) {
		const json::Value &p = packs->at(i);
		Pack pk;
		pk.id = str(p, "id");
		pk.engine = str(p, "engine");
		if (!valid_id(pk.id) || !pack_ids.insert(pk.id).second) {
			err = "catalogue: bad or duplicate pack id";
			return false;
		}
		if (pk.engine != "kokoro" && pk.engine != "vits") {
			err = "catalogue: pack " + pk.id + " has an unknown engine";
			return false;
		}
		const json::Value *files = p.find("files");
		if (!files || !files->is_object()) {
			err = "catalogue: pack " + pk.id + " has no files";
			return false;
		}
		std::vector<const char *> keys = {"model", "tokens", "dataDir"};
		if (pk.engine == "kokoro")
			keys.push_back("voices");
		for (const char *k : keys) {
			std::string f = str(*files, k);
			if (!safe_rel(f)) {
				err = "catalogue: pack " + pk.id + " file " + k + " must stay inside the pack";
				return false;
			}
			pk.required.push_back(f);
		}
		out.packs.push_back(pk);
	}
	std::set<std::string> voice_ids;
	for (size_t i = 0; i < voices->size(); ++i) {
		const json::Value &v = voices->at(i);
		Voice vc;
		vc.id = str(v, "id");
		vc.name = str(v, "name");
		vc.locale = str(v, "locale");
		vc.language = str(v, "language");
		vc.accent = str(v, "accent");
		vc.gender = str(v, "gender");
		vc.pack = str(v, "pack");
		const json::Value *sid = v.find("sid");
		if (!valid_id(vc.id) || !voice_ids.insert(vc.id).second) {
			err = "catalogue: bad or duplicate voice id";
			return false;
		}
		if (!pack_ids.count(vc.pack) || vc.name.empty() || vc.language.empty() || !sid || !sid->is_number() ||
		    sid->as_number() < 0 || sid->as_number() != static_cast<double>(sid->as_int())) {
			err = "catalogue: voice " + vc.id + " is incomplete";
			return false;
		}
		vc.sid = static_cast<int>(sid->as_int());
		out.voices.push_back(vc);
	}
	return true;
}

std::string display_name(const Voice &v)
{
	std::string s = v.language;
	if (!v.accent.empty())
		s += " (" + v.accent + ")";
	s += ": " + v.name;
	if (!v.gender.empty())
		s += ", " + v.gender;
	return s;
}

std::vector<Voice> installed_voices(const Catalogue &c, const std::function<bool(const Pack &)> &installed)
{
	std::set<std::string> ok;
	for (const Pack &p : c.packs)
		if (installed(p))
			ok.insert(p.id);
	std::vector<Voice> out;
	for (const Voice &v : c.voices)
		if (ok.count(v.pack))
			out.push_back(v);
	std::stable_sort(out.begin(), out.end(), [](const Voice &a, const Voice &b) {
		if (a.language != b.language)
			return a.language < b.language;
		if (a.accent != b.accent)
			return a.accent < b.accent;
		return a.name < b.name;
	});
	return out;
}

std::string say_request(uint32_t id, const std::string &voice_id, const std::string &text, double speed)
{
	json::Value o = json::Value::object();
	o.set("op", json::Value::string("say"));
	o.set("id", json::Value::number(static_cast<double>(id)));
	o.set("voice", json::Value::string(voice_id));
	o.set("text", json::Value::string(text));
	o.set("speed", json::Value::number(speed));
	return json::dump(o) + "\n";
}

std::string flush_request()
{
	return "{\"op\":\"flush\"}\n";
}

} // namespace voice
} // namespace oms
