// libobs side of the output fleet: implements OutputBackend (one obs_output per destination, all
// state per destination, see src/core/fleet.hpp) and owns the Fleet. All methods run on the UI
// thread except the libobs signal callbacks, which only push into the fleet's event queue.
#pragma once
#include "fleet.hpp"
#include "plan.hpp"

#include <obs.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oms {

struct EncoderChoice {
	std::string id;
	std::string label;
	std::string codec; // h264 / hevc / av1
	bool hardware = false;
};

struct AitumInfo {
	bool installed = false;
	std::string version; // "" when unknown
	bool version_inspected = false;
};

class OutputManager : public OutputBackend {
public:
	OutputManager(Config &cfg, const SecretProtector &protector, const Clock &clock);
	~OutputManager();

	void sync_with_config() { fleet_.sync_with_config(); }
	void on_main_stream_started();
	void on_main_stream_stopping() { fleet_.on_main_stream_stopping(); }
	void on_main_stream_stopped();
	bool main_live() const { return fleet_.main_live(); }
	void start_destination(const std::string &id) { fleet_.start_destination(id); }
	void stop_destination(const std::string &id) { fleet_.stop_destination(id); }
	void start_all() { fleet_.start_all(); }
	void stop_all() { fleet_.stop_all(); }
	void shutdown() { fleet_.shutdown(); }
	void tick();
	std::vector<RuntimeView> views() const { return fleet_.views(); }
	std::vector<OverlayEntry> overlay_entries() const { return fleet_.overlay_entries(); }
	std::string bottleneck_banner() const { return banner_; }
	Fleet &fleet() { return fleet_; }
	const std::vector<std::string> &double_stream_warnings() const { return double_warnings_; }

	// Pickers for the dock.
	std::vector<std::string> canvas_names() const; // non-main canvases currently registered
	std::vector<EncoderChoice> video_encoders() const;
	std::string track_name(int track) const; // "" when the user did not name it
	AitumInfo aitum_info() const;

	// OutputBackend
	bool launch(const std::string &id, uint64_t epoch, const Destination &eff, const std::string &key, bool &depends_on_main,
		    double &target_kbps, std::string &err) override;
	void request_stop(const std::string &id) override;
	bool sample(const std::string &id, Sample &out) override;
	void release(const std::string &id, bool force) override;
	bool has_output(const std::string &id) const override;

private:
	struct Ctx;
	struct Attempt {
		obs_output_t *out = nullptr;
		obs_service_t *svc = nullptr;
		Ctx *ctx = nullptr;
		obs_canvas_t *canvas = nullptr; // strong ref: the encoder's video_t belongs to it
	};

	EnvSnapshot snapshot() const;
	void drop(Attempt &a, bool disconnect);
	void update_banner(int64_t now);
	static void sig_start(void *data, calldata_t *cd);
	static void sig_stop(void *data, calldata_t *cd);

	Config &cfg_;
	const Clock &clock_;
	Fleet fleet_;
	std::map<std::string, Attempt> attempts_;
	std::string banner_;
	std::vector<std::string> double_warnings_;
	int64_t last_double_check_ = -1000000;
	struct MainSample {
		int64_t t;
		uint64_t frames;
		uint64_t dropped;
		uint32_t skipped;
		uint32_t lagged;
		uint32_t total;
	};
	std::deque<MainSample> main_samples_;
};

} // namespace oms
