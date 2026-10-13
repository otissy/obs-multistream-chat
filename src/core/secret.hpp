// Stream keys at rest. The interface lets tests (and non-Windows builds) swap the
// protector; the shipped plugin always uses the DPAPI one.
#pragma once
#include <memory>
#include <string>

namespace oms {

class SecretProtector {
public:
	virtual ~SecretProtector() = default;
	// Turns plaintext into a printable token safe to store in config.json.
	virtual bool protect(const std::string &plain, std::string &token) const = 0;
	// Reverses protect(). Returns false for foreign/corrupt tokens or a different Windows user.
	virtual bool unprotect(const std::string &token, std::string &plain) const = 0;
	virtual const char *name() const = 0;
};

// Windows DPAPI, current-user scope, app-specific entropy. Token format "dpapi:v1:<base64>".
// Returns nullptr on platforms without DPAPI (the plugin refuses to store keys then).
std::unique_ptr<SecretProtector> make_dpapi_protector();

// NOT SECURE. Reversible XOR + base64, prefix "test:v1:". Exists only so the core can be tested
// anywhere. The plugin never constructs it; a test asserts config files written by the
// plugin path never carry this prefix.
class InsecureTestProtector : public SecretProtector {
public:
	bool protect(const std::string &plain, std::string &token) const override;
	bool unprotect(const std::string &token, std::string &plain) const override;
	const char *name() const override { return "insecure-test"; }
};

// Overwrites the string's bytes and empties it (best effort; the compiler may not elide it).
void secure_zero(std::string &s);

} // namespace oms
