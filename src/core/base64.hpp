#pragma once
#include <cstdint>
#include <string>

namespace oms {

inline std::string base64_encode(const std::string &in)
{
	static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;
	while (i + 2 < in.size()) {
		uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) |
			     uint32_t(uint8_t(in[i + 2]));
		out.push_back(tbl[(v >> 18) & 63]);
		out.push_back(tbl[(v >> 12) & 63]);
		out.push_back(tbl[(v >> 6) & 63]);
		out.push_back(tbl[v & 63]);
		i += 3;
	}
	if (i + 1 == in.size()) {
		uint32_t v = uint32_t(uint8_t(in[i])) << 16;
		out.push_back(tbl[(v >> 18) & 63]);
		out.push_back(tbl[(v >> 12) & 63]);
		out.push_back('=');
		out.push_back('=');
	} else if (i + 2 == in.size()) {
		uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8);
		out.push_back(tbl[(v >> 18) & 63]);
		out.push_back(tbl[(v >> 12) & 63]);
		out.push_back(tbl[(v >> 6) & 63]);
		out.push_back('=');
	}
	return out;
}

// Strict: standard alphabet, canonical padding, no whitespace. Returns false on any defect.
inline bool base64_decode(const std::string &in, std::string &out)
{
	out.clear();
	if (in.size() % 4 != 0)
		return false;
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		if (c == '+')
			return 62;
		if (c == '/')
			return 63;
		return -1;
	};
	for (size_t i = 0; i < in.size(); i += 4) {
		int a = val(in[i]), b = val(in[i + 1]);
		bool last = (i + 4 == in.size());
		if (a < 0 || b < 0)
			return false;
		if (in[i + 2] == '=') {
			if (!last || in[i + 3] != '=' || (b & 15) != 0)
				return false;
			out.push_back(char((a << 2) | (b >> 4)));
			break;
		}
		int c = val(in[i + 2]);
		if (c < 0)
			return false;
		if (in[i + 3] == '=') {
			if (!last || (c & 3) != 0)
				return false;
			out.push_back(char((a << 2) | (b >> 4)));
			out.push_back(char(((b & 15) << 4) | (c >> 2)));
			break;
		}
		int d = val(in[i + 3]);
		if (d < 0)
			return false;
		out.push_back(char((a << 2) | (b >> 4)));
		out.push_back(char(((b & 15) << 4) | (c >> 2)));
		out.push_back(char(((c & 3) << 6) | d));
	}
	return true;
}

} // namespace oms
