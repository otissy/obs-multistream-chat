// Operating-system random bytes, for the per-session chat token.
#pragma once
#include <string>

namespace oms {

// Lower-case hex of `bytes` random bytes from the OS CSPRNG (BCryptGenRandom on Windows,
// /dev/urandom elsewhere). Returns "" on failure: callers must treat that as "no token".
std::string random_token_hex(size_t bytes);

} // namespace oms
