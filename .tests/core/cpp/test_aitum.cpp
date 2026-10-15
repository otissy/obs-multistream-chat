// L2: Aitum aitum.json parsing and mapping (synthetic fixtures, obviously fake values), the launch
// plan (audio track + canvas rules), schema 3 -> 4 migration, validation of the new fields.
#include "oms_test.hpp"

#include "../../../src/core/aitum.hpp"
#include "../../../src/core/config.hpp"
#include "../../../src/core/plan.hpp"
#include "../../../src/core/secret.hpp"

using namespace oms;

namespace {

const std::string kFakeAitumKey = "FAKE_AITUM_KEY_not_real_0000";
const std::string kFakeServer = "rtmp://ingest.example.invalid/live";

// Same shape as the real 1.2.4 file for the keys we read (keys only were inspected; values here are synthetic).
std::string fixture(const std::string &extra_on_stream = "")
{
	return std::string("{\"main_stream_output_show\":true,\"canvas\":[{\"name\":\"Vertical\",\"width\":1080,\"height\":1920,\"uuid\":\"x\"},"
			   "{\"name\":\"GAMEPLAY ONLY\",\"width\":1920,\"height\":1080}],\"outputs\":["
			   "{\"enabled\":true,\"type\":\"stream\",\"name\":\"Vertical Stream\",\"canvas\":\"Vertical\",\"audio_encoder\":\"ffmpeg_aac\","
			   "\"audio_encoder_settings\":{\"bitrate\":128},\"audio_tracks\":40,\"vod_track\":-1,\"video_encoders\":[{\"enabled\":true,"
			   "\"type\":\"stream\",\"name\":\"Vertical Stream\",\"canvas\":\"Vertical\",\"video_encoder\":\"obs_nvenc_h264_tex\","
			   "\"video_encoder_settings\":{\"bitrate\":6000,\"max_bitrate\":6000,\"keyint_sec\":2,\"rate_control\":\"cbr\",\"preset\":\"p5\","
			   "\"profile\":\"high\",\"tune\":\"hq\",\"bf\":2,\"lookahead\":true,\"cqp\":20,\"nested\":{\"a\":1}},\"scale\":true,\"width\":720,"
			   "\"height\":1280,\"scale_type\":5,\"output_video_encoder\":\"\"}]" + extra_on_stream + "},"
			   "{\"enabled\":false,\"type\":\"backtrack\",\"name\":\"Backtrack\",\"canvas\":\"Vertical\",\"audio_tracks\":8},"
			   "{\"enabled\":true,\"type\":\"virtual_cam\",\"name\":\"VCam\"}]}");
}

Destination base()
{
	Destination d;
	d.id = "d1";
	d.name = "Vertical TikTok";
	d.platform = "tiktok";
	d.url = "rtmps://own.example.invalid/live";
	d.key_token = "test:v1:own";
	return d;
}

} // namespace

TEST(L2, aitum_parses_real_shape)
{
	AitumConfig c = parse_aitum(fixture());
	CHECK(c.status == AitumStatus::Ok);
	CHECK_EQ(c.canvases.size(), size_t(2));
	CHECK_EQ(c.outputs.size(), size_t(1)); // only type stream
	CHECK_EQ(c.skipped_outputs, 2);
	const AitumOutput &o = c.outputs[0];
	CHECK_EQ(o.name, std::string("Vertical Stream"));
	CHECK_EQ(o.canvas, std::string("Vertical"));
	CHECK_EQ(o.video_encoder, std::string("obs_nvenc_h264_tex"));
	CHECK_EQ(o.bitrate_kbps, 6000);
	CHECK_EQ(o.keyint_sec, 2);
	CHECK_EQ(o.audio_bitrate_kbps, 128);
	CHECK_EQ(o.audio_tracks_mask, 40);
	CHECK(o.scale);
	CHECK_EQ(o.width, 720);
	CHECK_EQ(o.scale_type, 5);
	CHECK_FALSE(o.from_unverified_keys);
	bool saw_bf = false, saw_nested = false, saw_bitrate = false;
	for (size_t i = 0; i < o.extra.size(); ++i) {
		saw_bf |= o.extra[i].key == "bf" && o.extra[i].type == 'i' && o.extra[i].value == "2";
		saw_nested |= o.extra[i].key == "nested";
		saw_bitrate |= o.extra[i].key == "bitrate";
	}
	CHECK(saw_bf);
	CHECK_FALSE(saw_nested);  // not a flat scalar
	CHECK_FALSE(saw_bitrate); // mapped, not duplicated
}

TEST(L2, aitum_rejects_bad_files_softly)
{
	CHECK(parse_aitum("").status == AitumStatus::Malformed);
	CHECK(parse_aitum("not json").status == AitumStatus::Malformed);
	CHECK(parse_aitum("[1,2]").status == AitumStatus::Malformed);
	CHECK(parse_aitum("{}").status == AitumStatus::UnknownShape);
	CHECK(parse_aitum("{\"outputs\":{},\"canvas\":[]}").status == AitumStatus::UnknownShape);
	CHECK(parse_aitum("{\"outputs\":[],\"canvas\":3}").status == AitumStatus::UnknownShape);
	std::string huge(kMaxAitumFileBytes + 1, ' ');
	CHECK(parse_aitum(huge).status == AitumStatus::TooBig);
	AitumConfig e = parse_aitum("{\"outputs\":[],\"canvas\":[]}");
	CHECK(e.status == AitumStatus::Ok);
	CHECK(e.outputs.empty());
	// entries of the wrong type, nameless outputs, hostile numbers and strings
	AitumConfig w = parse_aitum("{\"canvas\":[5,{\"name\":7}],\"outputs\":[1,null,{\"type\":\"stream\"},{\"type\":\"stream\",\"name\":\"A\\u0001B\","
				    "\"video_encoders\":[3],\"audio_tracks\":-4,\"audio_encoder_settings\":{\"bitrate\":1e99}}]}");
	CHECK(w.status == AitumStatus::Ok);
	CHECK_EQ(w.outputs.size(), size_t(1));
	CHECK(w.outputs[0].name.find('\x01') == std::string::npos);
	CHECK_EQ(w.outputs[0].audio_tracks_mask, 0);
	CHECK_EQ(w.outputs[0].audio_bitrate_kbps, 0);
	std::string big = "{\"canvas\":[],\"outputs\":[{\"type\":\"stream\",\"name\":\"" + std::string(10000, 'n') + "\"}]}";
	CHECK_EQ(parse_aitum(big).outputs[0].name.size(), size_t(128));
}

TEST(L2, aitum_maps_onto_destination)
{
	AitumConfig c = parse_aitum(fixture());
	Destination out;
	std::string key;
	std::vector<std::string> notes;
	CHECK(apply_aitum(base(), c.outputs[0], out, key, notes));
	CHECK_EQ(out.encoder, std::string("own"));
	CHECK_EQ(out.canvas, std::string("Vertical"));
	CHECK_EQ(out.video_encoder, std::string("obs_nvenc_h264_tex"));
	CHECK_EQ(out.bitrate_kbps, 6000);
	CHECK_EQ(out.keyint_sec, 2);
	CHECK_EQ(out.preset, std::string("p5"));
	CHECK_EQ(out.profile, std::string("high"));
	CHECK_EQ(out.rate_control, std::string("cbr"));
	CHECK_EQ(out.out_width, 720);
	CHECK_EQ(out.out_height, 1280);
	CHECK_EQ(out.scale_type, 5);
	CHECK_EQ(out.audio_bitrate_kbps, 128);
	CHECK_EQ(out.audio_track, 4); // mask 40 = bits 3 and 5 -> lowest is track 4
	CHECK_EQ(out.url, base().url); // Aitum has no server here: ours stays
	CHECK(key.empty());
	CHECK(!out.venc_extra.empty());
	bool said_server = false, said_tracks = false;
	for (size_t i = 0; i < notes.size(); ++i) {
		said_server |= notes[i].find("no server of its own") != std::string::npos;
		said_tracks |= notes[i].find("several audio tracks") != std::string::npos;
	}
	CHECK(said_server);
	CHECK(said_tracks);
	CHECK_EQ(out.id, std::string("d1")); // identity untouched
	CHECK_EQ(out.key_token, std::string("test:v1:own"));
}

TEST(L2, aitum_server_key_delay_are_flagged_unverified_and_never_in_messages)
{
	std::string extra = ",\"stream_server\":\"" + kFakeServer + "\",\"stream_key\":\"" + kFakeAitumKey +
			    "\",\"custom_delay\":true,\"delay_sec\":20,\"delay_preserve\":true";
	AitumConfig c = parse_aitum(fixture(extra));
	CHECK(c.status == AitumStatus::Ok);
	CHECK(c.outputs[0].from_unverified_keys);
	Destination out;
	std::string key;
	std::vector<std::string> notes;
	CHECK(apply_aitum(base(), c.outputs[0], out, key, notes));
	CHECK_EQ(out.url, kFakeServer);
	CHECK_EQ(key, kFakeAitumKey);
	CHECK_EQ(out.delay_sec, 20);
	CHECK(out.delay_preserve);
	CHECK(out.key.empty()); // the key travels only through key_out
	bool unverified = false;
	for (size_t i = 0; i < notes.size(); ++i) {
		unverified |= notes[i].find("unverified") != std::string::npos;
		CHECK(notes[i].find(kFakeAitumKey) == std::string::npos);
		CHECK(notes[i].find("example.invalid") == std::string::npos);
	}
	CHECK(unverified);
	CHECK(c.message.find(kFakeAitumKey) == std::string::npos);
	for (size_t i = 0; i < c.notes.size(); ++i)
		CHECK(c.notes[i].find(kFakeAitumKey) == std::string::npos);
	wipe(c);
	CHECK(c.outputs[0].key.empty());
	// a hostile server value is dropped, not used
	AitumConfig bad = parse_aitum(fixture(",\"stream_server\":\"javascript:alert(1)\",\"stream_key\":\"k\""));
	Destination o2;
	std::string k2;
	std::vector<std::string> n2;
	CHECK(apply_aitum(base(), bad.outputs[0], o2, k2, n2));
	CHECK_EQ(o2.url, base().url);
}

TEST(L2, aitum_unusable_values_fall_back_with_notes)
{
	AitumConfig c = parse_aitum("{\"canvas\":[],\"outputs\":[{\"type\":\"stream\",\"name\":\"S\",\"video_encoders\":[{\"video_encoder\":\"bad id!\","
				    "\"video_encoder_settings\":{\"bitrate\":99999999},\"scale\":true,\"width\":721,\"height\":1281}]}]}");
	Destination out;
	std::string key;
	std::vector<std::string> notes;
	CHECK(apply_aitum(base(), c.outputs[0], out, key, notes));
	CHECK_EQ(out.video_encoder, std::string("obs_x264"));
	CHECK_EQ(out.bitrate_kbps, 4500);
	CHECK_EQ(out.out_width, 720); // odd sizes made even
	CHECK_EQ(out.out_height, 1280);
	CHECK(notes.size() >= 2);
}

TEST(L2, aitum_double_streaming_detection)
{
	std::vector<TargetRef> t;
	t.push_back({"A (plugin)", "RTMP://Ingest.Example.invalid:1935/live/", "k1"});
	t.push_back({"B (aitum)", "rtmp://ingest.example.invalid/live?x=1", "k1"});
	t.push_back({"C", "rtmp://ingest.example.invalid/live", "k2"});
	t.push_back({"D", "rtmp://other.example.invalid/live", "k1"});
	t.push_back({"E", "rtmp://ingest.example.invalid/live", ""});
	std::vector<std::string> w = find_double_streaming(t);
	CHECK_EQ(w.size(), size_t(1));
	CHECK(w[0].find("A (plugin)") != std::string::npos && w[0].find("B (aitum)") != std::string::npos);
	CHECK(w[0].find("k1") == std::string::npos);
	CHECK(find_double_streaming(std::vector<TargetRef>()).empty());
	CHECK_EQ(normalize_target_url("rtmps://H.example:443/app//"), std::string("rtmps://h.example/app"));
	CHECK_EQ(normalize_target_url("garbage"), std::string());
}

// ---- plan ------------------------------------------------------------------------------------

static EnvSnapshot env_main_live()
{
	EnvSnapshot e;
	e.main_live = true;
	e.main_audio_tracks = {1, 3}; // main stream encodes tracks 1 and 3
	e.canvases.push_back({"", true, true, 1920, 1080});
	e.canvases.push_back({"Vertical", false, true, 1080, 1920});
	e.canvases.push_back({"Pending", false, false, 0, 0});
	return e;
}

TEST(L2, plan_shared_audio_track_rules)
{
	Destination d = base();
	d.encoder = "shared";
	LaunchPlan p;
	std::string err;
	CHECK(make_plan(d, env_main_live(), p, err));
	CHECK(p.video_share_main && p.audio_share_main && p.depends_on_main);
	CHECK_EQ(p.main_audio_index, 0);
	d.audio_track = 3; // carried by the main stream's second encoder: reused
	CHECK(make_plan(d, env_main_live(), p, err));
	CHECK(p.audio_share_main);
	CHECK_EQ(p.main_audio_index, 1);
	d.audio_track = 5; // not carried: own AAC encoder on track 5
	CHECK(make_plan(d, env_main_live(), p, err));
	CHECK_FALSE(p.audio_share_main);
	CHECK_EQ(p.audio_track, 5);
	CHECK(p.video_share_main);
	d.audio_track = 7;
	CHECK_FALSE(make_plan(d, env_main_live(), p, err));
	EnvSnapshot off = env_main_live();
	off.main_live = false;
	d.audio_track = 0;
	CHECK_FALSE(make_plan(d, off, p, err));
	CHECK(err.find("main stream first") != std::string::npos);
}

TEST(L2, plan_canvas_rules)
{
	Destination d = base();
	d.encoder = "own";
	d.bitrate_kbps = 4500;
	d.canvas = "Vertical";
	d.audio_track = 2;
	LaunchPlan p;
	std::string err;
	CHECK(make_plan(d, env_main_live(), p, err));
	CHECK_FALSE(p.video_share_main);
	CHECK_FALSE(p.audio_share_main);
	CHECK_FALSE(p.depends_on_main);
	CHECK_EQ(p.canvas_w, 1080);
	CHECK_EQ(p.canvas_h, 1920);
	CHECK_EQ(p.out_w, 0);
	CHECK_EQ(p.audio_track, 2);
	d.out_width = 720;
	d.out_height = 1280;
	CHECK(make_plan(d, env_main_live(), p, err));
	CHECK_EQ(p.out_w, 720);
	d.out_width = 1080;
	d.out_height = 1920; // same as the canvas: no rescale
	CHECK(make_plan(d, env_main_live(), p, err));
	CHECK_EQ(p.out_w, 0);
	d.canvas = "Gone";
	CHECK_FALSE(make_plan(d, env_main_live(), p, err));
	CHECK(err.find("not available") != std::string::npos);
	d.canvas = "Pending";
	CHECK_FALSE(make_plan(d, env_main_live(), p, err));
	CHECK(err.find("no video output") != std::string::npos);
	d.canvas = "";
	d.audio_track = 0;
	CHECK(make_plan(d, env_main_live(), p, err)); // main canvas, own encoder, track of main's first encoder
	CHECK_EQ(p.audio_track, 1);
	EnvSnapshot e = env_main_live();
	e.check_encoders = true;
	e.video_encoders = {"obs_x264"};
	d.video_encoder = "obs_qsv11";
	CHECK_FALSE(make_plan(d, e, p, err));
	CHECK(err.find("not installed") != std::string::npos);
	d.video_encoder = "obs_x264";
	CHECK(make_plan(d, e, p, err));
	// a shared destination cannot name a canvas
	d.encoder = "shared";
	d.canvas = "Vertical";
	CHECK_FALSE(make_plan(d, env_main_live(), p, err));
	// own-encoder outputs run without the main stream
	EnvSnapshot off = env_main_live();
	off.main_live = false;
	off.main_audio_tracks.clear();
	d.encoder = "own";
	CHECK(make_plan(d, off, p, err));
	CHECK_EQ(p.audio_track, 1);
}

TEST(L2, track_labels)
{
	CHECK_EQ(track_label(2, ""), std::string("Track 2"));
	CHECK_EQ(track_label(2, "Music"), std::string("Track 2: Music"));
}

// ---- validation and migration ----------------------------------------------------------------

TEST(L2, destination_v4_field_validation)
{
	Destination d = base();
	d.encoder = "own";
	d.bitrate_kbps = 4500;
	CHECK(validate_destination(d, false).empty());
	Destination x = d;
	x.canvas = "Vertical";
	CHECK(validate_destination(x, false).empty());
	x.encoder = "shared";
	x.bitrate_kbps = 0;
	CHECK(!validate_destination(x, false).empty()); // canvas needs own encoder
	x = d;
	x.audio_track = 7;
	CHECK(!validate_destination(x, false).empty());
	x = d;
	x.audio_bitrate_kbps = 8;
	CHECK(!validate_destination(x, false).empty());
	x = d;
	x.out_width = 720;
	CHECK(!validate_destination(x, false).empty()); // both or none
	x.out_height = 1281;
	CHECK(!validate_destination(x, false).empty()); // even
	x.out_height = 1280;
	CHECK(validate_destination(x, false).empty());
	x = d;
	x.video_encoder = "bad id";
	CHECK(!validate_destination(x, false).empty());
	x = d;
	x.fps_div = 5;
	CHECK(!validate_destination(x, false).empty());
	x = d;
	x.aitum_mode = "follow";
	CHECK(!validate_destination(x, false).empty()); // needs an output name
	x.aitum_output = "Vertical Stream";
	CHECK(validate_destination(x, false).empty());
	x.aitum_mode = "sometimes";
	CHECK(!validate_destination(x, false).empty());
	x = d;
	x.venc_extra.push_back({"Bad-Key", 'i', "1"});
	CHECK(!validate_destination(x, false).empty());
	x = d;
	x.delay_sec = 4000;
	CHECK(!validate_destination(x, false).empty());
}

TEST(L2, config_v3_migrates_to_v4_x264_becomes_own)
{
	InsecureTestProtector p;
	std::string v3 = "{\"schema\":3,\"destinations\":[{\"id\":\"d1\",\"name\":\"A\",\"platform\":\"twitch\",\"url\":\"rtmp://live.twitch.tv/app\","
			 "\"key_enc\":\"x\",\"encoder\":\"x264\",\"bitrate_kbps\":2500},{\"id\":\"d2\",\"name\":\"B\",\"platform\":\"twitch\","
			 "\"url\":\"rtmp://live.twitch.tv/app\",\"key_enc\":\"x\",\"encoder\":\"shared\"}]}";
	LoadResult r = parse_config(v3, p);
	CHECK(r.status == LoadStatus::Migrated);
	CHECK(r.needs_save);
	CHECK_EQ(r.config.schema, 4);
	CHECK(r.warnings.empty());
	CHECK_EQ(r.config.destinations.size(), size_t(2));
	const Destination &a = r.config.destinations[0];
	CHECK_EQ(a.encoder, std::string("own"));
	CHECK_EQ(a.video_encoder, std::string("obs_x264"));
	CHECK_EQ(a.preset, std::string("veryfast"));
	CHECK_EQ(a.keyint_sec, 2);
	CHECK_EQ(a.bitrate_kbps, 2500);
	CHECK_EQ(a.canvas, std::string(""));
	CHECK_EQ(a.audio_track, 0);
	CHECK_EQ(a.audio_bitrate_kbps, 160);
	CHECK_EQ(a.aitum_mode, std::string("off"));
	CHECK_EQ(r.config.destinations[1].encoder, std::string("shared"));
	std::string out, err;
	CHECK(serialize_config(r.config, out, err));
	LoadResult again = parse_config(out, p);
	CHECK(again.status == LoadStatus::Ok);
	CHECK_EQ(again.config.destinations.size(), size_t(2));
}

TEST(L2, config_v4_roundtrip_all_new_fields)
{
	InsecureTestProtector p;
	Config c;
	Destination d = base();
	d.key_token = "";
	d.key = "FAKE_KEY_0000";
	d.encoder = "own";
	d.bitrate_kbps = 5000;
	d.canvas = "GAMEPLAY ONLY";
	d.video_encoder = "obs_nvenc_h264_tex";
	d.keyint_sec = 0;
	d.preset = "p5";
	d.profile = "high";
	d.rate_control = "cbr";
	d.out_width = 1280;
	d.out_height = 720;
	d.scale_type = 5;
	d.fps_div = 2;
	d.audio_track = 3;
	d.audio_bitrate_kbps = 192;
	d.delay_sec = 30;
	d.delay_preserve = true;
	d.aitum_mode = "follow";
	d.aitum_output = "Vertical Stream";
	d.venc_extra.push_back({"bf", 'i', "2"});
	d.venc_extra.push_back({"lookahead", 'b', "true"});
	d.venc_extra.push_back({"tune", 's', "hq"});
	c.destinations.push_back(d);
	std::string err, text;
	CHECK(seal_keys(c, p, err));
	CHECK(serialize_config(c, text, err));
	CHECK(text.find("FAKE_KEY_0000") == std::string::npos);
	LoadResult r = parse_config(text, p);
	CHECK(r.status == LoadStatus::Ok);
	CHECK(r.warnings.empty());
	const Destination &o = r.config.destinations[0];
	CHECK_EQ(o.canvas, d.canvas);
	CHECK_EQ(o.video_encoder, d.video_encoder);
	CHECK_EQ(o.keyint_sec, 0);
	CHECK_EQ(o.out_width, 1280);
	CHECK_EQ(o.fps_div, 2);
	CHECK_EQ(o.audio_track, 3);
	CHECK_EQ(o.audio_bitrate_kbps, 192);
	CHECK_EQ(o.delay_sec, 30);
	CHECK(o.delay_preserve);
	CHECK_EQ(o.aitum_mode, std::string("follow"));
	CHECK_EQ(o.aitum_output, std::string("Vertical Stream"));
	CHECK_EQ(o.venc_extra.size(), size_t(3));
	CHECK_EQ(o.venc_extra[1].type, 'b');
	// schema 4 is "too new" for nothing; schema 5 is read-only
	CHECK(parse_config("{\"schema\":5}", p).status == LoadStatus::TooNew);
}

TEST(L2, config_v4_malformed_new_fields_skip_the_destination_not_the_file)
{
	InsecureTestProtector p;
	std::string t = "{\"schema\":4,\"destinations\":["
			"{\"id\":\"d1\",\"name\":\"ok\",\"platform\":\"twitch\",\"url\":\"rtmp://live.twitch.tv/app\",\"key_enc\":\"x\"},"
			"{\"id\":\"d2\",\"name\":\"bad canvas\",\"platform\":\"twitch\",\"url\":\"rtmp://live.twitch.tv/app\",\"key_enc\":\"x\","
			"\"encoder\":\"shared\",\"canvas\":\"Vertical\"},"
			"{\"id\":\"d3\",\"name\":\"x264 in v4\",\"platform\":\"twitch\",\"url\":\"rtmp://live.twitch.tv/app\",\"key_enc\":\"x\","
			"\"encoder\":\"x264\"}]}";
	LoadResult r = parse_config(t, p);
	CHECK(r.status == LoadStatus::Ok);
	CHECK_EQ(r.config.destinations.size(), size_t(1));
	CHECK_EQ(r.warnings.size(), size_t(2));
}
