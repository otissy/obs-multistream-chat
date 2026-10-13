// Nothing that may contain a stream key goes to a log, a dialog or the overlay
// without passing through here.
#pragma once
#include <string>
#include <vector>

namespace oms {

extern const char *const kRedacted; // "[redacted]"

// Replaces every occurrence of each known secret (length >= 3) and anything that looks like a
// stream key (Twitch "live_<digits>_<token>", YouTube "xxxx-xxxx-xxxx-xxxx[-xxxx]") with
// "[redacted]". Matching is case-sensitive for secrets, as keys are.
std::string redact(const std::string &text, const std::vector<std::string> &secrets);

// "rtmp://user:pw@host:1935/app/KEY?x=1#f" -> "rtmp://host:1935/app/..." : drops userinfo, query
// and fragment, keeps only the first path segment (users paste keys after it).
std::string sanitize_url(const std::string &url);

// For display where "is a key set" is the only fact needed: "(set)" or "(empty)".
std::string describe_key(const std::string &key_or_token);

} // namespace oms
