#include "random.hpp"

#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cstdio>
#endif

namespace oms {

std::string random_token_hex(size_t bytes)
{
	if (bytes == 0 || bytes > 256)
		return std::string();
	std::vector<unsigned char> buf(bytes);
#ifdef _WIN32
	if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return std::string();
#else
	FILE *f = std::fopen("/dev/urandom", "rb");
	if (!f)
		return std::string();
	size_t got = std::fread(buf.data(), 1, buf.size(), f);
	std::fclose(f);
	if (got != buf.size())
		return std::string();
#endif
	static const char *hex = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < buf.size(); ++i) {
		out.push_back(hex[buf[i] >> 4]);
		out.push_back(hex[buf[i] & 15]);
	}
	return out;
}

} // namespace oms
