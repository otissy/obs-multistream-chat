#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace oms {
namespace json {

namespace {

class Parser {
public:
	Parser(const std::string &t, int max_depth) : s_(t), i_(0), max_depth_(max_depth) {}

	bool run(Value &out, std::string &err)
	{
		skip_ws();
		if (!parse_value(out, 0)) {
			err = err_;
			return false;
		}
		skip_ws();
		if (i_ != s_.size()) {
			fail("trailing characters");
			err = err_;
			return false;
		}
		return true;
	}

private:
	const std::string &s_;
	size_t i_;
	int max_depth_;
	std::string err_;

	bool fail(const char *msg)
	{
		if (err_.empty()) {
			char buf[96];
			std::snprintf(buf, sizeof buf, "%s at byte %zu", msg, i_);
			err_ = buf;
		}
		return false;
	}

	void skip_ws()
	{
		while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
			++i_;
	}

	bool literal(const char *word)
	{
		size_t n = 0;
		while (word[n])
			++n;
		if (s_.compare(i_, n, word) != 0)
			return false;
		i_ += n;
		return true;
	}

	bool parse_value(Value &out, int depth)
	{
		if (depth > max_depth_)
			return fail("nesting too deep");
		if (i_ >= s_.size())
			return fail("unexpected end");
		char c = s_[i_];
		if (c == '{')
			return parse_object(out, depth);
		if (c == '[')
			return parse_array(out, depth);
		if (c == '"') {
			std::string str;
			if (!parse_string(str))
				return false;
			out = Value::string(str);
			return true;
		}
		if (c == 't') {
			if (!literal("true"))
				return fail("bad literal");
			out = Value::boolean(true);
			return true;
		}
		if (c == 'f') {
			if (!literal("false"))
				return fail("bad literal");
			out = Value::boolean(false);
			return true;
		}
		if (c == 'n') {
			if (!literal("null"))
				return fail("bad literal");
			out = Value();
			return true;
		}
		if (c == '-' || (c >= '0' && c <= '9'))
			return parse_number(out);
		return fail("unexpected character");
	}

	bool parse_number(Value &out)
	{
		size_t start = i_;
		if (s_[i_] == '-')
			++i_;
		if (i_ >= s_.size())
			return fail("bad number");
		if (s_[i_] == '0') {
			++i_;
		} else if (s_[i_] >= '1' && s_[i_] <= '9') {
			while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
				++i_;
		} else {
			return fail("bad number");
		}
		if (i_ < s_.size() && s_[i_] == '.') {
			++i_;
			size_t d = i_;
			while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
				++i_;
			if (i_ == d)
				return fail("bad number");
		}
		if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
			++i_;
			if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-'))
				++i_;
			size_t d = i_;
			while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9')
				++i_;
			if (i_ == d)
				return fail("bad number");
		}
		std::string lex = s_.substr(start, i_ - start);
		double v = std::strtod(lex.c_str(), nullptr);
		if (!std::isfinite(v))
			return fail("number out of range");
		out = Value::number(v);
		return true;
	}

	static void append_utf8(std::string &o, uint32_t cp)
	{
		if (cp < 0x80) {
			o.push_back(char(cp));
		} else if (cp < 0x800) {
			o.push_back(char(0xC0 | (cp >> 6)));
			o.push_back(char(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			o.push_back(char(0xE0 | (cp >> 12)));
			o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			o.push_back(char(0x80 | (cp & 0x3F)));
		} else {
			o.push_back(char(0xF0 | (cp >> 18)));
			o.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
			o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
			o.push_back(char(0x80 | (cp & 0x3F)));
		}
	}

	bool hex4(uint32_t &v)
	{
		if (i_ + 4 > s_.size())
			return false;
		v = 0;
		for (int k = 0; k < 4; ++k) {
			char c = s_[i_ + k];
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= uint32_t(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= uint32_t(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= uint32_t(c - 'A' + 10);
			else
				return false;
		}
		i_ += 4;
		return true;
	}

	bool parse_string(std::string &out)
	{
		++i_; // opening quote
		out.clear();
		while (true) {
			if (i_ >= s_.size())
				return fail("unterminated string");
			unsigned char c = static_cast<unsigned char>(s_[i_]);
			if (c == '"') {
				++i_;
				return true;
			}
			if (c < 0x20)
				return fail("control character in string");
			if (c != '\\') {
				out.push_back(char(c));
				++i_;
				continue;
			}
			++i_;
			if (i_ >= s_.size())
				return fail("unterminated escape");
			char e = s_[i_++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				uint32_t cp;
				if (!hex4(cp))
					return fail("bad \\u escape");
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					uint32_t lo;
					if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u')
						return fail("lone surrogate");
					i_ += 2;
					if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
						return fail("bad surrogate pair");
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					return fail("lone surrogate");
				}
				append_utf8(out, cp);
				break;
			}
			default:
				return fail("bad escape");
			}
		}
	}

	bool parse_array(Value &out, int depth)
	{
		++i_;
		out = Value::array();
		skip_ws();
		if (i_ < s_.size() && s_[i_] == ']') {
			++i_;
			return true;
		}
		while (true) {
			skip_ws();
			Value item;
			if (!parse_value(item, depth + 1))
				return false;
			out.push(item);
			skip_ws();
			if (i_ >= s_.size())
				return fail("unterminated array");
			if (s_[i_] == ',') {
				++i_;
				continue;
			}
			if (s_[i_] == ']') {
				++i_;
				return true;
			}
			return fail("expected , or ]");
		}
	}

	bool parse_object(Value &out, int depth)
	{
		++i_;
		out = Value::object();
		skip_ws();
		if (i_ < s_.size() && s_[i_] == '}') {
			++i_;
			return true;
		}
		while (true) {
			skip_ws();
			if (i_ >= s_.size() || s_[i_] != '"')
				return fail("expected string key");
			std::string key;
			if (!parse_string(key))
				return false;
			skip_ws();
			if (i_ >= s_.size() || s_[i_] != ':')
				return fail("expected :");
			++i_;
			skip_ws();
			Value val;
			if (!parse_value(val, depth + 1))
				return false;
			out.set(key, val);
			skip_ws();
			if (i_ >= s_.size())
				return fail("unterminated object");
			if (s_[i_] == ',') {
				++i_;
				continue;
			}
			if (s_[i_] == '}') {
				++i_;
				return true;
			}
			return fail("expected , or }");
		}
	}
};

void write_value(const Value &v, bool pretty, int level, std::string &out);

void indent(bool pretty, int level, std::string &out)
{
	if (!pretty)
		return;
	out.push_back('\n');
	out.append(size_t(level) * 2, ' ');
}

void write_number(double n, std::string &out)
{
	if (!std::isfinite(n)) {
		out += "null";
		return;
	}
	char buf[40];
	if (n == std::floor(n) && std::fabs(n) < 1e15)
		std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
	else
		std::snprintf(buf, sizeof buf, "%.17g", n);
	out += buf;
}

void write_value(const Value &v, bool pretty, int level, std::string &out)
{
	switch (v.type()) {
	case Type::Null: out += "null"; break;
	case Type::Bool: out += v.as_bool() ? "true" : "false"; break;
	case Type::Number: write_number(v.as_number(), out); break;
	case Type::String:
		out.push_back('"');
		out += escape_string(v.as_string());
		out.push_back('"');
		break;
	case Type::Array:
		out.push_back('[');
		for (size_t i = 0; i < v.size(); ++i) {
			if (i)
				out.push_back(',');
			indent(pretty, level + 1, out);
			write_value(v.at(i), pretty, level + 1, out);
		}
		if (v.size())
			indent(pretty, level, out);
		out.push_back(']');
		break;
	case Type::Object:
		out.push_back('{');
		for (size_t i = 0; i < v.size(); ++i) {
			if (i)
				out.push_back(',');
			indent(pretty, level + 1, out);
			out.push_back('"');
			out += escape_string(v.key_at(i));
			out += pretty ? "\": " : "\":";
			write_value(v.value_at(i), pretty, level + 1, out);
		}
		if (v.size())
			indent(pretty, level, out);
		out.push_back('}');
		break;
	}
}

} // namespace

ParseResult parse(const std::string &text, size_t max_bytes, int max_depth)
{
	ParseResult r;
	if (text.size() > max_bytes) {
		r.error = "input too large";
		return r;
	}
	// A UTF-8 BOM is tolerated (editors add it); anything else before the value is an error.
	size_t off = 0;
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
	    static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
		off = 3;
	std::string body = off ? text.substr(off) : text;
	Parser p(body, max_depth);
	r.ok = p.run(r.value, r.error);
	if (!r.ok)
		r.value = Value();
	return r;
}

std::string escape_string(const std::string &s)
{
	std::string o;
	o.reserve(s.size() + 8);
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\b': o += "\\b"; break;
		case '\f': o += "\\f"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20 || c == 0x7F) {
				char buf[8];
				std::snprintf(buf, sizeof buf, "\\u%04x", c);
				o += buf;
			} else {
				o.push_back(char(c));
			}
		}
	}
	return o;
}

std::string dump(const Value &v, bool pretty)
{
	std::string out;
	write_value(v, pretty, 0, out);
	return out;
}

} // namespace json
} // namespace oms
