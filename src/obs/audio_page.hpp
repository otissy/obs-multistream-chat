// The dock's Audio tab (0.5.0): which OBS audio track each destination sends, and a live level meter
// for every track, so you can see what each platform will hear (and a warning when it is silent).
// The meters read OBS's own mixer tracks (obs_add_raw_audio_callback) only while the tab is visible.
// No Q_OBJECT (functor connects only), like the rest of the dock.
#pragma once
#include <QWidget>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;
class QProgressBar;
class QTimer;
class QVBoxLayout;
struct audio_data; // libobs media-io/audio-io.h

namespace oms {

// dBFS of a peak sample value (0..1), clamped to [-60, 0]; -60 for silence.
float peak_to_db(float peak);
// 0..100 bar position for a dBFS value on the meters' -60..0 scale.
int db_to_bar(float db);

class AudioPage : public QWidget {
public:
	// save: called after a destination's track changed in the config (saves and syncs the outputs)
	AudioPage(QWidget *parent, std::function<void()> save);
	~AudioPage() override;
	void refresh(); // every 500 ms from the dock

protected:
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;

private:
	void rebuild_rows();
	void start_meters();
	void stop_meters();
	void tick_meters();
	static void on_audio(void *param, size_t mix_idx, ::audio_data *data);

	static constexpr int kTracks = 6;
	std::function<void()> save_;
	QVBoxLayout *dest_box_ = nullptr;
	QGridLayout *dest_grid_ = nullptr;
	QLabel *empty_ = nullptr;
	QLabel *track_name_[kTracks] = {};
	QProgressBar *track_bar_[kTracks] = {};
	std::vector<std::string> row_ids_;
	std::vector<QComboBox *> row_track_;
	std::vector<QProgressBar *> row_bar_;
	std::vector<QLabel *> row_note_;
	QTimer *meter_timer_ = nullptr;
	bool metering_ = false;
	std::atomic<float> peak_[kTracks];  // written by OBS's audio thread, read by the UI timer
	float shown_db_[kTracks] = {};      // what the bars show (falls back slowly)
	int silent_ticks_[kTracks] = {};    // UI ticks below -55 dBFS in a row
	size_t channels_ = 2;
};

} // namespace oms
