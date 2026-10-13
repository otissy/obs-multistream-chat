// L2: json, base64, ring buffer, redaction, secrets, destination validation.
#include "oms_test.hpp"

#include "../../../src/core/base64.hpp"
#include "../../../src/core/destination.hpp"
#include "../../../src/core/json.hpp"
#include "../../../src/core/redact.hpp"
#include "../../../src/core/ring.hpp"
#include "../../../src/core/secret.hpp"

using namespace oms;

// ---- json ----------------------------------------------------------------------------------

TEST(L2, json_parses_scalars_and_containers)
{
	json::ParseResult r = json::parse("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-2.5e1}}");
	CHECK(r.ok);
	CHECK_EQ(r.value.find("a")->as_int(), 1);
	CHECK_EQ(r.value.find("b")->size(), size_t(3));
	CHECK(r.value.find("b")->at(0).as_bool());
	CHECK(r.value.find("b")->at(1).is_null());
	CHECK_EQ(r.value.find("b")->at(2).as_string(), std::string("x"));
	CHECK_NEAR(r.value.find("c")->find("d")->as_number(), -25.0, 1e-9);
}

TEST(L2, json_rejects_trailing_comma_comments_and_garbage)
{
	CHECK_FALSE(json::parse("[1,2,]").ok);
	CHECK_FALSE(json::parse("{\"a\":1,}").ok);
	CHECK_FALSE(json::parse("{\"a\":1} // c").ok);
	CHECK_FALSE(json::parse("{\"a\":1} x").ok);
	CHECK_FALSE(json::parse("").ok);
	CHECK_FALSE(json::parse("{").ok);
	CHECK_FALSE(json::parse("{'a':1}").ok);
	CHECK_FALSE(json::parse("[01]").ok);
	CHECK_FALSE(json::parse("[1.]").ok);
	CHECK_FALSE(json::parse("[NaN]").ok);
	CHECK_FALSE(json::parse("[1e999]").ok);
	CHECK_FALSE(json::parse("\"a\nb\"").ok); // raw control char inside a string
}

TEST(L2, json_depth_and_size_limits)
{
	std::string deep(100, '[');
	deep += std::string(100, ']');
	CHECK_FALSE(json::parse(deep).ok);
	std::string ok_deep(20, '[');
	ok_deep += std::string(20, ']');
	CHECK(json::parse(ok_deep).ok);
	std::string big = "[\"" + std::string(2000, 'a') + "\"]";
	CHECK_FALSE(json::parse(big, 1000).ok);
}

TEST(L2, json_unicode_escapes_and_surrogates)
{
	json::ParseResult r = json::parse("\"\\u00e9\\ud83d\\ude00\"");
	CHECK(r.ok);
	CHECK_EQ(r.value.as_string(), std::string("\xC3\xA9\xF0\x9F\x98\x80"));
	CHECK_FALSE(json::parse("\"\\ud83d\"").ok);  // lone high surrogate
	CHECK_FALSE(json::parse("\"\\ude00\"").ok);  // lone low surrogate
	CHECK_FALSE(json::parse("\"\\q\"").ok);
}

TEST(L2, json_dump_roundtrip_and_escaping)
{
	json::Value o = json::Value::object();
	o.set("s", json::Value::string("quote\" back\\ nl\n tab\t <\x01>"));
	o.set("n", json::Value::number(42));
	o.set("f", json::Value::number(0.5));
	o.set("n", json::Value::number(43)); // duplicate key replaces
	std::string text = json::dump(o);
	json::ParseResult r = json::parse(text);
	CHECK(r.ok);
	CHECK_EQ(r.value.find("s")->as_string(), std::string("quote\" back\\ nl\n tab\t <\x01>"));
	CHECK_EQ(r.value.find("n")->as_int(), 43);
	CHECK_EQ(r.value.size(), size_t(3));
	CHECK(text.find("\n") == std::string::npos);
}

TEST(L2, json_bom_is_tolerated)
{
	CHECK(json::parse("\xEF\xBB\xBF{\"a\":1}").ok);
}

TEST(L2, json_duplicate_keys_last_wins)
{
	json::ParseResult r = json::parse("{\"a\":1,\"a\":2}");
	CHECK(r.ok);
	CHECK_EQ(r.value.find("a")->as_int(), 2);
	CHECK_EQ(r.value.size(), size_t(1));
}

// ---- base64 / ring --------------------------------------------------------------------------

TEST(L2, base64_roundtrip_all_lengths)
{
	for (size_t n = 0; n < 40; ++n) {
		std::string s;
		for (size_t i = 0; i < n; ++i)
			s.push_back(char((i * 37 + n) & 0xFF));
		std::string enc = base64_encode(s), dec;
		CHECK(base64_decode(enc, dec));
		CHECK_EQ(dec, s);
	}
	CHECK_EQ(base64_encode("Man"), std::string("TWFu"));
	CHECK_EQ(base64_encode("Ma"), std::string("TWE="));
	CHECK_EQ(base64_encode("M"), std::string("TQ=="));
}

TEST(L2, base64_rejects_malformed)
{
	std::string out;
	CHECK_FALSE(base64_decode("TWF", out));
	CHECK_FALSE(base64_decode("TW=u", out));
	CHECK_FALSE(base64_decode("TWFu TWFu", out));
	CHECK_FALSE(base64_decode("TQ==TQ==", out)); // padding in the middle
	CHECK_FALSE(base64_decode("TR==", out));     // non-canonical trailing bits
	CHECK_FALSE(base64_decode("T!==", out));
}

TEST(L2, ring_overwrites_oldest)
{
	RingBuffer<int> r(3);
	CHECK(r.empty());
	r.push(1);
	r.push(2);
	CHECK_EQ(r.size(), size_t(2));
	r.push(3);
	r.push(4);
	r.push(5);
	CHECK_EQ(r.size(), size_t(3));
	CHECK_EQ(r.oldest(), 3);
	CHECK_EQ(r.at(1), 4);
	CHECK_EQ(r.newest(), 5);
	r.clear();
	CHECK(r.empty());
	RingBuffer<int> zero(0); // capacity is clamped to 1, never UB
	zero.push(9);
	zero.push(10);
	CHECK_EQ(zero.newest(), 10);
}

// ---- redaction -----------------------------------------------------------------------------

TEST(L2, redact_known_secret_everywhere)
{
	std::vector<std::string> secrets;
	secrets.push_back("live_000000_test");
	std::string out = redact("connect rtmp://x/app/live_000000_test failed; key=live_000000_test", secrets);
	CHECK(out.find("live_000000_test") == std::string::npos);
	CHECK(out.find("[redacted]") != std::string::npos);
}

TEST(L2, redact_pattern_without_known_secret)
{
	std::vector<std::string> none;
	CHECK(redact("url /app/live_123456_abcDEF123 ok", none).find("abcDEF123") == std::string::npos);
	CHECK(redact("key abcd-efgh-ijkl-mnop-qrst", none).find("abcd") == std::string::npos);
	CHECK(redact("key abcd-efgh-ijkl-mnop", none).find("ijkl") == std::string::npos);
	CHECK_EQ(redact("plain text, date 2026-10-13", none), std::string("plain text, date 2026-10-13"));
}

TEST(L2, redact_ignores_short_secrets)
{
	std::vector<std::string> s;
	s.push_back("ab"); // would shred ordinary text
	CHECK_EQ(redact("abab", s), std::string("abab"));
	s.push_back("");
	CHECK_EQ(redact("hello", s), std::string("hello"));
}

TEST(L2, sanitize_url_drops_userinfo_query_and_key_segments)
{
	CHECK_EQ(sanitize_url("rtmp://live.twitch.tv/app"), std::string("rtmp://live.twitch.tv/app"));
	CHECK_EQ(sanitize_url("rtmp://u:pw@host:1935/live/live_000000_test?x=1#f"), std::string("rtmp://host:1935/live/..."));
	CHECK_EQ(sanitize_url("not a url"), std::string("(invalid url)"));
	CHECK(sanitize_url("rtmps://h/app/secretkey").find("secretkey") == std::string::npos);
}

// ---- secrets -------------------------------------------------------------------------------

TEST(L2, test_protector_roundtrip_and_rejects_foreign)
{
	InsecureTestProtector p;
	std::string tok, plain;
	CHECK(p.protect("live_000000_test", tok));
	CHECK(tok.find("live_000000_test") == std::string::npos);
	CHECK(p.unprotect(tok, plain));
	CHECK_EQ(plain, std::string("live_000000_test"));
	CHECK_FALSE(p.unprotect("dpapi:v1:AAAA", plain));
	CHECK_FALSE(p.unprotect("", plain));
	CHECK_FALSE(p.unprotect("test:v1:!!!", plain));
}

TEST(L2, secure_zero_clears)
{
	std::string s = "live_000000_test";
	secure_zero(s);
	CHECK(s.empty());
}

#ifdef _WIN32
TEST(L2, dpapi_roundtrip_tamper_and_foreign)
{
	std::unique_ptr<SecretProtector> p = make_dpapi_protector();
	CHECK(p != nullptr);
	if (!p)
		return;
	std::string tok, plain;
	CHECK(p->protect("live_000000_test", tok));
	CHECK(tok.compare(0, 9, "dpapi:v1:") == 0);
	CHECK(tok.find("live_000000_test") == std::string::npos);
	CHECK(p->unprotect(tok, plain));
	CHECK_EQ(plain, std::string("live_000000_test"));
	// flip a byte in the middle of the blob: DPAPI must refuse it
	std::string bad = tok;
	size_t mid = 9 + (bad.size() - 9) / 2;
	bad[mid] = (bad[mid] == 'A') ? 'B' : 'A';
	CHECK_FALSE(p->unprotect(bad, plain));
	CHECK(plain.empty());
	CHECK_FALSE(p->unprotect("test:v1:xx", plain));
}
#endif

// ---- destination ---------------------------------------------------------------------------

static Destination good_dest()
{
	Destination d;
	d.id = "d1";
	d.name = "Twitch";
	d.platform = "twitch";
	d.url = "rtmp://live.twitch.tv/app";
	d.key = "live_000000_test";
	d.handle = "otiyyy";
	return d;
}

TEST(L2, destination_accepts_good)
{
	CHECK(validate_destination(good_dest()).empty());
}

TEST(L2, destination_rejects_bad_fields)
{
	Destination d = good_dest();
	d.url = "http://live.twitch.tv/app";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.url = "rtmp://user:pw@host/app";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.url = "rtmp://host/app with space";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.url = "rtmp:///app";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.name = "";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.handle = std::string("a\nb");
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.platform = "nope";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.id = "bad id!";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.key = "has space";
	CHECK(!validate_destination(d).empty());
	d = good_dest();
	d.key.clear();
	CHECK(!validate_destination(d, true).empty());
	CHECK(validate_destination(d, false).empty());
	d.key_token = "dpapi:v1:xxxx";
	CHECK(validate_destination(d, true).empty());
}

TEST(L2, destination_encoder_and_bitrate_rules)
{
	Destination d = good_dest();
	d.encoder = "x264";
	d.bitrate_kbps = 0;
	CHECK(!validate_destination(d).empty());
	d.bitrate_kbps = 2500;
	CHECK(validate_destination(d).empty());
	d.encoder = "nvenc";
	CHECK(!validate_destination(d).empty());
	d.encoder = "shared";
	d.bitrate_kbps = -1;
	CHECK(!validate_destination(d).empty());
}

TEST(L2, destination_presets_complete)
{
	const std::vector<PlatformPreset> &p = platform_presets();
	CHECK(p.size() >= 5);
	CHECK(find_preset("twitch") != nullptr);
	CHECK(find_preset("custom") != nullptr);
	CHECK(find_preset("nope") == nullptr);
	for (size_t i = 0; i < p.size(); ++i) {
		CHECK(std::string(p[i].default_url).empty() || std::string(p[i].default_url).find("rtmp") == 0);
		CHECK(!std::string(p[i].icon).empty());
	}
}

TEST(L2, make_id_is_valid_and_distinct)
{
	CHECK(is_valid_id(make_id(1)));
	CHECK(make_id(1) != make_id(2));
	CHECK(is_valid_id(make_id(0)));
}
