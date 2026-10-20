// L2: the voice helper protocol and catalogue as the plugin reads them (src/core/voice.*).
// The same frames and catalogue rules are tested on the JS side in .tests/voice.
#include "oms_test.hpp"

#include "../../../src/core/json.hpp"
#include "../../../src/core/voice.hpp"

#include <fstream>
#include <sstream>

using namespace oms;

namespace {

std::string frame(uint8_t type, uint32_t id, uint32_t rate, const std::string &payload)
{
	std::string h("OMSV", 4);
	h.push_back(static_cast<char>(type));
	h.append(3, '\0');
	for (uint32_t v : {id, rate, static_cast<uint32_t>(payload.size())})
		for (int i = 0; i < 4; ++i)
			h.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
	return h + payload;
}

std::string read_file(const std::string &p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

std::string repo_file(const char *rel)
{
	std::string here = __FILE__;
	size_t cut = here.find(".tests");
	return here.substr(0, cut) + rel;
}

} // namespace

TEST(L2, voice_frames_decode_whatever_the_chunking)
{
	std::string all = frame(1, 7, 24000, std::string("\x01\x02\x03\x04", 4)) + frame(2, 7, 0, "") + frame(3, 0, 0, "bad");
	for (size_t step : {size_t(1), size_t(3), size_t(19), size_t(20), size_t(21), all.size()}) {
		voice::FrameDecoder d;
		std::vector<voice::Frame> got;
		for (size_t i = 0; i < all.size(); i += step)
			CHECK(d.push(all.data() + i, std::min(step, all.size() - i), got));
		CHECK_EQ(got.size(), size_t(3));
		if (got.size() != 3)
			continue;
		CHECK(got[0].type == voice::FrameType::Audio);
		CHECK_EQ(got[0].id, 7u);
		CHECK_EQ(got[0].sample_rate, 24000u);
		CHECK_EQ(got[0].payload.size(), size_t(4));
		CHECK(got[1].type == voice::FrameType::Done);
		CHECK(got[2].type == voice::FrameType::Error);
		CHECK_EQ(got[2].payload, std::string("bad"));
	}
}

TEST(L2, voice_frames_bad_magic_or_huge_length_poison_the_stream)
{
	voice::FrameDecoder d;
	std::vector<voice::Frame> got;
	CHECK_FALSE(d.push(std::string(20, 'x').data(), 20, got));
	CHECK(d.failed());
	std::string ok = frame(2, 1, 0, "");
	CHECK_FALSE(d.push(ok.data(), ok.size(), got)); // stays failed
	CHECK(got.empty());

	voice::FrameDecoder d2;
	std::string huge = frame(1, 1, 1, "");
	huge[16] = '\x01';
	huge[17] = '\x00';
	huge[18] = '\x00';
	huge[19] = '\x01'; // 16 MiB + 1
	CHECK_FALSE(d2.push(huge.data(), huge.size(), got));
}

TEST(L2, voice_catalogue_shipped_file_parses_and_lists_installed_voices)
{
	voice::Catalogue c;
	std::string err;
	CHECK(voice::parse_catalogue(read_file(repo_file("voice/catalogue.json")), c, err));
	CHECK_EQ(err, std::string());
	CHECK(c.packs.size() >= 50);
	CHECK(c.voices.size() >= 150);
	auto only = [](const char *id) { return [id](const voice::Pack &p) { return p.id == id; }; };
	std::vector<voice::Voice> v = voice::installed_voices(c, only("vits-piper-en_GB-alba-medium"));
	CHECK_EQ(v.size(), size_t(1));
	if (!v.empty()) {
		CHECK_EQ(v[0].id, std::string("piper.en_GB-alba-medium"));
		CHECK_EQ(voice::display_name(v[0]), std::string("English (Scottish): Alba, female"));
	}
	std::vector<voice::Voice> k = voice::installed_voices(c, only("kokoro-multi-lang-v1_0"));
	CHECK_EQ(k.size(), size_t(54));
	for (size_t i = 1; i < k.size(); ++i)
		CHECK(k[i - 1].language <= k[i].language);
	CHECK(voice::installed_voices(c, [](const voice::Pack &) { return false; }).empty());
	for (const voice::Pack &p : c.packs)
		CHECK(p.required.size() == (p.engine == "kokoro" ? 4u : 3u));
}

TEST(L2, voice_catalogue_refuses_escaping_paths_unknown_packs_and_duplicates)
{
	const std::string good = R"({"schema":1,"packs":[{"id":"p1","engine":"vits","files":{"model":"m.onnx","tokens":"t.txt","dataDir":"d"}}],"voices":[{"id":"v1","name":"A","language":"English","locale":"en-GB","pack":"p1","sid":0}]})";
	voice::Catalogue c;
	std::string err;
	CHECK(voice::parse_catalogue(good, c, err));
	auto bad = [&](const std::string &from, const std::string &to) {
		std::string t = good;
		size_t at = t.find(from);
		CHECK(at != std::string::npos);
		t.replace(at, from.size(), to);
		voice::Catalogue x;
		std::string e;
		return !voice::parse_catalogue(t, x, e) && !e.empty();
	};
	CHECK(bad("\"m.onnx\"", "\"../m.onnx\""));
	CHECK(bad("\"m.onnx\"", "\"C:\\\\m.onnx\""));
	CHECK(bad("\"m.onnx\"", "\"/etc/m\""));
	CHECK(bad("\"d\"", "\"a/../../d\""));
	CHECK(bad("\"pack\":\"p1\"", "\"pack\":\"p2\""));
	CHECK(bad("\"sid\":0", "\"sid\":-1"));
	CHECK(bad("\"sid\":0", "\"sid\":1.5"));
	CHECK(bad("\"engine\":\"vits\"", "\"engine\":\"sapi\""));
	CHECK(bad("\"schema\":1", "\"schema\":2"));
	CHECK(bad("\"id\":\"v1\"", "\"id\":\"../v\""));
	CHECK(bad("}]}", "},{\"id\":\"v1\",\"name\":\"A\",\"language\":\"E\",\"pack\":\"p1\",\"sid\":0}]}"));
	CHECK_FALSE(voice::parse_catalogue("not json", c, err));
}

TEST(L2, voice_requests_are_single_json_lines_even_with_hostile_text)
{
	std::string r = voice::say_request(42, "kokoro.bf_emma", "line one\nline two \"quoted\" \\ \x01 \xe2\x80\xa8", 1.25);
	CHECK_EQ(r.back(), '\n');
	CHECK_EQ(r.find('\n'), r.size() - 1);
	json::ParseResult p = json::parse(r.substr(0, r.size() - 1));
	CHECK(p.ok);
	CHECK_EQ(p.value.find("op")->as_string(), std::string("say"));
	CHECK_EQ(p.value.find("id")->as_int(), 42);
	CHECK_EQ(p.value.find("voice")->as_string(), std::string("kokoro.bf_emma"));
	CHECK_EQ(p.value.find("text")->as_string(), std::string("line one\nline two \"quoted\" \\ \x01 \xe2\x80\xa8"));
	CHECK_NEAR(p.value.find("speed")->as_number(), 1.25, 1e-9);
	CHECK_EQ(voice::flush_request(), std::string("{\"op\":\"flush\"}\n"));
}
