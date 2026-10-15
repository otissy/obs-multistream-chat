// L2/L3: the output fleet through the OutputBackend abstraction with mock outputs: 8 simultaneous
// outputs, start/stop ordering, error isolation, concurrent notifications, Aitum "follow" resolution.
#include "oms_test.hpp"

#include "../../../src/core/clock.hpp"
#include "../../../src/core/fleet.hpp"

#include <atomic>
#include <map>
#include <set>
#include <thread>

using namespace oms;

namespace {

struct MockBackend : public OutputBackend {
	struct Out {
		bool exists = false;
		uint64_t epoch = 0;
		uint64_t bytes = 0;
	};
	Fleet *fleet = nullptr;
	std::mutex m;
	std::map<std::string, Out> outs;
	std::vector<std::string> calls; // "launch:id" "stop:id" "release:id" in order
	std::set<std::string> fail_ids;
	std::vector<std::thread> threads;
	std::vector<Destination> launched; // the effective destination each launch received
	std::vector<std::string> launched_keys;
	bool auto_started = true;

	~MockBackend()
	{
		for (size_t i = 0; i < threads.size(); ++i)
			threads[i].join();
	}
	void join_all()
	{
		for (size_t i = 0; i < threads.size(); ++i)
			threads[i].join();
		threads.clear();
	}
	bool launch(const std::string &id, uint64_t epoch, const Destination &eff, const std::string &key, bool &dep, double &target,
		    std::string &err) override
	{
		std::lock_guard<std::mutex> lk(m);
		calls.push_back("launch:" + id);
		launched.push_back(eff);
		launched_keys.push_back(key);
		Out &o = outs[id];
		o.exists = true;
		o.epoch = epoch;
		if (fail_ids.count(id)) {
			err = "mock: connect refused";
			return false;
		}
		dep = eff.encoder == "shared";
		target = 6000;
		if (auto_started) {
			Fleet *f = fleet;
			threads.push_back(std::thread([f, id, epoch]() { f->notify_started(id, epoch); }));
		}
		return true;
	}
	void request_stop(const std::string &id) override
	{
		std::lock_guard<std::mutex> lk(m);
		calls.push_back("stop:" + id);
		uint64_t ep = outs[id].epoch;
		Fleet *f = fleet;
		threads.push_back(std::thread([f, id, ep]() { f->notify_stopped(id, ep, kOutputSuccess, ""); }));
	}
	bool sample(const std::string &id, Sample &out) override
	{
		std::lock_guard<std::mutex> lk(m);
		Out &o = outs[id];
		o.bytes += 750000; // 6000 kbps per second
		out.bytes = o.bytes;
		out.frames = static_cast<int64_t>(o.bytes / 12500);
		out.dropped = 0;
		return true;
	}
	void release(const std::string &id, bool) override
	{
		std::lock_guard<std::mutex> lk(m);
		calls.push_back("release:" + id);
		outs[id].exists = false;
	}
	bool has_output(const std::string &id) const override
	{
		auto it = outs.find(id);
		return it != outs.end() && it->second.exists;
	}
	int index_of(const std::string &call) const
	{
		for (size_t i = 0; i < calls.size(); ++i)
			if (calls[i] == call)
				return static_cast<int>(i);
		return -1;
	}
};

Destination mk(const std::string &id, bool shared)
{
	Destination d;
	d.id = id;
	d.name = "Dest " + id;
	d.platform = "custom";
	d.url = "rtmp://" + id + ".example.invalid/live";
	d.key = "FAKE_KEY_" + id + "_0000";
	d.encoder = shared ? "shared" : "own";
	d.bitrate_kbps = shared ? 0 : 4500;
	d.auto_start = true;
	return d;
}

struct Rig {
	InsecureTestProtector prot;
	MockClock clock{1000};
	Config cfg;
	MockBackend be;
	std::unique_ptr<Fleet> fleet;
	explicit Rig(int shared_count, int own_count)
	{
		for (int i = 0; i < shared_count; ++i)
			cfg.destinations.push_back(mk("s" + std::to_string(i), true));
		for (int i = 0; i < own_count; ++i)
			cfg.destinations.push_back(mk("o" + std::to_string(i), false));
		std::string err;
		seal_keys(cfg, prot, err);
		fleet.reset(new Fleet(cfg, prot, clock, be));
		be.fleet = fleet.get();
	}
	void settle(int rounds = 6)
	{
		for (int i = 0; i < rounds; ++i) {
			be.join_all();
			clock.advance(1000);
			fleet->tick();
		}
	}
	int count_state(State s) const
	{
		int n = 0;
		std::vector<RuntimeView> v = fleet->views();
		for (size_t i = 0; i < v.size(); ++i)
			n += v[i].state == s ? 1 : 0;
		return n;
	}
};

} // namespace

TEST(L3, fleet_eight_simultaneous_outputs_all_go_live_and_stop)
{
	Rig r(4, 4); // 8 outputs: 4 share the main encoder, 4 have their own
	r.fleet->on_main_stream_started(); // auto_start: all 8 launch
	r.settle();
	CHECK_EQ(r.count_state(State::Live), 8);
	std::vector<RuntimeView> v = r.fleet->views();
	CHECK_EQ(v.size(), size_t(8));
	for (size_t i = 0; i < v.size(); ++i) {
		CHECK(v[i].bitrate_kbps > 3000); // every output sampled on its own
		CHECK(v[i].can_stop);
	}
	CHECK_EQ(r.be.calls.size(), size_t(8)); // exactly one launch each, nothing else
	r.fleet->stop_all();
	r.settle();
	CHECK_EQ(r.count_state(State::Idle), 8);
	for (int i = 0; i < 8; ++i)
		CHECK(!r.be.has_output(i < 4 ? "s" + std::to_string(i) : "o" + std::to_string(i - 4)));
}

TEST(L3, fleet_twelve_outputs_beyond_the_old_limit_of_two)
{
	Rig r(0, 12);
	r.fleet->start_all();
	r.settle();
	CHECK_EQ(r.count_state(State::Live), 12);
}

TEST(L3, fleet_one_failing_output_does_not_stop_the_others)
{
	Rig r(3, 3);
	r.be.fail_ids.insert("s1");
	r.be.fail_ids.insert("o2"); // two failures at once
	r.fleet->on_main_stream_started();
	r.settle();
	CHECK_EQ(r.count_state(State::Live), 4);
	std::vector<RuntimeView> v = r.fleet->views();
	int failing = 0;
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i].dest.id == "s1" || v[i].dest.id == "o2") {
			CHECK(v[i].state != State::Live);
			CHECK(v[i].last_error.find("connect refused") != std::string::npos);
			++failing;
		} else {
			CHECK(v[i].state == State::Live);
			CHECK(v[i].last_error.empty());
		}
	}
	CHECK_EQ(failing, 2);
	// the failures retry on their own backoff; the healthy ones are not relaunched
	r.be.fail_ids.clear();
	for (int i = 0; i < 8; ++i)
		r.settle(3);
	CHECK_EQ(r.count_state(State::Live), 6);
}

TEST(L3, fleet_main_stop_stops_dependents_first_and_keeps_manual_own_outputs)
{
	Rig r(2, 2);
	r.cfg.destinations[3].auto_start = false; // o1: manual own-encoder output
	r.fleet->sync_with_config();
	r.fleet->on_main_stream_started();
	r.fleet->start_destination("o1");
	r.settle();
	CHECK_EQ(r.count_state(State::Live), 4);
	size_t before = r.be.calls.size();
	r.fleet->on_main_stream_stopping();
	r.settle();
	CHECK(r.be.index_of("stop:s0") >= static_cast<int>(before));
	CHECK(r.be.index_of("stop:s1") >= static_cast<int>(before));
	CHECK(r.be.index_of("stop:o0") >= static_cast<int>(before)); // auto_start own output stops with the stream
	CHECK_EQ(r.be.index_of("stop:o1"), -1);                       // manual one keeps running
	r.fleet->on_main_stream_stopped();
	std::vector<RuntimeView> v = r.fleet->views();
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i].dest.id == "o1")
			CHECK(v[i].state == State::Live);
		else
			CHECK(v[i].state == State::Idle);
		CHECK(v[i].can_start == (v[i].dest.encoder == "own" && v[i].state == State::Idle));
	}
	// a shared destination cannot start without the main stream and says why
	r.fleet->start_destination("s0");
	r.settle(2);
	CHECK(r.count_state(State::Live) == 1);
	CHECK(r.fleet->views()[0].last_error.find("main stream first") != std::string::npos);
}

TEST(L3, fleet_concurrent_notifications_from_many_threads)
{
	Rig r(0, 8);
	r.be.auto_started = false;
	r.fleet->start_all();
	r.be.join_all();
	CHECK_EQ(r.count_state(State::Connecting), 8);
	std::vector<std::thread> ts;
	std::atomic<int> go(0);
	for (int i = 0; i < 8; ++i) {
		uint64_t ep;
		{
			std::lock_guard<std::mutex> lk(r.be.m);
			ep = r.be.outs["o" + std::to_string(i)].epoch;
		}
		ts.push_back(std::thread([&r, &go, i, ep]() {
			while (!go.load()) {
			}
			for (int k = 0; k < 200; ++k)
				r.fleet->notify_started("o" + std::to_string(i), ep);
		}));
	}
	go = 1;
	for (int k = 0; k < 50; ++k) {
		r.clock.advance(100);
		r.fleet->tick(); // drains while 8 threads push
	}
	for (size_t i = 0; i < ts.size(); ++i)
		ts[i].join();
	r.settle(2);
	CHECK_EQ(r.count_state(State::Live), 8);
}

TEST(L3, fleet_stale_events_from_an_old_attempt_are_ignored)
{
	Rig r(0, 1);
	r.be.auto_started = false;
	r.fleet->start_all();
	r.fleet->notify_started("o0", 999); // wrong epoch
	r.fleet->tick();
	CHECK(r.count_state(State::Live) == 0);
	r.fleet->notify_started("o0", r.be.outs["o0"].epoch);
	r.fleet->tick();
	CHECK(r.count_state(State::Live) == 1);
}

// ---- Aitum follow ------------------------------------------------------------------------------

static std::string aitum_text(const std::string &server, const std::string &key)
{
	return "{\"canvas\":[{\"name\":\"Vertical\",\"width\":1080,\"height\":1920}],\"outputs\":[{\"enabled\":true,\"type\":\"stream\","
	       "\"name\":\"Vertical Stream\",\"canvas\":\"Vertical\",\"stream_server\":\"" + server + "\",\"stream_key\":\"" + key +
	       "\",\"audio_tracks\":4,\"video_encoders\":[{\"video_encoder\":\"obs_x264\",\"video_encoder_settings\":{\"bitrate\":3000,"
	       "\"keyint_sec\":2}}]}]}";
}

TEST(L3, fleet_follow_aitum_uses_its_settings_and_key_only_in_memory)
{
	Rig r(0, 1);
	Destination &d = r.cfg.destinations[0];
	d.aitum_mode = "follow";
	d.aitum_output = "Vertical Stream";
	r.fleet->sync_with_config();
	r.fleet->set_aitum_provider([]() { return aitum_text("rtmp://aitum.example.invalid/live", "FAKE_AITUM_KEY_1111"); });
	r.fleet->start_all();
	r.settle();
	CHECK_EQ(r.be.launched.size(), size_t(1));
	const Destination &eff = r.be.launched[0];
	CHECK_EQ(eff.url, std::string("rtmp://aitum.example.invalid/live"));
	CHECK_EQ(eff.canvas, std::string("Vertical"));
	CHECK_EQ(eff.bitrate_kbps, 3000);
	CHECK_EQ(eff.audio_track, 3);
	CHECK_EQ(r.be.launched_keys[0], std::string("FAKE_AITUM_KEY_1111"));
	// our own config never learned the key or the server
	std::string text, err;
	CHECK(serialize_config(r.cfg, text, err));
	CHECK(text.find("FAKE_AITUM_KEY_1111") == std::string::npos);
	CHECK(text.find("aitum.example.invalid") == std::string::npos);
	// and no view carries it
	std::vector<RuntimeView> v = r.fleet->views();
	CHECK(v[0].notice.find("FAKE_AITUM_KEY_1111") == std::string::npos);
	CHECK(v[0].last_error.find("FAKE_AITUM_KEY_1111") == std::string::npos);
	CHECK(r.count_state(State::Live) == 1);
	// the file is re-read at every start: change it, restart, the new key is used
	r.fleet->stop_all();
	r.settle();
	r.fleet->set_aitum_provider([]() { return aitum_text("rtmp://aitum.example.invalid/live", "FAKE_AITUM_KEY_2222"); });
	r.fleet->start_all();
	r.settle();
	CHECK_EQ(r.be.launched_keys.back(), std::string("FAKE_AITUM_KEY_2222"));
}

TEST(L3, fleet_follow_aitum_fails_soft_on_missing_malformed_unknown)
{
	const char *texts[] = {"", "{{{", "{\"nothing\":true}", "{\"canvas\":[],\"outputs\":[]}"};
	for (size_t i = 0; i < 4; ++i) {
		Rig r(0, 1);
		Destination &d = r.cfg.destinations[0];
		d.aitum_mode = "follow";
		d.aitum_output = "Vertical Stream";
		r.fleet->sync_with_config();
		std::string t = texts[i];
		r.fleet->set_aitum_provider([t]() { return t; });
		r.fleet->start_all();
		r.settle();
		CHECK_EQ(r.count_state(State::Live), 1); // still streams with its own settings
		CHECK_EQ(r.be.launched[0].url, std::string("rtmp://o0.example.invalid/live"));
		CHECK_EQ(r.be.launched_keys[0], std::string("FAKE_KEY_o0_0000"));
		std::vector<RuntimeView> v = r.fleet->views();
		CHECK(v[0].notice.find("Aitum settings not used") != std::string::npos);
	}
}

TEST(L3, fleet_copy_from_aitum_seals_the_key)
{
	Rig r(0, 1);
	r.fleet->set_aitum_provider([]() { return aitum_text("rtmp://aitum.example.invalid/live", "FAKE_AITUM_KEY_3333"); });
	Destination out;
	std::vector<std::string> notes;
	std::string err;
	CHECK(r.fleet->copy_from_aitum(r.cfg.destinations[0], "Vertical Stream", out, notes, err));
	CHECK_EQ(out.url, std::string("rtmp://aitum.example.invalid/live"));
	CHECK(out.key.empty());
	CHECK(out.key_token.find("FAKE_AITUM_KEY_3333") == std::string::npos);
	std::string plain;
	CHECK(unlock_key(out, r.prot, plain));
	CHECK_EQ(plain, std::string("FAKE_AITUM_KEY_3333"));
	CHECK_EQ(out.aitum_mode, std::string("copy"));
	CHECK_FALSE(r.fleet->copy_from_aitum(r.cfg.destinations[0], "Nope", out, notes, err));
	std::string msg;
	std::vector<std::string> names = r.fleet->aitum_output_names(msg);
	CHECK_EQ(names.size(), size_t(1));
	CHECK_EQ(names[0], std::string("Vertical Stream"));
}

TEST(L3, fleet_double_stream_warning_names_places_never_values)
{
	Rig r(0, 2);
	r.fleet->set_aitum_provider([]() { return aitum_text("rtmp://o0.example.invalid/live", "FAKE_KEY_o0_0000"); });
	r.fleet->set_external_targets([]() {
		std::vector<TargetRef> v;
		v.push_back({"OBS main stream", "rtmp://o1.example.invalid/live/", "FAKE_KEY_o1_0000"});
		return v;
	});
	std::vector<std::string> w = r.fleet->double_stream_warnings();
	CHECK_EQ(w.size(), size_t(2));
	for (size_t i = 0; i < w.size(); ++i) {
		CHECK(w[i].find("FAKE_KEY") == std::string::npos);
		CHECK(w[i].find("example.invalid") == std::string::npos);
	}
	r.fleet->set_aitum_provider([]() { return std::string(); });
	r.fleet->set_external_targets(ExternalTargets());
	CHECK(r.fleet->double_stream_warnings().empty());
}
