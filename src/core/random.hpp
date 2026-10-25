// Operating-system random bytes (chat token, overlay access key) and a constant-time compare.
#pragma once
#include <string>

namespace oms {

// Lower-case hex of `bytes` random bytes from the OS CSPRNG (BCryptGenRandom on Windows,
// /dev/urandom elsewhere). Returns "" on failure: callers must treat that as "no token".
std::string random_token_hex(size_t bytes);

// true when a == b. Runs over the longer of the two lengths whatever the contents, so the time
// taken does not reveal how many leading characters matched. An empty `want` never matches.
bool secret_equal(const std::string &want, const std::string &presented);

} // namespace oms
