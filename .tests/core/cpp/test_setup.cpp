// Setup flow (0.5.0): "Get started" progress, chat source ids, the one add path, add-only merge from
// destinations, the reader start mode.
#include "oms_test.hpp"

#include "../../../src/core/chat_targets.hpp"
#include "../../../src/core/setup_flow.hpp"

using namespace oms;

static ChatSource src(const std::string &id, const std::string &platform, const std::string &target, bool on = true)
{
	ChatSource s;
	s.id = id;
	s.platform = platform;
	s.target = target;
	s.enabled = on;
	return s;
}

static Destination dest(const std::string &platform, const std::string &handle, const std::string &name = "x")
{
	Destination d;
	d.platform = platform;
	d.handle = handle;
	d.name = name;
	return d;
}

TEST(L2, setup_progress_counts_the_three_steps)
{
	Config c;
	CHECK_EQ(setup_progress(c, false).done(), 0);
	c.destinations.push_back(dest("twitch", "me"));
	CHECK_EQ(setup_progress(c, false).done(), 1);
	CHECK_FALSE(setup_progress(c, false).chat);
	c.chat.sources.push_back(src("c1", "twitch", "me"));
	CHECK_EQ(setup_progress(c, false).done(), 2);
	CHECK_FALSE(setup_progress(c, false).complete());
	CHECK_TRUE(setup_progress(c, true).complete());
	// a copied overlay alone counts as one step, in any order
	CHECK_EQ(setup_progress(Config(), true).done(), 1);
}

TEST(L2, next_chat_source_id_never_reuses_an_id)
{
	std::vector<ChatSource> s;
	CHECK_EQ(next_chat_source_id(s), std::string("c1"));
	s.push_back(src("c2", "twitch", "a"));
	s.push_back(src("c3", "twitch", "b"));
	// the old code used "c" + (size + 1) = "c3" here: a duplicate after a removal
	CHECK_EQ(next_chat_source_id(s), std::string("c1"));
	s.push_back(src("c1", "twitch", "c"));
	CHECK_EQ(next_chat_source_id(s), std::string("c4"));
}

TEST(L2, add_chat_source_normalises_dedupes_and_reenables)
{
	std::vector<ChatSource> s;
	std::string t, err;
	CHECK(add_chat_source(s, "twitch", "  https://twitch.tv/Shroud ", t, err) == AddResult::Added);
	CHECK_EQ(t, std::string("shroud"));
	CHECK_EQ(s.size(), static_cast<size_t>(1));
	CHECK_EQ(s[0].id, std::string("c1"));
	CHECK(add_chat_source(s, "twitch", "shroud", t, err) == AddResult::Duplicate);
	CHECK_EQ(s.size(), static_cast<size_t>(1));
	s[0].enabled = false;
	CHECK(add_chat_source(s, "twitch", "SHROUD", t, err) == AddResult::Enabled);
	CHECK_TRUE(s[0].enabled);
	CHECK_EQ(s.size(), static_cast<size_t>(1));
	// same name on another platform is another source
	CHECK(add_chat_source(s, "kick", "shroud", t, err) == AddResult::Added);
	CHECK_EQ(s.size(), static_cast<size_t>(2));
}

TEST(L2, add_chat_source_rejects_bad_input_and_a_full_list)
{
	std::vector<ChatSource> s;
	std::string t, err;
	CHECK(add_chat_source(s, "twitch", "", t, err) == AddResult::Invalid);
	CHECK(add_chat_source(s, "twitch", "javascript:alert(1)", t, err) == AddResult::Invalid);
	CHECK(add_chat_source(s, "twitch", "https://twitch.tv.evil.example/x", t, err) == AddResult::Invalid);
	CHECK(add_chat_source(s, "nope", "shroud", t, err) == AddResult::Invalid);
	CHECK(s.empty());
	for (int i = 0; i < kMaxChatSources; ++i)
		s.push_back(src("c" + std::to_string(i + 1), "twitch", "chan" + std::to_string(i + 100)));
	CHECK(add_chat_source(s, "twitch", "another_one", t, err) == AddResult::Full);
	CHECK_EQ(s.size(), static_cast<size_t>(kMaxChatSources));
	// a full list still re-enables an existing one
	s[3].enabled = false;
	CHECK(add_chat_source(s, "twitch", s[3].target, t, err) == AddResult::Enabled);
}

TEST(L2, from_destinations_is_add_only_and_keeps_a_collaborator)
{
	std::vector<ChatSource> s;
	s.push_back(src("c1", "twitch", "collab_friend"));
	std::vector<Destination> d;
	d.push_back(dest("twitch", "my_channel"));
	d.push_back(dest("youtube", "My YT"));     // not a channel handle (space): skipped
	d.push_back(dest("facebook", "me"));       // no chat reader: skipped
	d.push_back(dest("kick", "", "kickname"));  // no handle: the name is used
	CHECK_EQ(add_sources_from_destinations(d, s), 2);
	CHECK_EQ(s.size(), static_cast<size_t>(3));
	CHECK_EQ(s[0].target, std::string("collab_friend")); // the old code overwrote this one
	CHECK_EQ(s[1].target, std::string("my_channel"));
	CHECK_EQ(s[2].platform, std::string("kick"));
	CHECK(s[1].id != s[0].id && s[2].id != s[1].id && s[2].id != s[0].id);
	// running it again adds nothing
	CHECK_EQ(add_sources_from_destinations(d, s), 0);
	CHECK_EQ(s.size(), static_cast<size_t>(3));
}

TEST(L2, from_destinations_adds_the_youtube_channel_by_handle)
{
	std::vector<ChatSource> s;
	std::vector<Destination> d;
	d.push_back(dest("youtube", "RoyalAscent"));        // 11 letters: still the channel, never a video id
	d.push_back(dest("youtube", "@Other_Chan"));
	d.push_back(dest("youtube", "", "MyYouTubeName"));   // YouTube never falls back to the name
	CHECK_EQ(add_sources_from_destinations(d, s), 2);
	CHECK_EQ(s[0].target, std::string("@royalascent"));
	CHECK_EQ(s[1].target, std::string("@other_chan"));
	CHECK_EQ(chat_page_url("youtube", s[0].target), std::string("https://www.youtube.com/@royalascent/live"));
}

TEST(L2, voice_for_platform_uses_the_platform_voice_else_the_main_one)
{
	std::vector<std::pair<std::string, std::string>> per = {{"twitch", "kokoro.af_heart"}, {"kick", ""}, {"youtube", "piper.gone"}};
	std::vector<std::string> installed = {"kokoro.af_heart", "kokoro.bf_emma"};
	CHECK_EQ(voice_for_platform(per, "twitch", "kokoro.bf_emma", installed), std::string("kokoro.af_heart"));
	CHECK_EQ(voice_for_platform(per, "kick", "kokoro.bf_emma", installed), std::string("kokoro.bf_emma"));    // "same as main"
	CHECK_EQ(voice_for_platform(per, "youtube", "kokoro.bf_emma", installed), std::string("kokoro.bf_emma")); // uninstalled
	CHECK_EQ(voice_for_platform(per, "tiktok", "kokoro.bf_emma", installed), std::string("kokoro.bf_emma"));  // not set
	CHECK_EQ(voice_for_platform({}, "twitch", "", installed), std::string(""));
}

TEST(L2, from_destinations_ignores_a_name_that_is_just_the_platform_label)
{
	// 0.5.0's dialog fills the name with the platform's label; that is not a channel called "twitch"
	std::vector<ChatSource> s;
	std::vector<Destination> d;
	d.push_back(dest("twitch", "", "Twitch"));
	d.push_back(dest("kick", "", "kick"));
	CHECK_EQ(add_sources_from_destinations(d, s), 0);
	CHECK(s.empty());
}

TEST(L2, from_destinations_skips_handles_that_are_not_channels)
{
	std::vector<ChatSource> s;
	std::vector<Destination> d;
	d.push_back(dest("twitch", "my stream name with spaces!", ""));
	d.push_back(dest("twitch", "", ""));
	CHECK_EQ(add_sources_from_destinations(d, s), 0);
	CHECK(s.empty());
}

TEST(L2, reader_start_mode_round_trips)
{
	ChatConfig c;
	for (int m = 0; m < 3; ++m) {
		set_reader_start_mode(c, m);
		CHECK_EQ(reader_start_mode(c), m);
	}
	c.start_with_obs = true;
	c.auto_start_with_stream = true; // an old config with both ticked reads as "when OBS starts"
	CHECK_EQ(reader_start_mode(c), 1);
}
