// L2: config load/save, migration, malformed files, key sealing. Uses the insecure test protector
// everywhere except the one DPAPI test (Windows only), so it runs on any machine.
#include "oms_test.hpp"

#include "../../../src/core/clock.hpp"
#include "../../../src/core/config.hpp"
#include "../../../src/core/json.hpp"
#include "../../../src/core/state_feed.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace oms;
namespace fs = std::filesystem;

static const std::string kFakeKey = "live_000000_test";

// A fresh scratch directory per call, removed by the test through ScratchDir's destructor.
struct ScratchDir {
	fs::path dir;
	ScratchDir()
	{
		static int counter = 0;
		long long stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		dir = fs::temp_directory_path() / ("oms-test-" + std::to_string(stamp) + "-" + std::to_string(++counter));
		fs::create_directories(dir);
	}
	~ScratchDir()
	{
		std::error_code ec;
		fs::remove_all(dir, ec);
	}
	std::string file(const std::string &name) const { return (dir / name).u8string(); }
};

static std::string slurp(const std::string &path)
{
	std::string s;
	read_file(path, s);
	return s;
}

static Destination dest(const std::string &id, const std::string &name)
{
	Destination d;
	d.id = id;
	d.name = name;
	d.platform = "twitch";
	d.url = "rtmp://live.twitch.tv/app";
	d.handle = "otiyyy";
	d.key = kFakeKey;
	return d;
}

TEST(L2, config_roundtrip_with_sealed_keys)
{
	InsecureTestProtector p;
	Config c;
	c.server.port = 0;
	c.server.last_port = 51734;
	c.destinations.push_back(dest("d1", "Twitch"));
	Destination y = dest("d2", "YouTube");
	y.platform = "youtube";
	y.url = "rtmp://a.rtmp.youtube.com/live2";
	y.encoder = "own";
	y.bitrate_kbps = 3500;
	y.enabled = false;
	y.auto_start = false;
	c.destinations.push_back(y);
	std::string err;
	CHECK(seal_keys(c, p, err));
	CHECK(c.destinations[0].key.empty()); // plaintext wiped
	std::string text;
	CHECK(serialize_config(c, text, err));
	CHECK(text.find(kFakeKey) == std::string::npos); // never on disk
	LoadResult r = parse_config(text, p);
	CHECK(r.status == LoadStatus::Ok);
	CHECK_EQ(r.config.destinations.size(), size_t(2));
	CHECK_EQ(r.config.server.last_port, 51734);
	CHECK_EQ(r.config.destinations[1].encoder, std::string("own"));
	CHECK_EQ(r.config.destinations[1].bitrate_kbps, 3500);
	CHECK_FALSE(r.config.destinations[1].enabled);
	CHECK_FALSE(r.config.destinations[1].auto_start);
	std::string plain;
	CHECK(unlock_key(r.config.destinations[0], p, plain));
	CHECK_EQ(plain, kFakeKey);
	CHECK(r.warnings.empty());
}

TEST(L2, config_refuses_to_serialize_plaintext_key)
{
	Config c;
	c.destinations.push_back(dest("d1", "Twitch")); // key not sealed
	std::string text, err;
	CHECK_FALSE(serialize_config(c, text, err));
	CHECK(err.find(kFakeKey) == std::string::npos); // the error text never carries the key either
	CHECK(text.empty());
}

TEST(L2, config_migrates_schema1_and_seals_plaintext)
{
	InsecureTestProtector p;
	std::string v1 = "{\"schema\":1,\"targets\":[{\"title\":\"Main\",\"platform\":\"twitch\","
			 "\"server\":\"rtmp://live.twitch.tv/app\",\"key\":\"" + kFakeKey +
			 "\",\"shown_name\":\"otiyyy\",\"reuse_encoder\":false,\"enabled\":true}]}";
	LoadResult r = parse_config(v1, p);
	CHECK(r.status == LoadStatus::Migrated);
	CHECK(r.needs_save);
	CHECK_FALSE(r.read_only);
	CHECK_EQ(r.config.schema, kConfigSchema);
	CHECK_EQ(r.config.destinations.size(), size_t(1));
	const Destination &d = r.config.destinations[0];
	CHECK_EQ(d.name, std::string("Main"));
	CHECK_EQ(d.handle, std::string("otiyyy"));
	CHECK_EQ(d.encoder, std::string("own"));
	CHECK_EQ(d.bitrate_kbps, 2500);
	CHECK(d.key.empty());
	CHECK(!d.key_token.empty());
	CHECK(d.key_token.find(kFakeKey) == std::string::npos);
	std::string out, err;
	CHECK(serialize_config(r.config, out, err));
	CHECK(out.find(kFakeKey) == std::string::npos);
	CHECK(r.message.find(kFakeKey) == std::string::npos);
}

TEST(L2, config_v2_plaintext_key_is_sealed_on_load)
{
	InsecureTestProtector p;
	std::string text = "{\"schema\":4,\"destinations\":[{\"id\":\"d1\",\"name\":\"T\",\"platform\":\"twitch\","
			   "\"url\":\"rtmp://live.twitch.tv/app\",\"key\":\"" + kFakeKey + "\"}]}";
	LoadResult r = parse_config(text, p);
	CHECK(r.status == LoadStatus::Ok);
	CHECK(r.needs_save);
	CHECK(r.config.destinations[0].key.empty());
	std::string plain;
	CHECK(unlock_key(r.config.destinations[0], p, plain));
	CHECK_EQ(plain, kFakeKey);
}

TEST(L2, config_malformed_inputs_never_crash)
{
	InsecureTestProtector p;
	const char *bad[] = {"", "{", "[]", "null", "42", "{\"schema\":\"two\"}", "{\"schema\":0}",
			     "{\"schema\":-5}", "\xff\xfe garbage", "{\"schema\":4,\"destinations\":{}}"};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		LoadResult r = parse_config(bad[i], p);
		bool ok_or_defaulted = r.status == LoadStatus::Malformed || r.status == LoadStatus::Ok;
		CHECK(ok_or_defaulted);
		CHECK(r.config.destinations.empty());
		CHECK_FALSE(r.read_only);
	}
	CHECK(parse_config("{", p).status == LoadStatus::Malformed);
	CHECK(parse_config("{\"schema\":4,\"destinations\":{}}", p).status == LoadStatus::Ok);
	CHECK(!parse_config("{\"schema\":4,\"destinations\":{}}", p).warnings.empty());
}

TEST(L2, config_bad_destinations_are_skipped_not_fatal)
{
	InsecureTestProtector p;
	std::string text = "{\"schema\":4,\"destinations\":["
			   "{\"id\":\"d1\",\"name\":\"ok\",\"platform\":\"twitch\",\"url\":\"rtmp://h/app\"},"
			   "{\"id\":\"d2\",\"name\":\"badurl\",\"platform\":\"twitch\",\"url\":\"http://h/app\"},"
			   "42,"
			   "{\"id\":\"d1\",\"name\":\"dup id\",\"platform\":\"twitch\",\"url\":\"rtmp://h/app\"}]}";
	LoadResult r = parse_config(text, p);
	CHECK(r.status == LoadStatus::Ok);
	CHECK_EQ(r.config.destinations.size(), size_t(2));
	CHECK(r.config.destinations[0].id != r.config.destinations[1].id); // duplicate id regenerated
	CHECK_EQ(r.warnings.size(), size_t(2));
}

TEST(L2, config_caps_destination_count)
{
	InsecureTestProtector p;
	std::string text = "{\"schema\":4,\"destinations\":[";
	for (int i = 0; i < 20; ++i) {
		if (i)
			text += ",";
		text += "{\"id\":\"d" + std::to_string(i) + "\",\"name\":\"n\",\"platform\":\"custom\",\"url\":\"rtmp://h/a\"}";
	}
	text += "]}";
	LoadResult r = parse_config(text, p);
	CHECK_EQ(r.config.destinations.size(), size_t(kMaxDestinations));
	CHECK_EQ(r.warnings.size(), size_t(20 - kMaxDestinations));
}

TEST(L2, config_newer_schema_is_read_only)
{
	InsecureTestProtector p;
	LoadResult r = parse_config("{\"schema\":99,\"destinations\":[]}", p);
	CHECK(r.status == LoadStatus::TooNew);
	CHECK(r.read_only);
	CHECK_FALSE(r.needs_save);
}

TEST(L2, config_port_values_are_range_checked)
{
	InsecureTestProtector p;
	LoadResult r = parse_config("{\"schema\":4,\"server\":{\"port\":70000,\"last_port\":-1}}", p);
	CHECK_EQ(r.config.server.port, 0);
	CHECK_EQ(r.config.server.last_port, 0);
	CHECK(!r.warnings.empty());
	r = parse_config("{\"schema\":4,\"server\":{\"port\":51000,\"last_port\":51001}}", p);
	CHECK_EQ(r.config.server.port, 51000);
	CHECK_EQ(r.config.server.last_port, 51001);
}

TEST(L2, config_file_missing_gives_defaults)
{
	ScratchDir tmp;
	InsecureTestProtector p;
	MockClock clk;
	LoadResult r = load_config_file(tmp.file("config.json"), p, clk);
	CHECK(r.status == LoadStatus::Created);
	CHECK(r.config.destinations.empty());
	CHECK(r.config.auto_start_with_stream);
}

TEST(L2, config_file_save_load_atomic_and_no_temp_left)
{
	ScratchDir tmp;
	InsecureTestProtector p;
	MockClock clk;
	Config c;
	c.destinations.push_back(dest("d1", "Twitch"));
	std::string err;
	CHECK(seal_keys(c, p, err));
	std::string path = tmp.file("sub/dir/config.json");
	CHECK(save_config_file(path, c, err)); // creates parent directories
	CHECK(!fs::exists(fs::u8path(path + ".tmp")));
	CHECK(slurp(path).find(kFakeKey) == std::string::npos);
	// overwrite with a change
	c.destinations[0].handle = "changed";
	CHECK(save_config_file(path, c, err));
	LoadResult r = load_config_file(path, p, clk);
	CHECK(r.status == LoadStatus::Ok);
	CHECK_EQ(r.config.destinations[0].handle, std::string("changed"));
}

TEST(L2, config_file_malformed_is_moved_aside_with_timestamp)
{
	ScratchDir tmp;
	InsecureTestProtector p;
	MockClock clk(123456);
	std::string path = tmp.file("config.json");
	std::string err;
	CHECK(write_file_atomic(path, "{ this is not json", err));
	LoadResult r = load_config_file(path, p, clk);
	CHECK(r.status == LoadStatus::Malformed);
	CHECK(r.config.destinations.empty());
	CHECK(!fs::exists(fs::u8path(path)));
	CHECK(fs::exists(fs::u8path(path + ".bad-123456")));
	CHECK(r.message.find("config.json.bad-123456") != std::string::npos);
	// a subsequent load starts clean instead of tripping on the same file
	CHECK(load_config_file(path, p, clk).status == LoadStatus::Created);
}

TEST(L2, config_file_too_new_is_left_untouched)
{
	ScratchDir tmp;
	InsecureTestProtector p;
	MockClock clk;
	std::string path = tmp.file("config.json");
	std::string err;
	std::string body = "{\"schema\":7,\"future\":true}";
	CHECK(write_file_atomic(path, body, err));
	LoadResult r = load_config_file(path, p, clk);
	CHECK(r.status == LoadStatus::TooNew);
	CHECK(r.read_only);
	CHECK_EQ(slurp(path), body);
}

TEST(L2, config_oversized_file_is_malformed_not_oom)
{
	ScratchDir tmp;
	InsecureTestProtector p;
	MockClock clk;
	std::string path = tmp.file("config.json");
	std::string err;
	CHECK(write_file_atomic(path, "[" + std::string(2 * 1024 * 1024, '1') + "]", err));
	LoadResult r = load_config_file(path, p, clk);
	CHECK(r.status == LoadStatus::Malformed);
}

TEST(L2, unlock_fails_for_foreign_or_empty_token)
{
	InsecureTestProtector p;
	Destination d = dest("d1", "x");
	std::string plain = "stale";
	d.key.clear();
	CHECK_FALSE(unlock_key(d, p, plain));
	CHECK(plain.empty());
	d.key_token = "dpapi:v1:AAAA"; // sealed by someone else
	CHECK_FALSE(unlock_key(d, p, plain));
}

TEST(L2, overlay_json_has_no_secret_material_and_no_urls)
{
	std::vector<OverlayEntry> v;
	OverlayEntry e;
	e.id = "d1";
	e.platform = "twitch";
	e.handle = "otiyyy";
	e.state = State::Live;
	v.push_back(e);
	std::string j = overlay_state_json(v, 7, true);
	json::ParseResult r = json::parse(j);
	CHECK(r.ok);
	CHECK_EQ(r.value.find("destinations")->at(0).find("state")->as_string(), std::string("live"));
	CHECK(r.value.find("destinations")->at(0).find("live")->as_bool());
	CHECK(j.find("rtmp") == std::string::npos);
	CHECK(j.find("key") == std::string::npos);
	CHECK_EQ(r.value.find("seq")->as_int(), 7);
}

TEST(L2, state_hub_publishes_only_on_change_and_wakes_waiters)
{
	StateHub hub;
	std::vector<OverlayEntry> v;
	OverlayEntry e;
	e.id = "d1";
	e.platform = "twitch";
	e.handle = "h";
	v.push_back(e);
	uint64_t ver = 0;
	hub.snapshot(&ver);
	CHECK_EQ(ver, uint64_t(0));
	CHECK(hub.publish(v, false));
	CHECK_FALSE(hub.publish(v, false)); // identical: no new version
	hub.snapshot(&ver);
	CHECK_EQ(ver, uint64_t(1));
	v[0].state = State::Live;
	CHECK(hub.publish(v, true));
	std::string js;
	uint64_t nv = 0;
	CHECK(hub.wait_for_change(1, 10, js, nv));
	CHECK_EQ(nv, uint64_t(2));
	CHECK_FALSE(hub.wait_for_change(2, 10, js, nv)); // nothing newer: times out
	hub.shutdown();
	CHECK_FALSE(hub.wait_for_change(0, 1000, js, nv)); // returns immediately after shutdown
}

#ifdef _WIN32
TEST(L2, config_dpapi_end_to_end_file)
{
	std::unique_ptr<SecretProtector> p = make_dpapi_protector();
	CHECK(p != nullptr);
	if (!p)
		return;
	ScratchDir tmp;
	MockClock clk;
	Config c;
	c.destinations.push_back(dest("d1", "Twitch"));
	std::string err;
	CHECK(seal_keys(c, *p, err));
	std::string path = tmp.file("config.json");
	CHECK(save_config_file(path, c, err));
	std::string disk = slurp(path);
	CHECK(disk.find(kFakeKey) == std::string::npos);
	CHECK(disk.find("dpapi:v1:") != std::string::npos);
	LoadResult r = load_config_file(path, *p, clk);
	std::string plain;
	CHECK(unlock_key(r.config.destinations[0], *p, plain));
	CHECK_EQ(plain, kFakeKey);
}
#endif

TEST(L2, config_keeps_a_valid_overlay_access_key_and_drops_a_bad_one)
{
	InsecureTestProtector p;
	const std::string key = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
	CHECK(access_key_valid(key));
	CHECK_FALSE(access_key_valid(""));
	CHECK_FALSE(access_key_valid(key.substr(1)));
	CHECK_FALSE(access_key_valid(key + "0"));
	CHECK_FALSE(access_key_valid("00112233445566778899AABBCCDDEEFF00112233445566778899aabbccddeeff"));
	CHECK_FALSE(access_key_valid("00112233445566778899aabbccddeeff00112233445566778899aabbccddee/g"));
	Config c;
	c.server.access_key = key;
	std::string text, err;
	CHECK(serialize_config(c, text, err));
	LoadResult r = parse_config(text, p);
	CHECK(r.status == LoadStatus::Ok);
	CHECK_EQ(r.config.server.access_key, key);
	CHECK(r.warnings.empty());
	// a hand-edited or truncated key is not used (the plugin then makes a new one) and is reported
	r = parse_config("{\"schema\":4,\"server\":{\"access_key\":\"short\"}}", p);
	CHECK(r.config.server.access_key.empty());
	CHECK_EQ(r.warnings.size(), size_t(1));
	CHECK(r.warnings[0].find("short") == std::string::npos); // the bad value itself is never echoed
	// no key: nothing written, nothing read
	Config none;
	CHECK(serialize_config(none, text, err));
	CHECK(text.find("access_key") == std::string::npos);
	r = parse_config(text, p);
	CHECK(r.config.server.access_key.empty());
}
