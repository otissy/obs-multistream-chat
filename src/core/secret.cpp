#include "secret.hpp"
#include "base64.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace oms {

void secure_zero(std::string &s)
{
	volatile char *p = s.empty() ? nullptr : &s[0];
	for (size_t i = 0; i < s.size(); ++i)
		p[i] = 0;
	s.clear();
}

static const char kTestPrefix[] = "test:v1:";

bool InsecureTestProtector::protect(const std::string &plain, std::string &token) const
{
	std::string x = plain;
	for (size_t i = 0; i < x.size(); ++i)
		x[i] = char(x[i] ^ 0x5A);
	token = std::string(kTestPrefix) + base64_encode(x);
	return true;
}

bool InsecureTestProtector::unprotect(const std::string &token, std::string &plain) const
{
	plain.clear();
	const std::string prefix(kTestPrefix);
	if (token.compare(0, prefix.size(), prefix) != 0)
		return false;
	std::string raw;
	if (!base64_decode(token.substr(prefix.size()), raw))
		return false;
	for (size_t i = 0; i < raw.size(); ++i)
		raw[i] = char(raw[i] ^ 0x5A);
	plain = raw;
	return true;
}

#ifdef _WIN32

namespace {

const char kDpapiPrefix[] = "dpapi:v1:";
// Fixed, app-specific entropy: stops another program that merely calls CryptUnprotectData
// without knowing it from decrypting the blob. Not a secret (it is in the binary).
const BYTE kEntropy[] = {'o', 'b', 's', '-', 'm', 'u', 'l', 't', 'i', 's', 't', 'r', 'e', 'a', 'm', '/', 'k', 'e', 'y', '1'};

class DpapiProtector : public SecretProtector {
public:
	bool protect(const std::string &plain, std::string &token) const override
	{
		DATA_BLOB in;
		in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()));
		in.cbData = static_cast<DWORD>(plain.size());
		DATA_BLOB ent;
		ent.pbData = const_cast<BYTE *>(kEntropy);
		ent.cbData = static_cast<DWORD>(sizeof kEntropy);
		DATA_BLOB out = {0, nullptr};
		if (!CryptProtectData(&in, L"obs-multistream stream key", &ent, nullptr, nullptr,
				      CRYPTPROTECT_UI_FORBIDDEN, &out))
			return false;
		std::string blob(reinterpret_cast<const char *>(out.pbData), out.cbData);
		LocalFree(out.pbData);
		token = std::string(kDpapiPrefix) + base64_encode(blob);
		return true;
	}

	bool unprotect(const std::string &token, std::string &plain) const override
	{
		plain.clear();
		const std::string prefix(kDpapiPrefix);
		if (token.compare(0, prefix.size(), prefix) != 0)
			return false;
		std::string blob;
		if (!base64_decode(token.substr(prefix.size()), blob) || blob.empty())
			return false;
		DATA_BLOB in;
		in.pbData = reinterpret_cast<BYTE *>(&blob[0]);
		in.cbData = static_cast<DWORD>(blob.size());
		DATA_BLOB ent;
		ent.pbData = const_cast<BYTE *>(kEntropy);
		ent.cbData = static_cast<DWORD>(sizeof kEntropy);
		DATA_BLOB out = {0, nullptr};
		if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
			return false;
		plain.assign(reinterpret_cast<const char *>(out.pbData), out.cbData);
		SecureZeroMemory(out.pbData, out.cbData);
		LocalFree(out.pbData);
		return true;
	}

	const char *name() const override { return "dpapi"; }
};

} // namespace

std::unique_ptr<SecretProtector> make_dpapi_protector()
{
	return std::unique_ptr<SecretProtector>(new DpapiProtector());
}

#else

std::unique_ptr<SecretProtector> make_dpapi_protector()
{
	return nullptr;
}

#endif

} // namespace oms
