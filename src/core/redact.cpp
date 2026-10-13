#include "redact.hpp"

#include <cctype>

namespace oms {

const char *const kRedacted = "[redacted]";

namespace {

bool is_alnum(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) != 0;
}
bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

std::string replace_all(const std::string &text, const std::string &needle)
{
	if (needle.size() < 3)
		return text;
	std::string out;
	size_t pos = 0;
	while (true) {
		size_t hit = text.find(needle, pos);
		if (hit == std::string::npos) {
			out.append(text, pos, std::string::npos);
			break;
		}
		out.append(text, pos, hit - pos);
		out += kRedacted;
		pos = hit + needle.size();
	}
	return out;
}

// Twitch: live_<digits>_<alnum...>. Returns match length at i, or 0.
size_t match_twitch(const std::string &s, size_t i)
{
	if (s.compare(i, 5, "live_") != 0)
		return 0;
	size_t j = i + 5;
	size_t d = j;
	while (j < s.size() && is_digit(s[j]))
		++j;
	if (j == d || j >= s.size() || s[j] != '_')
		return 0;
	++j;
	size_t t = j;
	while (j < s.size() && is_alnum(s[j]))
		++j;
	if (j == t)
		return 0;
	return j - i;
}

// YouTube: groups of 4 alnum separated by '-', 4 or 5 groups.
size_t match_youtube(const std::string &s, size_t i)
{
	size_t j = i;
	int groups = 0;
	while (groups < 5) {
		if (j + 4 > s.size())
			break;
		bool ok = true;
		for (size_t k = 0; k < 4; ++k)
			if (!is_alnum(s[j + k]))
				ok = false;
		if (!ok)
			break;
		j += 4;
		++groups;
		if (groups < 5 && j < s.size() && s[j] == '-' && j + 1 < s.size() && is_alnum(s[j + 1]))
			++j;
		else
			break;
	}
	if (groups < 4)
		return 0;
	if (j < s.size() && is_alnum(s[j]))
		return 0; // longer alnum run: not a key group
	return j - i;
}

} // namespace

std::string redact(const std::string &text, const std::vector<std::string> &secrets)
{
	std::string cur = text;
	for (size_t k = 0; k < secrets.size(); ++k)
		cur = replace_all(cur, secrets[k]);

	std::string out;
	size_t i = 0;
	while (i < cur.size()) {
		bool boundary = (i == 0) || !is_alnum(cur[i - 1]);
		size_t n = 0;
		if (boundary) {
			n = match_twitch(cur, i);
			if (!n)
				n = match_youtube(cur, i);
		}
		if (n) {
			out += kRedacted;
			i += n;
		} else {
			out.push_back(cur[i]);
			++i;
		}
	}
	return out;
}

std::string sanitize_url(const std::string &url)
{
	size_t scheme_end = url.find("://");
	if (scheme_end == std::string::npos)
		return "(invalid url)";
	std::string scheme = url.substr(0, scheme_end + 3);
	std::string rest = url.substr(scheme_end + 3);
	size_t cut = rest.find_first_of("?#");
	if (cut != std::string::npos)
		rest = rest.substr(0, cut);
	size_t slash = rest.find('/');
	std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
	std::string path = slash == std::string::npos ? std::string() : rest.substr(slash);
	size_t at = authority.rfind('@');
	if (at != std::string::npos)
		authority = authority.substr(at + 1);
	std::string out = scheme + authority;
	if (!path.empty()) {
		size_t next = path.find('/', 1);
		if (next == std::string::npos) {
			out += path;
		} else {
			out += path.substr(0, next);
			if (next + 1 < path.size())
				out += "/...";
			else
				out += "/";
		}
	}
	return out;
}

std::string describe_key(const std::string &key_or_token)
{
	return key_or_token.empty() ? "(empty)" : "(set)";
}

} // namespace oms
