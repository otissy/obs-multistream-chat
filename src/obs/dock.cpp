#include "dock.hpp"
#include "chat_panel.hpp"
#include "aitum.hpp"
#include "destination.hpp"
#include "plan.hpp"
#include "plugin.hpp"
#include "setup_flow.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGroupBox>
#include <QDir>
#include <QFile>
#include <QThread>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <vector>

namespace oms {

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString qs(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

std::string ss(const QString &q)
{
	return q.toUtf8().toStdString();
}

QString fmt_uptime(int64_t ms)
{
	if (ms <= 0)
		return QStringLiteral("-");
	int64_t s = ms / 1000;
	return QString("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}

// status dot colours: green live, amber degraded/reconnecting/connecting, red failed, grey idle
const char *dot_color(State s)
{
	switch (s) {
	case State::Live: return "#3ddc84";
	case State::Degraded:
	case State::Reconnecting:
	case State::Connecting: return "#ffb020";
	case State::Failed: return "#ff4d4f";
	case State::Idle: break;
	}
	return "#8a8a96";
}

QString state_label(const RuntimeView &v)
{
	switch (v.state) {
	case State::Idle: return T("State.Idle");
	case State::Connecting: return T("State.Connecting");
	case State::Live: return T("State.Live");
	case State::Degraded: return T("State.Degraded");
	case State::Reconnecting:
		if (v.retry_in_ms >= 0)
			return T("State.Reconnecting") + QString(" (%1 s, #%2)").arg((v.retry_in_ms + 999) / 1000).arg(v.attempts);
		return T("State.Reconnecting");
	case State::Failed: return T("State.Failed");
	}
	return QString();
}

// ---------------------------------------------------------------------------------------------

QString source_summary(const Destination &d)
{
	QString video = d.canvas.empty() ? T("Dialog.CanvasMain") : qs(d.canvas);
	QString audio = d.audio_track == 0 ? T("Dialog.TrackMainShort") : QString("T%1").arg(d.audio_track);
	return video + " / " + audio;
}

QString source_tooltip(const Destination &d)
{
	QString t = (d.encoder == "shared" ? T("Dialog.EncSharedShort") : qs(d.video_encoder) + QString(" %1 kbps").arg(d.bitrate_kbps));
	if (d.audio_track != 0)
		t += "\n" + qs(track_label(d.audio_track, plugin().mgr->track_name(d.audio_track)));
	if (d.aitum_mode != "off")
		t += "\nAitum: " + qs(d.aitum_mode) + " \"" + qs(d.aitum_output) + "\"";
	return t;
}

class DestinationDialog : public QDialog {
public:
	DestinationDialog(QWidget *parent, const Destination &d, bool editing) : QDialog(parent), dest_(d), editing_(editing)
	{
		setWindowTitle(editing ? T("Dialog.EditTitle") : T("Dialog.AddTitle"));
		setMinimumWidth(520);
		QFormLayout *form = new QFormLayout();

		platform_ = new QComboBox();
		const std::vector<PlatformPreset> &p = platform_presets();
		for (size_t i = 0; i < p.size(); ++i)
			platform_->addItem(QIcon(QPixmap::fromImage(create_platform_icon(p[i].id, 20))), qs(p[i].label), qs(p[i].id));
		int idx = platform_->findData(qs(d.platform.empty() ? "twitch" : d.platform));
		platform_->setCurrentIndex(idx < 0 ? 0 : idx);
		form->addRow(T("Dialog.Platform"), platform_);

		url_ = new QLineEdit(qs(d.url));
		url_->setPlaceholderText("rtmp://... or rtmps://...");
		form->addRow(T("Dialog.Url"), url_);

		hint_ = new QLabel();
		hint_->setWordWrap(true);
		hint_->setStyleSheet("color: #8a8a96;");
		form->addRow(QString(), hint_);

		key_ = new QLineEdit();
		key_->setEchoMode(QLineEdit::Password);
		key_->setMaxLength(512);
		key_->setPlaceholderText(editing && !d.key_token.empty() ? T("Dialog.KeyKeep") : T("Dialog.KeyHere"));
		reveal_ = new QPushButton(T("Dialog.Reveal"));
		reveal_->setCheckable(true);
		QHBoxLayout *kl = new QHBoxLayout();
		kl->setContentsMargins(0, 0, 0, 0);
		kl->addWidget(key_, 1);
		kl->addWidget(reveal_);
		QWidget *kw = new QWidget();
		kw->setLayout(kl);
		form->addRow(T("Dialog.Key"), kw);
		QObject::connect(reveal_, &QPushButton::toggled, [this](bool on) {
			if (on && key_->text().isEmpty() && !dest_.key_token.empty() && plugin().protector) {
				std::string plain;
				if (unlock_key(dest_, *plugin().protector, plain)) {
					key_->setText(qs(plain));
					secure_zero(plain);
				}
			}
			key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
			reveal_->setText(on ? T("Dialog.Hide") : T("Dialog.Reveal"));
		});

		handle_ = new QLineEdit(qs(d.handle));
		handle_->setMaxLength(100);
		handle_->setPlaceholderText(T("Dialog.HandleHint"));
		form->addRow(T("Dialog.Handle"), handle_);

		name_ = new QLineEdit(qs(d.name));
		name_->setMaxLength(64);
		name_->setPlaceholderText(T("Dialog.NameHint"));
		form->addRow(T("Dialog.Name"), name_);

		QFormLayout *adv = new QFormLayout();
		// ---- video source -------------------------------------------------------------------
		adv->addRow(heading(T("Dialog.SecVideo")));
		OutputManager &mgr = *plugin().mgr;
		canvas_ = new QComboBox();
		canvas_->addItem(T("Dialog.CanvasMain"), "");
		std::vector<CanvasInfo> canvases = mgr.canvases();
		for (size_t i = 0; i < canvases.size(); ++i) {
			if (canvases[i].is_main || canvases[i].name.empty())
				continue;
			QString label = qs(canvases[i].name);
			if (canvases[i].width > 0 && canvases[i].height > 0) {
				label += QString(" (%1x%2%3)").arg(canvases[i].width).arg(canvases[i].height)
					.arg(canvases[i].width < canvases[i].height ? " - Vertical" : "");
			}
			canvas_->addItem(label, qs(canvases[i].name));
		}
		if (!d.canvas.empty() && canvas_->findData(qs(d.canvas)) < 0)
			canvas_->addItem(qs(d.canvas) + " " + T("Dialog.NotAvailable"), qs(d.canvas));
		canvas_->setCurrentIndex(std::max(0, canvas_->findData(qs(d.canvas))));
		adv->addRow(T("Dialog.Canvas"), canvas_);

		encoder_ = new QComboBox();
		encoder_->addItem(T("Dialog.EncShared"), "shared");
		encoder_->addItem(T("Dialog.EncOwn"), "own");
		encoder_->setCurrentIndex(d.encoder == "own" ? 1 : 0);
		adv->addRow(T("Dialog.Encoder"), encoder_);

		venc_ = new QComboBox();
		std::vector<EncoderChoice> encs = mgr.video_encoders();
		bool has_x264 = false;
		for (size_t i = 0; i < encs.size(); ++i)
			has_x264 |= encs[i].id == "obs_x264";
		if (!has_x264)
			venc_->addItem("x264 (obs_x264)", "obs_x264");
		for (size_t i = 0; i < encs.size(); ++i)
			venc_->addItem(qs(encs[i].label) + " (" + qs(encs[i].id) + ")" + (encs[i].hardware ? " " + T("Dialog.UsesGpu") : QString()),
				       qs(encs[i].id));
		if (venc_->findData(qs(d.video_encoder)) < 0)
			venc_->addItem(qs(d.video_encoder) + " " + T("Dialog.NotInstalled"), qs(d.video_encoder));
		venc_->setCurrentIndex(std::max(0, venc_->findData(qs(d.video_encoder))));
		adv->addRow(T("Dialog.VideoEncoder"), venc_);

		bitrate_ = new QSpinBox();
		bitrate_->setRange(300, 51000);
		bitrate_->setSuffix(" kbps");
		bitrate_->setValue(d.bitrate_kbps >= 300 ? d.bitrate_kbps : 4500);
		adv->addRow(T("Dialog.Bitrate"), bitrate_);

		keyint_ = new QSpinBox();
		keyint_->setRange(0, 10);
		keyint_->setSuffix(" s");
		keyint_->setSpecialValueText(T("Dialog.Default"));
		keyint_->setValue(d.keyint_sec);
		adv->addRow(T("Dialog.Keyint"), keyint_);

		preset_ = new QLineEdit(qs(d.preset));
		preset_->setPlaceholderText(T("Dialog.Default"));
		profile_ = new QLineEdit(qs(d.profile));
		profile_->setPlaceholderText(T("Dialog.Default"));
		rate_ = new QComboBox();
		rate_->setEditable(true);
		rate_->addItems(QStringList() << "CBR" << "VBR" << "CQP" << "cbr" << "vbr");
		rate_->setCurrentText(qs(d.rate_control));
		QHBoxLayout *pl = new QHBoxLayout();
		pl->setContentsMargins(0, 0, 0, 0);
		pl->addWidget(new QLabel(T("Dialog.Preset")));
		pl->addWidget(preset_, 1);
		pl->addWidget(new QLabel(T("Dialog.Profile")));
		pl->addWidget(profile_, 1);
		pl->addWidget(new QLabel(T("Dialog.RateControl")));
		pl->addWidget(rate_, 1);
		QWidget *pw = new QWidget();
		pw->setLayout(pl);
		adv->addRow(QString(), pw);

		width_ = new QSpinBox();
		width_->setRange(0, 8192);
		width_->setSingleStep(2);
		width_->setSpecialValueText(T("Dialog.CanvasSize"));
		width_->setValue(d.out_width);
		height_ = new QSpinBox();
		height_->setRange(0, 8192);
		height_->setSingleStep(2);
		height_->setSpecialValueText(T("Dialog.CanvasSize"));
		height_->setValue(d.out_height);
		fps_ = new QComboBox();
		fps_->addItem(T("Dialog.Fps1"), 1);
		fps_->addItem(T("Dialog.Fps2"), 2);
		fps_->addItem(T("Dialog.Fps3"), 3);
		fps_->addItem(T("Dialog.Fps4"), 4);
		fps_->setCurrentIndex(std::max(0, std::min(3, d.fps_div - 1)));
		QHBoxLayout *rl = new QHBoxLayout();
		rl->setContentsMargins(0, 0, 0, 0);
		rl->addWidget(width_);
		rl->addWidget(new QLabel("x"));
		rl->addWidget(height_);
		rl->addWidget(fps_, 1);
		QWidget *rw = new QWidget();
		rw->setLayout(rl);
		adv->addRow(T("Dialog.Rescale"), rw);

		video_note_ = new QLabel();
		video_note_->setWordWrap(true);
		video_note_->setStyleSheet("color: #8a8a96;");
		adv->addRow(QString(), video_note_);

		// ---- audio ----------------------------------------------------------------------------
		adv->addRow(heading(T("Dialog.SecAudio")));
		track_ = new QComboBox();
		track_->addItem(T("Dialog.TrackMain"), 0);
		for (int t = 1; t <= kMaxAudioTracks; ++t)
			track_->addItem(qs(track_label(t, mgr.track_name(t))), t);
		track_->setCurrentIndex(std::max(0, std::min(kMaxAudioTracks, d.audio_track)));
		adv->addRow(T("Dialog.Track"), track_);
		abitrate_ = new QSpinBox();
		abitrate_->setRange(64, 320);
		abitrate_->setSuffix(" kbps");
		abitrate_->setValue(d.audio_bitrate_kbps);
		adv->addRow(T("Dialog.AudioBitrate"), abitrate_);
		audio_note_ = new QLabel();
		audio_note_->setWordWrap(true);
		audio_note_->setStyleSheet("color: #8a8a96;");
		adv->addRow(QString(), audio_note_);

		delay_ = new QSpinBox();
		delay_->setRange(0, 3600);
		delay_->setSuffix(" s");
		delay_->setValue(d.delay_sec);
		delay_keep_ = new QCheckBox(T("Dialog.DelayKeep"));
		delay_keep_->setChecked(d.delay_preserve);
		QHBoxLayout *dl = new QHBoxLayout();
		dl->setContentsMargins(0, 0, 0, 0);
		dl->addWidget(delay_);
		dl->addWidget(delay_keep_, 1);
		QWidget *dw = new QWidget();
		dw->setLayout(dl);
		adv->addRow(T("Dialog.Delay"), dw);

		// ---- Aitum ----------------------------------------------------------------------------
		adv->addRow(heading(T("Dialog.SecAitum")));
		aitum_mode_ = new QComboBox();
		aitum_mode_->addItem(T("Dialog.AitumOff"), "off");
		aitum_mode_->addItem(T("Dialog.AitumCopy"), "copy");
		aitum_mode_->addItem(T("Dialog.AitumFollow"), "follow");
		aitum_mode_->setCurrentIndex(d.aitum_mode == "follow" ? 2 : d.aitum_mode == "copy" ? 1 : 0);
		adv->addRow(T("Dialog.AitumMode"), aitum_mode_);
		aitum_output_ = new QComboBox();
		aitum_output_->setEditable(true);
		std::string amsg;
		std::vector<std::string> names = mgr.fleet().aitum_output_names(amsg);
		for (size_t i = 0; i < names.size(); ++i)
			aitum_output_->addItem(qs(names[i]));
		aitum_output_->setCurrentText(qs(d.aitum_output));
		import_btn_ = new QPushButton(T("Dialog.AitumImport"));
		QHBoxLayout *al = new QHBoxLayout();
		al->setContentsMargins(0, 0, 0, 0);
		al->addWidget(aitum_output_, 1);
		al->addWidget(import_btn_);
		QWidget *aw = new QWidget();
		aw->setLayout(al);
		adv->addRow(T("Dialog.AitumOutput"), aw);
		aitum_note_ = new QLabel();
		aitum_note_->setWordWrap(true);
		aitum_note_->setStyleSheet("color: #8a8a96;");
		AitumInfo ai = mgr.aitum_info();
		QString info;
		if (!ai.installed)
			info = T("Dialog.AitumNotLoaded");
		else if (ai.version_inspected)
			info = T("Dialog.AitumVersionOk").arg(qs(ai.version));
		else
			info = T("Dialog.AitumVersionOther").arg(ai.version.empty() ? QString("?") : qs(ai.version)).arg(kAitumInspectedVersion);
		if (!amsg.empty())
			info += "\n" + qs(amsg);
		info += "\n" + T("Dialog.AitumNeverTouches");
		aitum_note_->setText(info);
		adv->addRow(QString(), aitum_note_);
		QObject::connect(import_btn_, &QPushButton::clicked, [this]() { import_from_aitum(); });

		enabled_ = new QCheckBox(T("Dialog.Enabled"));
		enabled_->setChecked(d.enabled);
		auto_ = new QCheckBox(T("Dialog.AutoStart"));
		auto_->setChecked(d.auto_start);
		form->addRow(QString(), enabled_);
		form->addRow(QString(), auto_);

		adv_btn_ = new QPushButton();
		adv_btn_->setCheckable(true);
		adv_btn_->setFlat(true);
		adv_btn_->setStyleSheet("text-align:left; color:#58a6ff; font-weight:bold; padding:6px 0;");
		adv_box_ = new QWidget();
		adv_box_->setLayout(adv);
		QObject::connect(adv_btn_, &QPushButton::toggled, [this](bool on) { show_advanced(on); });

		error_ = new QLabel();
		error_->setWordWrap(true);
		error_->setStyleSheet("color: #ff4d4f;");
		error_->hide();

		QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		QObject::connect(bb, &QDialogButtonBox::rejected, [this]() { reject(); });
		QObject::connect(bb, &QDialogButtonBox::accepted, [this]() { try_accept(); });

		QWidget *body = new QWidget();
		QVBoxLayout *bl = new QVBoxLayout(body);
		bl->setContentsMargins(0, 0, 0, 0);
		bl->addLayout(form);
		bl->addWidget(adv_btn_);
		bl->addWidget(adv_box_);
		bl->addStretch(1);
		form_ = form;
		QScrollArea *scroll = new QScrollArea();
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidget(body);
		QVBoxLayout *root = new QVBoxLayout(this);
		root->addWidget(scroll, 1);
		root->addWidget(error_);
		root->addWidget(bb);
		resize(560, 460);

		prev_default_ = default_url_for(current_platform());
		if (const PlatformPreset *lp = find_preset(current_platform())) {
			prev_label_ = qs(lp->label);
			if (name_->text().trimmed().isEmpty())
				name_->setText(prev_label_);
		}
		QObject::connect(platform_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { on_platform_changed(); });
		QObject::connect(encoder_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		QObject::connect(canvas_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		QObject::connect(track_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		QObject::connect(aitum_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		update_hint();
		update_enabled();
		// open by itself when this destination already uses anything in it
		show_advanced(editing && uses_advanced(d));
	}

	Destination result() const { return dest_; }
	void open_advanced() { show_advanced(true); }
	void fill_for_shot(const char *handle)
	{
		handle_->setText(QString::fromUtf8(handle));
		key_->setText("live_000000_test"); // the documented fake key (packaging test)
	}

	// true when the destination differs from the plain "same picture, same sound as the main stream" setup
	static bool uses_advanced(const Destination &d)
	{
		const PlatformPreset *p = find_preset(d.platform);
		bool custom_url = p && *p->default_url && !d.url.empty() && d.url != p->default_url;
		return !d.canvas.empty() || d.encoder == "own" || d.audio_track != 0 || d.delay_sec > 0 || d.aitum_mode != "off" ||
		       custom_url;
	}

private:
	static QLabel *heading(const QString &text)
	{
		QLabel *l = new QLabel("<b>" + text + "</b>");
		l->setStyleSheet("margin-top:8px;");
		return l;
	}
	std::string current_platform() const { return ss(platform_->currentData().toString()); }
	static std::string default_url_for(const std::string &platform)
	{
		const PlatformPreset *p = find_preset(platform);
		return p ? p->default_url : "";
	}
	void update_hint()
	{
		const PlatformPreset *p = find_preset(current_platform());
		hint_->setText(p ? qs(p->url_hint) : QString());
		update_url_row();
	}
	void update_url_row()
	{
		const PlatformPreset *p = find_preset(current_platform());
		bool fixed = p && *p->default_url;
		bool show = !fixed || (adv_btn_ && adv_btn_->isChecked()) || url_->text().trimmed().isEmpty();
		form_->setRowVisible(url_, show);
		form_->setRowVisible(hint_, show && !hint_->text().isEmpty());
	}
	void show_advanced(bool on)
	{
		if (adv_btn_->isChecked() != on) {
			bool prev = adv_btn_->blockSignals(true);
			adv_btn_->setChecked(on);
			adv_btn_->blockSignals(prev);
		}
		adv_btn_->setText(QString::fromUtf8(on ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ") + T("Dialog.Advanced"));
		adv_box_->setVisible(on);
		update_url_row();
		if (on && isVisible())
			resize(std::max(width(), 760), std::max(height(), 760));
	}
	void update_enabled()
	{
		bool off_main = !ss(canvas_->currentData().toString()).empty();
		if (off_main && encoder_->currentIndex() != 1) {
			bool prev = encoder_->blockSignals(true);
			encoder_->setCurrentIndex(1); // another canvas needs its own encoder
			encoder_->blockSignals(prev);
		}
		bool own = ss(encoder_->currentData().toString()) == "own";
		for (QWidget *w : std::vector<QWidget *>{venc_, bitrate_, keyint_, preset_, profile_, rate_, width_, height_, fps_})
			w->setEnabled(own);
		bitrate_->setToolTip(own ? QString() : T("Dialog.BitrateShared"));
		int track = track_->currentData().toInt();
		abitrate_->setEnabled(own || track != 0);
		video_note_->setText(off_main ? T("Dialog.NoteCanvas") : own ? T("Dialog.NoteOwn") : T("Dialog.NoteShared"));
		audio_note_->setText(own ? T("Dialog.NoteAudioOwn") : track == 0 ? T("Dialog.NoteAudioMain") : T("Dialog.NoteAudioSharedTrack"));
		bool follow = ss(aitum_mode_->currentData().toString()) == "follow";
		import_btn_->setEnabled(ss(aitum_mode_->currentData().toString()) != "off");
		if (follow)
			video_note_->setText(video_note_->text() + "\n" + T("Dialog.NoteFollow"));
	}
	void on_platform_changed()
	{
		const PlatformPreset *np = find_preset(current_platform());
		QString nm = name_->text().trimmed();
		if (np && (nm.isEmpty() || nm == prev_label_))
			name_->setText(qs(np->label));
		prev_label_ = np ? qs(np->label) : QString();
		std::string cur = ss(url_->text().trimmed());
		std::string next = default_url_for(current_platform());
		if (cur.empty() || cur == prev_default_)
			url_->setText(qs(next));
		prev_default_ = next;
		if (current_platform() == "tiktok" && canvas_->currentIndex() == 0) {
			for (int i = 0; i < canvas_->count(); ++i) {
				QString id = canvas_->itemData(i).toString();
				if (id.contains("vertical", Qt::CaseInsensitive)) {
					canvas_->setCurrentIndex(i);
					break;
				}
			}
		}
		update_hint();
	}

	// Form -> Destination (keeps identity, key_token and the imported extras of dest_).
	Destination collect() const
	{
		Destination d = dest_;
		d.platform = current_platform();
		d.name = ss(name_->text().trimmed());
		d.url = ss(url_->text().trimmed());
		d.handle = ss(handle_->text().trimmed());
		d.canvas = ss(canvas_->currentData().toString());
		d.encoder = ss(encoder_->currentData().toString());
		d.video_encoder = ss(venc_->currentData().toString());
		d.bitrate_kbps = d.encoder == "own" ? bitrate_->value() : 0;
		d.keyint_sec = keyint_->value();
		d.preset = ss(preset_->text().trimmed());
		d.profile = ss(profile_->text().trimmed());
		d.rate_control = ss(rate_->currentText().trimmed());
		d.out_width = width_->value();
		d.out_height = height_->value();
		d.fps_div = fps_->currentData().toInt();
		d.audio_track = track_->currentData().toInt();
		d.audio_bitrate_kbps = abitrate_->value();
		d.delay_sec = delay_->value();
		d.delay_preserve = delay_keep_->isChecked();
		d.aitum_mode = ss(aitum_mode_->currentData().toString());
		d.aitum_output = ss(aitum_output_->currentText().trimmed());
		d.enabled = enabled_->isChecked();
		d.auto_start = auto_->isChecked();
		return d;
	}
	void load(const Destination &d)
	{
		url_->setText(qs(d.url));
		canvas_->setCurrentIndex(std::max(0, canvas_->findData(qs(d.canvas))));
		if (!d.canvas.empty() && canvas_->findData(qs(d.canvas)) < 0)
			canvas_->addItem(qs(d.canvas) + " " + T("Dialog.NotAvailable"), qs(d.canvas));
		canvas_->setCurrentIndex(std::max(0, canvas_->findData(qs(d.canvas))));
		encoder_->setCurrentIndex(d.encoder == "own" ? 1 : 0);
		if (venc_->findData(qs(d.video_encoder)) < 0)
			venc_->addItem(qs(d.video_encoder) + " " + T("Dialog.NotInstalled"), qs(d.video_encoder));
		venc_->setCurrentIndex(std::max(0, venc_->findData(qs(d.video_encoder))));
		bitrate_->setValue(d.bitrate_kbps >= 300 ? d.bitrate_kbps : 4500);
		keyint_->setValue(d.keyint_sec);
		preset_->setText(qs(d.preset));
		profile_->setText(qs(d.profile));
		rate_->setCurrentText(qs(d.rate_control));
		width_->setValue(d.out_width);
		height_->setValue(d.out_height);
		fps_->setCurrentIndex(std::max(0, std::min(3, d.fps_div - 1)));
		track_->setCurrentIndex(std::max(0, std::min(kMaxAudioTracks, d.audio_track)));
		abitrate_->setValue(d.audio_bitrate_kbps);
		delay_->setValue(d.delay_sec);
		delay_keep_->setChecked(d.delay_preserve);
		update_enabled();
	}
	// Reads the chosen Aitum output once (key sealed) and shows what was mapped.
	bool import_from_aitum()
	{
		Destination base = collect();
		Destination out;
		std::vector<std::string> notes;
		std::string err;
		if (!plugin().mgr->fleet().copy_from_aitum(base, base.aitum_output, out, notes, err)) {
			error_->setText(T("Dialog.AitumImportFailed") + "\n" + qs(err));
			error_->show();
			return false;
		}
		dest_ = out; // carries venc_extra and a sealed key if Aitum had one
		load(out);
		QString msg = T("Dialog.AitumImported");
		for (size_t i = 0; i < notes.size(); ++i)
			msg += "\n- " + qs(notes[i]);
		error_->hide();
		aitum_note_->setText(msg);
		imported_ = true;
		return true;
	}
	void try_accept()
	{
		if (ss(aitum_mode_->currentData().toString()) == "copy" && !imported_ && !ss(aitum_output_->currentText().trimmed()).empty() &&
		    !import_from_aitum())
			return;
		Destination d = collect();
		d.key = ss(key_->text().trimmed());
		std::vector<ValidationIssue> issues = validate_destination(d, !editing_ || d.key_token.empty());
		if (!issues.empty()) {
			QString msg;
			for (size_t i = 0; i < issues.size(); ++i)
				msg += qs(issues[i].message) + "\n";
			error_->setText(msg.trimmed());
			error_->show();
			secure_zero(d.key);
			return;
		}
		dest_ = d;
		secure_zero(d.key);
		key_->clear();
		accept();
	}

	Destination dest_;
	bool editing_;
	bool imported_ = false;
	std::string prev_default_;
	QString prev_label_;
	QFormLayout *form_ = nullptr;
	QPushButton *adv_btn_ = nullptr;
	QWidget *adv_box_ = nullptr;
	QComboBox *platform_;
	QLineEdit *name_, *url_, *key_, *handle_, *preset_, *profile_;
	QLabel *hint_, *error_, *video_note_, *audio_note_, *aitum_note_;
	QPushButton *reveal_, *import_btn_;
	QComboBox *encoder_, *canvas_, *venc_, *rate_, *fps_, *track_, *aitum_mode_, *aitum_output_;
	QSpinBox *bitrate_, *keyint_, *width_, *height_, *abitrate_, *delay_;
	QCheckBox *enabled_, *auto_, *delay_keep_;
};

// Small per-user UI state next to config.json (not part of the config schema): the "Get started" card.
QString ui_state_path()
{
	char *p = obs_module_config_path("ui-state.ini");
	QString s = p ? QString::fromUtf8(p) : QString();
	bfree(p);
	return s;
}

bool ui_flag(const char *key)
{
	QString p = ui_state_path();
	return !p.isEmpty() && QSettings(p, QSettings::IniFormat).value(key, false).toBool();
}

void set_ui_flag(const char *key, bool on)
{
	QString p = ui_state_path();
	if (p.isEmpty())
		return;
	QSettings s(p, QSettings::IniFormat);
	s.setValue(key, on);
}

QWidget *scrolled(QWidget *page)
{
	QScrollArea *sa = new QScrollArea();
	sa->setWidgetResizable(true);
	sa->setFrameShape(QFrame::NoFrame);
	sa->setWidget(page);
	return sa;
}

} // namespace

// ---------------------------------------------------------------------------------------------

MultistreamDock::MultistreamDock(QWidget *parent) : QWidget(parent)
{
	chat_panel_ = new ChatPanel(this);
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(4);

	// ---- always visible: warnings, the big button and one summary line ---------------------------
	QWidget *head = new QWidget();
	QVBoxLayout *hl = new QVBoxLayout(head);
	hl->setContentsMargins(6, 6, 6, 0);
	banner_ = new QLabel();
	banner_->setWordWrap(true);
	banner_->setStyleSheet("background:#5a3a00; color:#ffd58a; padding:6px; border-radius:4px;");
	banner_->hide();
	hl->addWidget(banner_);
	notice_ = new QLabel();
	notice_->setWordWrap(true);
	notice_->setStyleSheet("background:#3a2530; color:#ffb3c1; padding:6px; border-radius:4px;");
	notice_->hide();
	hl->addWidget(notice_);
	double_ = new QLabel();
	double_->setWordWrap(true);
	double_->setStyleSheet("background:#5a1f1f; color:#ffc9c9; padding:6px; border-radius:4px;");
	double_->hide();
	hl->addWidget(double_);

	// at-a-glance strip for use DURING a stream: one big button and one summary line
	big_btn_ = new QPushButton();
	big_btn_->setMinimumHeight(40);
	big_btn_->setStyleSheet("font-size:15px; font-weight:bold;");
	glance_ = new QLabel();
	glance_->setWordWrap(true);
	hl->addWidget(big_btn_);
	hl->addWidget(glance_);

	// ---- "Get started": three steps, each with the button that does it ---------------------------
	setup_card_ = new QFrame();
	setup_card_->setObjectName("omscSetup");
	setup_card_->setStyleSheet("#omscSetup { background:#1d2433; border:1px solid #2f3b55; border-radius:6px; }");
	QVBoxLayout *cl = new QVBoxLayout(setup_card_);
	cl->setContentsMargins(10, 8, 10, 8);
	cl->setSpacing(6);
	QHBoxLayout *ct = new QHBoxLayout();
	setup_title_ = new QLabel();
	setup_title_->setStyleSheet("font-weight:bold; font-size:13px; color:#f2f2f5;");
	QPushButton *hide_setup = new QPushButton(T("Setup.Hide"));
	hide_setup->setFlat(true);
	hide_setup->setCursor(Qt::PointingHandCursor);
	hide_setup->setStyleSheet("QPushButton { border:none; background:transparent; color:#9aa4b8; font-size:11px; text-decoration:underline; padding:0 2px; }");
	ct->addWidget(setup_title_, 1);
	ct->addWidget(hide_setup);
	cl->addLayout(ct);
	const char *step_keys[3] = {"Setup.Step1", "Setup.Step2", "Setup.Step3"};
	const char *btn_keys[3] = {"Setup.Btn1", "Setup.Btn2", "Setup.Btn3"};
	for (int i = 0; i < 3; ++i) {
		QHBoxLayout *r = new QHBoxLayout();
		step_lbl_[i] = new QLabel();
		step_lbl_[i]->setWordWrap(true);
		step_btn_[i] = new QPushButton(T(btn_keys[i]));
		step_btn_[i]->setStyleSheet("background-color:#5865F2; color:#ffffff; font-weight:bold; padding:4px 12px; border-radius:3px;");
		step_text_[i] = T(step_keys[i]);
		r->addWidget(step_lbl_[i], 1);
		r->addWidget(step_btn_[i]);
		cl->addLayout(r);
	}
	hl->addWidget(setup_card_);
	outer->addWidget(head);

	// ---- one flat row of tabs --------------------------------------------------------------------
	tabs_ = new QTabWidget();
	tabs_->setDocumentMode(true);
	tabs_->setUsesScrollButtons(true);
	QWidget *streams_page = new QWidget();
	QWidget *overlays_page = new QWidget();
	tab_chat_ = tabs_->addTab(chat_panel_->live_page(), T("Tab.Chat"));
	tab_streams_ = tabs_->addTab(streams_page, T("Tab.Streams"));
	tab_sources_ = tabs_->addTab(scrolled(chat_panel_->sources_page()), T("Tab.Sources"));
	tab_overlays_ = tabs_->addTab(scrolled(overlays_page), T("Tab.Overlays"));
	tab_voice_ = tabs_->addTab(scrolled(chat_panel_->voice_page()), T("Tab.Voice"));
	outer->addWidget(tabs_, 1);
	outer->addWidget(chat_panel_->footer());
	chat_panel_->attach(tabs_, tab_chat_, [this]() { tabs_->setCurrentIndex(tab_sources_); });

	// ---- Streams ---------------------------------------------------------------------------------
	QVBoxLayout *root = new QVBoxLayout(streams_page);
	root->setContentsMargins(6, 6, 6, 6);
	streams_empty_ = new QLabel(T("Streams.Empty"));
	streams_empty_->setWordWrap(true);
	streams_empty_->setStyleSheet("color:#c8c8d0; padding:12px 4px;");
	streams_empty_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	root->addWidget(streams_empty_, 1);

	table_ = new QTableWidget(0, 9);
	QStringList heads;
	heads << T("Col.On") << T("Col.Status") << T("Col.Name") << T("Col.Handle") << T("Dialog.Track") << T("Col.Bitrate") << T("Col.Dropped")
	      << T("Col.Uptime") << QString();
	table_->setHorizontalHeaderLabels(heads);
	table_->verticalHeader()->hide();
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->horizontalHeader()->setStretchLastSection(false);
	table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
	table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(8, QHeaderView::Fixed);
	table_->setColumnWidth(8, 64); // the Start/Stop button: cell widgets do not size their column
	root->addWidget(table_, 1);

	QHBoxLayout *row1 = new QHBoxLayout();
	QPushButton *add = new QPushButton(T("Btn.Add"));
	add->setStyleSheet("background-color:#5865F2; color:#ffffff; font-weight:bold; padding:4px 12px; border-radius:3px;");
	edit_btn_ = new QPushButton(T("Btn.Edit"));
	remove_btn_ = new QPushButton(T("Btn.Remove"));
	QPushButton *guide_btn = new QPushButton(T("Btn.AudioHelp"));
	guide_btn->setFlat(true);
	guide_btn->setStyleSheet("color:#58a6ff;");
	row1->addWidget(add);
	row1->addWidget(edit_btn_);
	row1->addWidget(remove_btn_);
	row1->addStretch(1);
	row1->addWidget(guide_btn);
	root->addLayout(row1);

	// ---- Overlays: both browser-source URLs in one place -----------------------------------------
	QVBoxLayout *ol = new QVBoxLayout(overlays_page);
	ol->setContentsMargins(8, 8, 8, 8);
	ol->setSpacing(10);
	QLabel *ohow = new QLabel(T("Overlays.How"));
	ohow->setWordWrap(true);
	ohow->setStyleSheet("color:#8a8a96;");
	ol->addWidget(ohow);
	for (int i = 0; i < 2; ++i) {
		QGroupBox *g = new QGroupBox(T(i == 0 ? "Overlays.ChatTitle" : "Overlays.IconsTitle"));
		QVBoxLayout *gl = new QVBoxLayout(g);
		QLabel *what = new QLabel(T(i == 0 ? "Overlays.ChatWhat" : "Overlays.IconsWhat"));
		what->setWordWrap(true);
		gl->addWidget(what);
		ov_url_[i] = new QLabel();
		ov_url_[i]->setTextInteractionFlags(Qt::TextSelectableByMouse);
		ov_url_[i]->setStyleSheet("color:#8a8a96; font-family:Consolas, monospace; font-size:11px;");
		ov_url_[i]->setWordWrap(true);
		gl->addWidget(ov_url_[i]);
		QHBoxLayout *r = new QHBoxLayout();
		r->addWidget(new QLabel(T("Overlays.Theme")));
		ov_theme_[i] = new QComboBox();
		ov_theme_[i]->addItem(T("Overlays.ThemeDark"), "dark");
		ov_theme_[i]->addItem(T("Overlays.ThemeTransparent"), "transparent");
		ov_theme_[i]->addItem(T("Overlays.ThemeLight"), "light");
		r->addWidget(ov_theme_[i]);
		r->addStretch(1);
		QPushButton *copy = new QPushButton(T("Overlays.Copy"));
		copy->setStyleSheet("background-color:#5865F2; color:#ffffff; font-weight:bold; padding:4px 12px; border-radius:3px;");
		r->addWidget(copy);
		gl->addLayout(r);
		QLabel *size = new QLabel(T(i == 0 ? "Overlays.ChatSize" : "Overlays.IconsSize"));
		size->setWordWrap(true);
		size->setStyleSheet("color:#8a8a96; font-size:11px;");
		gl->addWidget(size);
		ov_copied_[i] = new QLabel(T("Overlays.Copied"));
		ov_copied_[i]->setStyleSheet("color:#3ddc84;");
		ov_copied_[i]->hide();
		gl->addWidget(ov_copied_[i]);
		ol->addWidget(g);
		QObject::connect(copy, &QPushButton::clicked, [this, i]() { copy_overlay_url(i); });
		QObject::connect(ov_theme_[i], QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { refresh(); });
	}
	ol->addStretch(1);

	QObject::connect(add, &QPushButton::clicked, [this]() { add_destination(); });
	QObject::connect(edit_btn_, &QPushButton::clicked, [this]() { edit_selected(); });
	QObject::connect(remove_btn_, &QPushButton::clicked, [this]() { remove_selected(); });
	QObject::connect(guide_btn, &QPushButton::clicked, [this]() { show_audio_tutorial(); });
	QObject::connect(big_btn_, &QPushButton::clicked, [this]() {
		if (big_btn_->property("act").toString() == "stop")
			plugin().mgr->stop_all();
		else
			plugin().mgr->start_all();
	});
	QObject::connect(step_btn_[0], &QPushButton::clicked, [this]() {
		tabs_->setCurrentIndex(tab_streams_);
		add_destination();
	});
	QObject::connect(step_btn_[1], &QPushButton::clicked, [this]() { tabs_->setCurrentIndex(tab_sources_); });
	QObject::connect(step_btn_[2], &QPushButton::clicked, [this]() { tabs_->setCurrentIndex(tab_overlays_); });
	QObject::connect(hide_setup, &QPushButton::clicked, [this]() {
		set_ui_flag("setup_hidden", true);
		refresh();
	});
	QObject::connect(table_, &QTableWidget::itemSelectionChanged, [this]() {
		bool any = !selected_id().empty();
		edit_btn_->setEnabled(any);
		remove_btn_->setEnabled(any);
	});
	edit_btn_->setEnabled(false);
	remove_btn_->setEnabled(false);
	refresh();
	// first run: start where the work is; afterwards on the live chat
	tabs_->setCurrentIndex(setup_progress(plugin().cfg, ui_flag("overlay_copied")).destination ? tab_chat_ : tab_streams_);
}

std::string MultistreamDock::selected_id() const
{
	QList<QTableWidgetSelectionRange> r = table_->selectedRanges();
	if (r.isEmpty())
		return std::string();
	int row = r.first().topRow();
	return (row >= 0 && row < static_cast<int>(row_ids_.size())) ? row_ids_[static_cast<size_t>(row)] : std::string();
}

void MultistreamDock::save_and_sync()
{
	std::string err;
	if (!plugin_save_config(err))
		QMessageBox::warning(this, T("Dock.Title"), T("Msg.SaveFailed") + "\n" + qs(err));
	plugin().mgr->sync_with_config();
	refresh();
}

void MultistreamDock::add_destination()
{
	PluginState &st = plugin();
	if (st.cfg.destinations.size() >= static_cast<size_t>(kMaxDestinations)) {
		QMessageBox::information(this, T("Dock.Title"), T("Msg.TooMany"));
		return;
	}
	Destination d;
	unsigned seed = static_cast<unsigned>(st.clock.now_ms()) ^ static_cast<unsigned>(st.cfg.destinations.size() * 2654435761u);
	bool unique = false;
	while (!unique) {
		d.id = make_id(seed++);
		unique = true;
		for (size_t i = 0; i < st.cfg.destinations.size(); ++i)
			if (st.cfg.destinations[i].id == d.id)
				unique = false;
	}
	d.platform = "twitch";
	d.url = find_preset("twitch")->default_url;
	DestinationDialog dlg(this, d, false);
	if (dlg.exec() == QDialog::Accepted) {
		st.cfg.destinations.push_back(dlg.result());
		save_and_sync();
	}
}

void MultistreamDock::edit_selected()
{
	PluginState &st = plugin();
	std::string id = selected_id();
	for (size_t i = 0; i < st.cfg.destinations.size(); ++i) {
		if (st.cfg.destinations[i].id != id)
			continue;
		DestinationDialog dlg(this, st.cfg.destinations[i], true);
		if (dlg.exec() == QDialog::Accepted) {
			Destination nd = dlg.result();
			if (nd.key.empty())
				nd.key_token = st.cfg.destinations[i].key_token; // blank = keep the saved key
			st.cfg.destinations[i] = nd;
			save_and_sync();
		}
		return;
	}
}

void MultistreamDock::remove_selected()
{
	PluginState &st = plugin();
	std::string id = selected_id();
	for (size_t i = 0; i < st.cfg.destinations.size(); ++i) {
		if (st.cfg.destinations[i].id != id)
			continue;
		if (QMessageBox::question(this, T("Dock.Title"), T("Msg.ConfirmRemove").arg(qs(st.cfg.destinations[i].name))) != QMessageBox::Yes)
			return;
		st.cfg.destinations.erase(st.cfg.destinations.begin() + static_cast<long>(i));
		save_and_sync();
		return;
	}
}

void MultistreamDock::copy_overlay_url(int which)
{
	std::string url = which == 0 ? plugin_chat_url() : plugin_overlay_url();
	if (url.empty()) {
		QMessageBox::warning(this, T("Dock.Title"), T("Msg.NoServer"));
		return;
	}
	QApplication::clipboard()->setText(overlay_url(which));
	ov_copied_[which]->show();
	set_ui_flag("overlay_copied", true);
	refresh();
}

// The URL the Copy button puts on the clipboard (theme from the combo; the chat gets readable defaults).
QString MultistreamDock::overlay_url(int which) const
{
	std::string base = which == 0 ? plugin_chat_url() : plugin_overlay_url();
	if (base.empty())
		return QString();
	QString theme = ov_theme_[which]->currentData().toString();
	return qs(base) + "?theme=" + theme + (which == 0 ? "&font=22&max=30" : "");
}

void MultistreamDock::update_setup(const Config &cfg)
{
	SetupProgress p = setup_progress(cfg, ui_flag("overlay_copied"));
	bool show = !p.complete() && !ui_flag("setup_hidden");
	setup_card_->setVisible(show);
	if (!show)
		return;
	setup_title_->setText(T("Setup.Title").arg(p.done()));
	bool done[3] = {p.destination, p.chat, p.overlay};
	for (int i = 0; i < 3; ++i) {
		step_lbl_[i]->setText(QString::fromUtf8(done[i] ? "\xE2\x9C\x94 " : "") + QString("%1. ").arg(i + 1) + step_text_[i]);
		step_lbl_[i]->setStyleSheet(done[i] ? "color:#3ddc84;" : "color:#e0e0e8;");
		step_btn_[i]->setVisible(!done[i]);
	}
}

void MultistreamDock::rebuild_rows(const std::vector<std::string> &ids)
{
	row_ids_ = ids;
	table_->setRowCount(static_cast<int>(ids.size()));
	OutputManager &mgr = *plugin().mgr;
	for (size_t i = 0; i < ids.size(); ++i) {
		int row = static_cast<int>(i);
		std::string id = ids[i];
		QCheckBox *cb = new QCheckBox();
		cb->setStyleSheet("margin-left:8px;");
		QObject::connect(cb, &QCheckBox::toggled, [this, id](bool on) {
			PluginState &st = plugin();
			for (size_t k = 0; k < st.cfg.destinations.size(); ++k)
				if (st.cfg.destinations[k].id == id && st.cfg.destinations[k].enabled != on) {
					st.cfg.destinations[k].enabled = on;
					save_and_sync();
					return;
				}
		});
		table_->setCellWidget(row, 0, cb);

		QComboBox *track_cb = new QComboBox();
		track_cb->addItem(T("Dialog.TrackMain"), 0);
		for (int t = 1; t <= kMaxAudioTracks; ++t)
			track_cb->addItem(qs(track_label(t, mgr.track_name(t))), t);
		track_cb->setStyleSheet("QComboBox { padding: 2px 4px; font-size: 11px; }");
		track_cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		track_cb->setMinimumContentsLength(13);
		QObject::connect(track_cb, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, id, track_cb](int) {
			int chosen_track = track_cb->currentData().toInt();
			PluginState &st = plugin();
			for (size_t k = 0; k < st.cfg.destinations.size(); ++k) {
				if (st.cfg.destinations[k].id == id && st.cfg.destinations[k].audio_track != chosen_track) {
					st.cfg.destinations[k].audio_track = chosen_track;
					save_and_sync();
					return;
				}
			}
		});
		table_->setCellWidget(row, 4, track_cb);

		QPushButton *btn = new QPushButton();
		QObject::connect(btn, &QPushButton::clicked, [id, btn]() {
			if (btn->property("act").toString() == "stop")
				plugin().mgr->stop_destination(id);
			else
				plugin().mgr->start_destination(id);
		});
		table_->setCellWidget(row, 8, btn);
		for (int c = 1; c <= 7; ++c) {
			if (c == 4)
				continue;
			table_->setItem(row, c, new QTableWidgetItem());
		}
	}
	table_->resizeColumnsToContents();
	table_->setColumnWidth(8, 64);
}

// Big start/stop button and the one-line summary: destinations live / total, worst state, chat reader.
void MultistreamDock::update_glance(const std::vector<RuntimeView> &views)
{
	PluginState &st = plugin();
	int live = 0, trouble = 0, enabled = 0;
	bool any_active = false, any_startable = false;
	double kbps = 0;
	for (size_t i = 0; i < views.size(); ++i) {
		const RuntimeView &v = views[i];
		if (v.dest.enabled)
			++enabled;
		if (v.state == State::Live) {
			++live;
			kbps += v.bitrate_kbps;
		}
		if (v.state == State::Degraded || v.state == State::Reconnecting || v.state == State::Failed)
			++trouble;
		any_active = any_active || v.can_stop;
		any_startable = any_startable || v.can_start;
	}
	big_btn_->setProperty("act", any_active ? "stop" : "start");
	big_btn_->setText(any_active ? T("Btn.StopAllBig") : T("Btn.StartAllBig"));
	big_btn_->setEnabled(any_active || any_startable);
	big_btn_->setStyleSheet(QString("font-size:15px; font-weight:bold; color:white; background:%1;").arg(any_active ? "#9b2c2c" : "#2f7d4f"));

	QString chat;
	if (!plugin_reader_running()) {
		chat = T("Glance.ChatOff");
	} else {
		std::vector<SourceStatus> src = st.chat_hub.sources();
		int reading = 0;
		for (size_t i = 0; i < src.size(); ++i)
			reading += src[i].state == "reading" ? 1 : 0;
		QString c_on = T("Glance.ChatOn");
		chat = c_on.contains("%1") ? c_on.arg(reading).arg(static_cast<int>(src.size()))
					   : QString("chat %1/%2").arg(reading).arg(static_cast<int>(src.size()));
	}
	QString t_str = T("Glance.Streams");
	QString line = t_str.contains("%1") ? t_str.arg(live).arg(enabled)
					    : QString("%1 of %2 live").arg(live).arg(enabled);
	if (live > 0)
		line += QString(" | %1 kbps").arg(static_cast<int>(kbps));
	if (trouble > 0) {
		QString c_tr = T("Glance.Trouble");
		line += " | " + (c_tr.contains("%1") ? c_tr.arg(trouble) : QString("%1 in trouble").arg(trouble));
	}
	line += " | " + chat;
	glance_->setText(line);
	glance_->setStyleSheet(trouble > 0 ? "color:#ffb020;" : live > 0 ? "color:#3ddc84;" : "color:#8a8a96;");
}

void MultistreamDock::refresh()
{
	PluginState &st = plugin();
	if (chat_panel_)
		chat_panel_->refresh();
	std::vector<RuntimeView> views = st.mgr->views();

	std::string banner = st.mgr->bottleneck_banner();
	banner_->setVisible(!banner.empty());
	banner_->setText(qs(banner));
	notice_->setVisible(!st.notice.empty());
	notice_->setText(qs(st.notice));
	const std::vector<std::string> &dw = st.mgr->double_stream_warnings();
	QString dtext;
	for (size_t i = 0; i < dw.size(); ++i)
		dtext += (i ? "\n" : "") + qs(dw[i]);
	double_->setVisible(!dw.empty());
	double_->setText(dtext);
	for (int i = 0; i < 2; ++i) {
		QString u = overlay_url(i);
		ov_url_[i]->setText(u.isEmpty() ? T("Msg.NoServer") : u);
	}
	update_setup(st.cfg);
	streams_empty_->setVisible(views.empty());
	bool any_live = false;
	for (size_t i = 0; i < views.size(); ++i)
		any_live = any_live || views[i].state != State::Idle;
	for (int c = 5; c <= 7; ++c)
		table_->setColumnHidden(c, !any_live);
	table_->setVisible(!views.empty());
	big_btn_->setVisible(!views.empty());
	glance_->setVisible(!views.empty() || plugin_reader_running());

	update_glance(views);

	std::vector<std::string> ids;
	for (size_t i = 0; i < views.size(); ++i)
		ids.push_back(views[i].dest.id);
	if (ids != row_ids_)
		rebuild_rows(ids);

	for (size_t i = 0; i < views.size(); ++i) {
		const RuntimeView &v = views[i];
		int row = static_cast<int>(i);
		QCheckBox *cb = qobject_cast<QCheckBox *>(table_->cellWidget(row, 0));
		if (cb && cb->isChecked() != v.dest.enabled) {
			bool prev = cb->blockSignals(true);
			cb->setChecked(v.dest.enabled);
			cb->blockSignals(prev);
		}
		QTableWidgetItem *status = table_->item(row, 1);
		status->setText(QString::fromUtf8("\xE2\x97\x8F ") + state_label(v));
		status->setForeground(QColor(dot_color(v.state)));
		QString tip = qs(v.detail);
		if (!v.notice.empty())
			tip += (tip.isEmpty() ? "" : "\n") + qs(v.notice);
		if (!v.last_error.empty())
			tip += (tip.isEmpty() ? "" : "\n") + qs(v.last_error);
		status->setToolTip(tip);

		QString display_name = qs(v.dest.name);
		if (!v.dest.canvas.empty())
			display_name += " [" + qs(v.dest.canvas) + "]";
		table_->item(row, 2)->setText(display_name);
		table_->item(row, 2)->setIcon(QIcon(QPixmap::fromImage(create_platform_icon(v.dest.platform, 20))));
		table_->item(row, 2)->setToolTip(source_tooltip(v.dest));

		table_->item(row, 3)->setText(qs(v.dest.handle));
		bool live = v.state == State::Live || v.state == State::Degraded;

		QComboBox *track_cb = qobject_cast<QComboBox *>(table_->cellWidget(row, 4));
		if (track_cb) {
			int idx = track_cb->findData(v.dest.audio_track);
			if (idx >= 0 && track_cb->currentIndex() != idx) {
				bool prev = track_cb->blockSignals(true);
				track_cb->setCurrentIndex(idx);
				track_cb->blockSignals(prev);
			}
			track_cb->setEnabled(!live);
			track_cb->setToolTip(live ? T("Dialog.TrackLocked")
						  : qs(track_label(v.dest.audio_track, plugin().mgr->track_name(v.dest.audio_track))));
		}

		table_->item(row, 5)->setText(live ? QString("%1 kbps").arg(static_cast<int>(v.bitrate_kbps)) : "-");
		table_->item(row, 6)->setText(live ? QString("%1 %").arg(v.dropped_pct, 0, 'f', 1) : "-");
		table_->item(row, 7)->setText(fmt_uptime(v.uptime_ms));
		QPushButton *btn = qobject_cast<QPushButton *>(table_->cellWidget(row, 8));
		if (btn) {
			bool stop = v.can_stop;
			btn->setProperty("act", stop ? "stop" : "start");
			btn->setText(stop ? T("Btn.Stop") : T("Btn.Start"));
			btn->setEnabled(stop || v.can_start);
		}
	}
}

void MultistreamDock::show_audio_tutorial()
{
	QDialog dlg(this);
	dlg.setWindowTitle("Audio tracks per destination");
	dlg.resize(760, 620);
	QVBoxLayout *l = new QVBoxLayout(&dlg);
	QTextBrowser *tb = new QTextBrowser(&dlg);
	tb->setOpenExternalLinks(true);
	tb->setStyleSheet("background:#18181f; color:#e0e0e8; font-family:Segoe UI, sans-serif; font-size:13px; line-height:1.5; padding:12px;");

	QString html = R"HTML(
	<h2 style="color:#58a6ff; margin-top:0;">Audio tracks per destination</h2>
	<p>How to send a different sound mix to each platform (for example: no music on YouTube).</p>

	<div style="background:#20202c; border-left:4px solid #58a6ff; padding:10px; margin-bottom:14px; border-radius:4px;">
		<b style="color:#ffffff;">Quick track choice:</b> each row of the Streams table has an audio-track list; change it there without opening a dialog.
	</div>

	<h3 style="color:#3ddc84;">1. How Audio Routing Works in OBS</h3>
	<p>OBS has <b>6 internal audio tracks</b>. By default, your main stream broadcasts Track 1. With this plugin, every destination can broadcast a completely different audio track!</p>
	<ul>
		<li><b>Main (Default):</b> Broadcasts the exact audio track that your primary OBS stream is sending.</li>
		<li><b>Track 1 to Track 6:</b> Broadcasts an independent audio mix with its own dedicated AAC encoder.</li>
	</ul>

	<h3 style="color:#3ddc84;">2. Setting Up Independent Audio in OBS (Advanced Audio Properties)</h3>
	<p>To control which sounds go to which platform:</p>
	<ol>
		<li>In the OBS main window, click the <b>Edit</b> menu at the top &gt; <b>Advanced Audio Properties</b> (or click the gear/3-dots on any Audio Mixer fader &gt; Advanced Audio Properties).</li>
		<li>Look at the <b>Tracks (1 2 3 4 5 6)</b> checkboxes on the right side of the window:</li>
	</ol>

	<table border="1" cellpadding="6" cellspacing="0" style="border-collapse:collapse; border-color:#384259; width:100%; margin:10px 0; background:#1c1c26;">
		<tr style="background:#282c3c; color:#ffffff;">
			<th>Audio Source</th>
			<th>Track 1 (e.g. Twitch)</th>
			<th>Track 2 (e.g. YouTube VOD)</th>
			<th>Track 3 (e.g. TikTok)</th>
			<th>Track 4 (e.g. Kick)</th>
		</tr>
		<tr>
			<td><b>Microphone</b></td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
		</tr>
		<tr>
			<td><b>Game Audio</b></td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
		</tr>
		<tr>
			<td><b>Music / Spotify</b></td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#ff6b6b;">&#10008; Unchecked (No DMCA!)</td>
			<td align="center" style="color:#ff6b6b;">&#10008; Unchecked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
		</tr>
		<tr>
			<td><b>Discord / Voice Chat</b></td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#ff6b6b;">&#10008; Unchecked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
			<td align="center" style="color:#3ddc84;">&#10004; Checked</td>
		</tr>
	</table>

	<h3 style="color:#3ddc84;">3. Naming Your Audio Tracks in OBS</h3>
	<p>Want the dropdown to show friendly names like <i>"Track 2 (No Music)"</i> instead of just numbers?</p>
	<ol>
		<li>In OBS, open <b>Settings &gt; Output</b>.</li>
		<li>Set <i>Output Mode</i> to <b>Advanced</b>.</li>
		<li>Under the <i>Audio</i> tab, name Tracks 1 to 6 (for example "Twitch Mix", "No music", "Voice only").</li>
		<li>The Streams tab shows those names in its track lists.</li>
	</ol>

		<h3 style="color:#3ddc84;">4. Where each destination gets its sound</h3>
		<p>Streams tab &gt; select a destination &gt; <b>Edit</b> &gt; <b>Advanced options</b> &gt; <i>Audio track</i>, or use the
		track list right in the Streams table. A destination that is live keeps its track until it stops.</p>

		<h3 style="color:#3ddc84;">5. Vertical video (TikTok)</h3>
		<p>If Aitum Stream Suite (or another plugin) adds a vertical canvas, pick it under <b>Edit &gt; Advanced options &gt;
		Video source</b>. That destination then gets its own encoder at the vertical size while the main stream stays
		horizontal. TikTok destinations pick a canvas named "vertical" by themselves.</p>
	)HTML";

	tb->setHtml(html);
	l->addWidget(tb, 1);
	QHBoxLayout *bl = new QHBoxLayout();
	bl->addStretch(1);
	QPushButton *close_btn = new QPushButton("Close", &dlg);
	close_btn->setStyleSheet("padding:6px 18px; font-weight:bold; background:#2f7d4f; color:white; border-radius:4px;");
	QObject::connect(close_btn, &QPushButton::clicked, &dlg, &QDialog::accept);
	bl->addWidget(close_btn);
	l->addLayout(bl);
	dlg.exec();
}

// README step pictures (OMSC_SHOTS=<dir>, see plugin-main.cpp). Renders the real dock and dialogs offscreen
// (WA_DontShowOnScreen), page by page, in the order a first-time user goes through them. With `demo` the
// destinations and chat channels are sample data held in memory only: nothing here saves the config, and
// the config is put back at the end. Sample chat lines go through the real hub (ingest + flush).
void MultistreamDock::capture_screens(const QString &dir, bool demo)
{
	QDir().mkpath(dir);
	PluginState &st = plugin();
	const Config saved = st.cfg;
	const bool saved_overlay = ui_flag("overlay_copied"), saved_hidden = ui_flag("setup_hidden");
	const QSize dock_size(460, 860);
	auto settle = []() {
		for (int i = 0; i < 6; ++i)
			QApplication::processEvents();
	};
	auto shot = [&](QWidget *w, const char *name) {
		settle();
		w->grab().save(dir + "/" + name + ".png");
		blog(LOG_INFO, "[obs-multistream-chat] shots: %s", name);
	};
	auto make_dock = [&]() {
		MultistreamDock *d = new MultistreamDock(nullptr);
		d->setAttribute(Qt::WA_DontShowOnScreen);
		d->resize(dock_size);
		d->show();
		d->refresh();
		return d;
	};
	auto tab = [&](MultistreamDock *d, int idx, const char *name) {
		d->tabs_->setCurrentIndex(idx);
		d->refresh();
		shot(d, name);
	};

	// 1. first run: nothing set up yet
	st.cfg.destinations.clear();
	st.cfg.chat.sources.clear();
	set_ui_flag("overlay_copied", false);
	set_ui_flag("setup_hidden", false);
	st.mgr->sync_with_config();
	MultistreamDock *d = make_dock();
	tab(d, d->tab_streams_, "01-first-run");

	// 2-3. the add dialog: basic, then with Advanced open
	{
		Destination nd;
		nd.id = "shot";
		nd.platform = "twitch";
		nd.url = find_preset("twitch")->default_url;
		DestinationDialog dlg(nullptr, nd, false);
		dlg.setAttribute(Qt::WA_DontShowOnScreen);
		dlg.show();
		dlg.fill_for_shot("otiyyy");
		shot(&dlg, "02-add-destination");
		dlg.open_advanced();
		dlg.resize(760, 1000);
		shot(&dlg, "03-add-destination-advanced");
		dlg.hide();
	}

	if (demo) {
		const char *plats[3][3] = {{"twitch", "Twitch", "otiyyy"}, {"youtube", "YouTube", "otiyyy"}, {"kick", "Kick", "otiyyy"}};
		for (int i = 0; i < 3; ++i) {
			Destination x;
			x.id = std::string("demo") + std::to_string(i + 1);
			x.platform = plats[i][0];
			x.name = plats[i][1];
			x.handle = plats[i][2];
			x.url = find_preset(x.platform)->default_url;
			if (x.url.empty())
				x.url = "rtmps://example.invalid/app";
			st.cfg.destinations.push_back(x);
		}
		st.mgr->sync_with_config();
	}
	tab(d, d->tab_streams_, "04-streams");

	if (demo) {
		std::string t, err;
		add_chat_source(st.cfg.chat.sources, "twitch", "otiyyy", t, err);
		add_chat_source(st.cfg.chat.sources, "kick", "otiyyy", t, err);
		add_chat_source(st.cfg.chat.sources, "twitch", "collab_friend", t, err);
	}
	tab(d, d->tab_sources_, "05-chat-sources");
	tab(d, d->tab_overlays_, "06-overlays");
	set_ui_flag("overlay_copied", true);

	if (demo) {
		const char *lines[][4] = {{"twitch", "c1", "SampleViewer", "that save was insane"},
					  {"kick", "c2", "KickFan", "hello from Kick!"},
					  {"twitch", "c3", "collab_mod", "gg, both chats in one place"},
					  {"twitch", "c1", "NightOwl", "what settings do you use?"},
					  {"kick", "c2", "SampleUser", "W stream"}};
		std::string body = "{\"v\":1,\"messages\":[";
		for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) {
			body += std::string(i ? "," : "") + "{\"platform\":\"" + lines[i][0] + "\",\"source\":\"" + lines[i][1] + "\",\"id\":\"shot" +
				std::to_string(i) + "\",\"author\":\"" + lines[i][2] + "\",\"parts\":[{\"t\":\"text\",\"v\":\"" + lines[i][3] + "\"}]}";
		}
		body += "]}";
		st.chat_hub.ingest(body);
		QThread::msleep(2500);
		st.chat_hub.flush();
	}
	delete d;
	d = make_dock(); // a new dock loads the hub's backlog, like OBS starting with chat already flowing
	tab(d, d->tab_chat_, "07-chat");
	tab(d, d->tab_voice_, "08-voice");
	delete d;

	st.cfg = saved;
	st.mgr->sync_with_config();
	set_ui_flag("overlay_copied", saved_overlay);
	set_ui_flag("setup_hidden", saved_hidden);
	QFile f(dir + "/done.txt");
	if (f.open(QIODevice::WriteOnly))
		f.write("ok\n");
}

} // namespace oms
