#include "audio_page.hpp"
#include "chat_panel.hpp"
#include "plan.hpp"
#include "plugin.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QAction>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace oms {

float peak_to_db(float peak)
{
	if (!(peak > 0.001f)) // below -60 dBFS (and NaN): silence on this scale
		return -60.0f;
	float db = 20.0f * std::log10(std::min(peak, 1.0f));
	return std::max(-60.0f, std::min(0.0f, db));
}

int db_to_bar(float db)
{
	float x = (std::max(-60.0f, std::min(0.0f, db)) + 60.0f) / 60.0f;
	return static_cast<int>(std::lround(x * 100.0f));
}

namespace {

QString qs(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

// The mixer track the main OBS stream sends: Advanced output's "Audio track", else track 1.
int main_stream_track()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return 1;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && std::strcmp(mode, "Advanced") == 0) {
		int t = static_cast<int>(config_get_int(cfg, "AdvOut", "TrackIndex"));
		if (t >= 1 && t <= 6)
			return t;
	}
	return 1;
}

QProgressBar *meter()
{
	QProgressBar *b = new QProgressBar();
	b->setRange(0, 100);
	b->setTextVisible(false);
	b->setFixedHeight(8);
	b->setMinimumWidth(60);
	// green to amber to red across the -60..0 dBFS scale
	b->setStyleSheet("QProgressBar { background:#1b1d24; border:1px solid #2c2f3a; border-radius:3px; }"
			 "QProgressBar::chunk { border-radius:2px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
			 " stop:0 #2f9e5b, stop:0.72 #3ddc84, stop:0.86 #ffb020, stop:1 #ff4d4f); }");
	return b;
}

} // namespace

AudioPage::AudioPage(QWidget *parent, std::function<void()> save) : QWidget(parent), save_(std::move(save))
{
	for (int i = 0; i < kTracks; ++i)
		peak_[i].store(0.0f);
	QVBoxLayout *root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(10);

	QLabel *how = new QLabel("Each platform can get its own sound mix. Pick the OBS audio track each destination sends; "
				 "the meters show what is on each track right now. Which sources go on which track is set in OBS: "
				 "Advanced Audio Properties, the Tracks checkboxes.");
	how->setWordWrap(true);
	how->setStyleSheet("color:#8a8a96;");
	root->addWidget(how);

	QWidget *mw = static_cast<QWidget *>(obs_frontend_get_main_window());
	QAction *adv = mw ? mw->findChild<QAction *>("actionAdvAudioProperties") : nullptr;
	if (adv) {
		QPushButton *open = new QPushButton("Open Advanced Audio Properties");
		QObject::connect(open, &QPushButton::clicked, [adv]() { adv->trigger(); });
		root->addWidget(open, 0, Qt::AlignLeft);
	}

	// destinations: one row each (rebuilt when the list changes)
	QGroupBox *dg = new QGroupBox("Sound per destination");
	dest_box_ = new QVBoxLayout(dg);
	empty_ = new QLabel("No destinations yet: add them on the Streams tab.");
	empty_->setStyleSheet("color:#8a8a96;");
	dest_box_->addWidget(empty_);
	QWidget *gw = new QWidget();
	dest_grid_ = new QGridLayout(gw);
	dest_grid_->setContentsMargins(0, 0, 0, 0);
	dest_grid_->setColumnStretch(2, 1);
	dest_box_->addWidget(gw);
	root->addWidget(dg);

	// every track
	QGroupBox *tg = new QGroupBox("OBS audio tracks");
	QGridLayout *tl = new QGridLayout(tg);
	tl->setColumnStretch(1, 1);
	for (int i = 0; i < kTracks; ++i) {
		track_name_[i] = new QLabel();
		track_bar_[i] = meter();
		tl->addWidget(track_name_[i], i, 0);
		tl->addWidget(track_bar_[i], i, 1);
	}
	root->addWidget(tg);
	root->addStretch(1);

	meter_timer_ = new QTimer(this);
	QObject::connect(meter_timer_, &QTimer::timeout, [this]() { tick_meters(); });
	refresh();
}

AudioPage::~AudioPage()
{
	stop_meters();
}

void AudioPage::showEvent(QShowEvent *e)
{
	QWidget::showEvent(e);
	start_meters();
}

void AudioPage::hideEvent(QHideEvent *e)
{
	stop_meters();
	QWidget::hideEvent(e);
}

// OBS's audio thread, once per audio tick (~21 ms) per track: keep the loudest sample since the last UI read.
void AudioPage::on_audio(void *param, size_t mix_idx, ::audio_data *data)
{
	AudioPage *self = static_cast<AudioPage *>(param);
	if (!data || mix_idx >= static_cast<size_t>(kTracks))
		return;
	float peak = 0.0f;
	for (size_t c = 0; c < self->channels_ && c < MAX_AV_PLANES; ++c) {
		const float *s = reinterpret_cast<const float *>(data->data[c]);
		if (!s)
			continue;
		for (uint32_t i = 0; i < data->frames; ++i) {
			float v = std::fabs(s[i]);
			if (v > peak)
				peak = v;
		}
	}
	float prev = self->peak_[mix_idx].load(std::memory_order_relaxed);
	while (peak > prev && !self->peak_[mix_idx].compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {
	}
}

void AudioPage::start_meters()
{
	if (metering_)
		return;
	audio_t *a = obs_get_audio();
	if (!a)
		return;
	channels_ = audio_output_get_channels(a);
	// planar float, OBS's own mix format: no conversion
	for (int i = 0; i < kTracks; ++i)
		obs_add_raw_audio_callback(static_cast<size_t>(i), nullptr, &AudioPage::on_audio, this);
	metering_ = true;
	meter_timer_->start(60);
}

void AudioPage::stop_meters()
{
	if (!metering_)
		return;
	meter_timer_->stop();
	for (int i = 0; i < kTracks; ++i)
		obs_remove_raw_audio_callback(static_cast<size_t>(i), &AudioPage::on_audio, this);
	metering_ = false;
	for (int i = 0; i < kTracks; ++i) {
		shown_db_[i] = -60.0f;
		track_bar_[i]->setValue(0);
	}
}

void AudioPage::tick_meters()
{
	for (int i = 0; i < kTracks; ++i) {
		float db = peak_to_db(peak_[i].exchange(0.0f, std::memory_order_relaxed));
		// rise at once, fall back about 20 dB per second (like OBS's own meters)
		shown_db_[i] = db >= shown_db_[i] ? db : std::max(db, shown_db_[i] - 1.2f);
		track_bar_[i]->setValue(db_to_bar(shown_db_[i]));
		silent_ticks_[i] = shown_db_[i] < -55.0f ? silent_ticks_[i] + 1 : 0;
	}
	int main_track = main_stream_track();
	for (size_t r = 0; r < row_ids_.size(); ++r) {
		int t = row_track_[r]->currentData().toInt();
		int idx = (t == 0 ? main_track : t) - 1;
		row_bar_[r]->setValue(track_bar_[idx]->value());
		bool silent = silent_ticks_[idx] > 80; // ~5 s of silence
		row_note_[r]->setText(silent ? QString::fromUtf8("\xE2\x9A\xA0 silent") : QString());
		row_note_[r]->setToolTip(silent ? "Nothing has been on this track for 5 seconds. In OBS: Advanced Audio Properties, "
						  "tick this track for the sources this platform should hear."
						: QString());
	}
}

void AudioPage::rebuild_rows()
{
	while (QLayoutItem *it = dest_grid_->takeAt(0)) {
		if (QWidget *w = it->widget())
			w->deleteLater();
		delete it;
	}
	row_track_.clear();
	row_bar_.clear();
	row_note_.clear();
	PluginState &st = plugin();
	OutputManager &mgr = *st.mgr;
	for (size_t r = 0; r < row_ids_.size(); ++r) {
		const Destination *d = nullptr;
		for (size_t k = 0; k < st.cfg.destinations.size(); ++k)
			if (st.cfg.destinations[k].id == row_ids_[r])
				d = &st.cfg.destinations[k];
		if (!d)
			continue;
		const std::string id = d->id;
		int row = static_cast<int>(r);
		QLabel *ico = new QLabel();
		ico->setPixmap(QPixmap::fromImage(create_platform_icon(d->platform, 18)));
		QLabel *name = new QLabel(qs(d->name));
		QComboBox *cb = new QComboBox();
		cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		cb->setMinimumContentsLength(12);
		cb->addItem("As the main stream", 0);
		for (int t = 1; t <= kTracks; ++t)
			cb->addItem(qs(track_label(t, mgr.track_name(t))), t);
		cb->setCurrentIndex(std::max(0, cb->findData(d->audio_track)));
		QObject::connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, id, cb](int) {
			int track = cb->currentData().toInt();
			PluginState &ps = plugin();
			for (size_t k = 0; k < ps.cfg.destinations.size(); ++k)
				if (ps.cfg.destinations[k].id == id && ps.cfg.destinations[k].audio_track != track) {
					ps.cfg.destinations[k].audio_track = track;
					if (save_)
						save_();
					return;
				}
		});
		QProgressBar *bar = meter();
		QLabel *note = new QLabel();
		note->setStyleSheet("color:#ffb020; font-size:11px;");
		dest_grid_->addWidget(ico, row, 0);
		dest_grid_->addWidget(name, row, 1);
		dest_grid_->addWidget(cb, row, 2);
		dest_grid_->addWidget(bar, row, 3);
		dest_grid_->addWidget(note, row, 4);
		row_track_.push_back(cb);
		row_bar_.push_back(bar);
		row_note_.push_back(note);
	}
}

void AudioPage::refresh()
{
	PluginState &st = plugin();
	OutputManager &mgr = *st.mgr;
	std::vector<std::string> ids;
	for (size_t i = 0; i < st.cfg.destinations.size(); ++i)
		ids.push_back(st.cfg.destinations[i].id);
	if (ids != row_ids_) {
		row_ids_ = ids;
		rebuild_rows();
	}
	empty_->setVisible(row_ids_.empty());
	int main_track = main_stream_track();
	for (int t = 1; t <= kTracks; ++t)
		track_name_[t - 1]->setText(qs(track_label(t, mgr.track_name(t))) +
					    (t == main_track ? QString::fromUtf8("  \xC2\xB7 main stream") : QString()));
	// a destination that is live keeps its track until it stops (the output cannot switch mixes)
	std::vector<RuntimeView> views = mgr.views();
	for (size_t r = 0; r < row_ids_.size() && r < row_track_.size(); ++r) {
		bool live = false;
		int track = 0;
		for (size_t v = 0; v < views.size(); ++v)
			if (views[v].dest.id == row_ids_[r]) {
				live = views[v].state == State::Live || views[v].state == State::Degraded;
				track = views[v].dest.audio_track;
			}
		QComboBox *cb = row_track_[r];
		int idx = cb->findData(track);
		if (idx >= 0 && idx != cb->currentIndex()) {
			bool prev = cb->blockSignals(true);
			cb->setCurrentIndex(idx);
			cb->blockSignals(prev);
		}
		cb->setEnabled(!live);
		cb->setToolTip(live ? "This destination is live: stop it to change its track." : QString());
	}
}

} // namespace oms
