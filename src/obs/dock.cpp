#include "dock.hpp"
#include "chat_panel.hpp"
#include "aitum.hpp"
#include "destination.hpp"
#include "plan.hpp"
#include "plugin.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
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

		name_ = new QLineEdit(qs(d.name));
		name_->setMaxLength(64);
		form->addRow(T("Dialog.Name"), name_);

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

		// ---- video source -------------------------------------------------------------------
		form->addRow(heading(T("Dialog.SecVideo")));
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
		form->addRow(T("Dialog.Canvas"), canvas_);

		encoder_ = new QComboBox();
		encoder_->addItem(T("Dialog.EncShared"), "shared");
		encoder_->addItem(T("Dialog.EncOwn"), "own");
		encoder_->setCurrentIndex(d.encoder == "own" ? 1 : 0);
		form->addRow(T("Dialog.Encoder"), encoder_);

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
		form->addRow(T("Dialog.VideoEncoder"), venc_);

		bitrate_ = new QSpinBox();
		bitrate_->setRange(300, 51000);
		bitrate_->setSuffix(" kbps");
		bitrate_->setValue(d.bitrate_kbps >= 300 ? d.bitrate_kbps : 4500);
		form->addRow(T("Dialog.Bitrate"), bitrate_);

		keyint_ = new QSpinBox();
		keyint_->setRange(0, 10);
		keyint_->setSuffix(" s");
		keyint_->setSpecialValueText(T("Dialog.Default"));
		keyint_->setValue(d.keyint_sec);
		form->addRow(T("Dialog.Keyint"), keyint_);

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
		form->addRow(QString(), pw);

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
		form->addRow(T("Dialog.Rescale"), rw);

		video_note_ = new QLabel();
		video_note_->setWordWrap(true);
		video_note_->setStyleSheet("color: #8a8a96;");
		form->addRow(QString(), video_note_);

		// ---- audio ----------------------------------------------------------------------------
		form->addRow(heading(T("Dialog.SecAudio")));
		track_ = new QComboBox();
		track_->addItem(T("Dialog.TrackMain"), 0);
		for (int t = 1; t <= kMaxAudioTracks; ++t)
			track_->addItem(qs(track_label(t, mgr.track_name(t))), t);
		track_->setCurrentIndex(std::max(0, std::min(kMaxAudioTracks, d.audio_track)));
		form->addRow(T("Dialog.Track"), track_);
		abitrate_ = new QSpinBox();
		abitrate_->setRange(64, 320);
		abitrate_->setSuffix(" kbps");
		abitrate_->setValue(d.audio_bitrate_kbps);
		form->addRow(T("Dialog.AudioBitrate"), abitrate_);
		audio_note_ = new QLabel();
		audio_note_->setWordWrap(true);
		audio_note_->setStyleSheet("color: #8a8a96;");
		form->addRow(QString(), audio_note_);

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
		form->addRow(T("Dialog.Delay"), dw);

		// ---- Aitum ----------------------------------------------------------------------------
		form->addRow(heading(T("Dialog.SecAitum")));
		aitum_mode_ = new QComboBox();
		aitum_mode_->addItem(T("Dialog.AitumOff"), "off");
		aitum_mode_->addItem(T("Dialog.AitumCopy"), "copy");
		aitum_mode_->addItem(T("Dialog.AitumFollow"), "follow");
		aitum_mode_->setCurrentIndex(d.aitum_mode == "follow" ? 2 : d.aitum_mode == "copy" ? 1 : 0);
		form->addRow(T("Dialog.AitumMode"), aitum_mode_);
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
		form->addRow(T("Dialog.AitumOutput"), aw);
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
		form->addRow(QString(), aitum_note_);
		QObject::connect(import_btn_, &QPushButton::clicked, [this]() { import_from_aitum(); });

		enabled_ = new QCheckBox(T("Dialog.Enabled"));
		enabled_->setChecked(d.enabled);
		auto_ = new QCheckBox(T("Dialog.AutoStart"));
		auto_->setChecked(d.auto_start);
		form->addRow(QString(), enabled_);
		form->addRow(QString(), auto_);

		error_ = new QLabel();
		error_->setWordWrap(true);
		error_->setStyleSheet("color: #ff4d4f;");
		error_->hide();

		QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		QObject::connect(bb, &QDialogButtonBox::rejected, [this]() { reject(); });
		QObject::connect(bb, &QDialogButtonBox::accepted, [this]() { try_accept(); });

		QWidget *body = new QWidget();
		body->setLayout(form);
		QScrollArea *scroll = new QScrollArea();
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidget(body);
		QVBoxLayout *root = new QVBoxLayout(this);
		root->addWidget(scroll, 1);
		root->addWidget(error_);
		root->addWidget(bb);
		resize(560, 720);

		prev_default_ = default_url_for(current_platform());
		QObject::connect(platform_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { on_platform_changed(); });
		QObject::connect(encoder_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		QObject::connect(canvas_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		QObject::connect(track_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		QObject::connect(aitum_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		update_hint();
		update_enabled();
	}

	Destination result() const { return dest_; }

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
	QComboBox *platform_;
	QLineEdit *name_, *url_, *key_, *handle_, *preset_, *profile_;
	QLabel *hint_, *error_, *video_note_, *audio_note_, *aitum_note_;
	QPushButton *reveal_, *import_btn_;
	QComboBox *encoder_, *canvas_, *venc_, *rate_, *fps_, *track_, *aitum_mode_, *aitum_output_;
	QSpinBox *bitrate_, *keyint_, *width_, *height_, *abitrate_, *delay_;
	QCheckBox *enabled_, *auto_, *delay_keep_;
};

} // namespace

// ---------------------------------------------------------------------------------------------

MultistreamDock::MultistreamDock(QWidget *parent) : QWidget(parent)
{
	QTabWidget *tabs = new QTabWidget();
	QWidget *streams_page = new QWidget();
	chat_panel_ = new ChatPanel(this);
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->addWidget(tabs);
	tabs->addTab(streams_page, T("Tab.Streams"));
	tabs->addTab(chat_panel_, T("Tab.Chat"));
	QVBoxLayout *root = new QVBoxLayout(streams_page);
	root->setContentsMargins(6, 6, 6, 6);

	banner_ = new QLabel();
	banner_->setWordWrap(true);
	banner_->setStyleSheet("background:#5a3a00; color:#ffd58a; padding:6px; border-radius:4px;");
	banner_->hide();
	root->addWidget(banner_);

	notice_ = new QLabel();
	notice_->setWordWrap(true);
	notice_->setStyleSheet("background:#3a2530; color:#ffb3c1; padding:6px; border-radius:4px;");
	notice_->hide();
	root->addWidget(notice_);

	double_ = new QLabel();
	double_->setWordWrap(true);
	double_->setStyleSheet("background:#5a1f1f; color:#ffc9c9; padding:6px; border-radius:4px;");
	double_->hide();
	root->addWidget(double_);

	// at-a-glance strip for use DURING a stream: one big button and one summary line
	big_btn_ = new QPushButton();
	big_btn_->setMinimumHeight(40);
	big_btn_->setStyleSheet("font-size:15px; font-weight:bold;");
	glance_ = new QLabel();
	glance_->setWordWrap(true);
	root->addWidget(big_btn_);
	root->addWidget(glance_);

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
	table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
	root->addWidget(table_, 1);

	QHBoxLayout *row1 = new QHBoxLayout();
	QPushButton *add = new QPushButton(T("Btn.Add"));
	edit_btn_ = new QPushButton(T("Btn.Edit"));
	remove_btn_ = new QPushButton(T("Btn.Remove"));
	QPushButton *guide_btn = new QPushButton(QString::fromUtf8("? Audio & Setup Guide"));
	guide_btn->setStyleSheet("background:#232733; color:#58a6ff; font-weight:bold; border:1px solid #384259; border-radius:3px; padding:3px 10px;");
	row1->addWidget(add);
	row1->addWidget(edit_btn_);
	row1->addWidget(remove_btn_);
	row1->addStretch(1);
	row1->addWidget(guide_btn);
	root->addLayout(row1);

	QHBoxLayout *row2 = new QHBoxLayout();
	url_label_ = new QLabel();
	url_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	url_label_->setStyleSheet("color:#8a8a96;");
	QPushButton *copy = new QPushButton(T("Btn.CopyUrl"));
	row2->addWidget(url_label_, 1);
	row2->addWidget(copy);
	root->addLayout(row2);

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
	QObject::connect(copy, &QPushButton::clicked, [this]() { copy_overlay_url(); });
	QObject::connect(table_, &QTableWidget::itemSelectionChanged, [this]() {
		bool any = !selected_id().empty();
		edit_btn_->setEnabled(any);
		remove_btn_->setEnabled(any);
	});
	edit_btn_->setEnabled(false);
	remove_btn_->setEnabled(false);
	refresh();
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

void MultistreamDock::copy_overlay_url()
{
	std::string url = plugin_overlay_url();
	if (url.empty()) {
		QMessageBox::warning(this, T("Dock.Title"), T("Msg.NoServer"));
		return;
	}
	QApplication::clipboard()->setText(qs(url + "?theme=dark"));
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
	std::string url = plugin_overlay_url();
	url_label_->setText(url.empty() ? T("Msg.NoServer") : qs(url));

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
	dlg.setWindowTitle("OBS Multi-Stream & Audio Routing Tutorial");
	dlg.resize(760, 620);
	QVBoxLayout *l = new QVBoxLayout(&dlg);
	QTextBrowser *tb = new QTextBrowser(&dlg);
	tb->setOpenExternalLinks(true);
	tb->setStyleSheet("background:#18181f; color:#e0e0e8; font-family:Segoe UI, sans-serif; font-size:13px; line-height:1.5; padding:12px;");

	QString html = R"HTML(
	<h2 style="color:#58a6ff; margin-top:0;">Multi-Stream & Audio Routing Tutorial</h2>
	<p>Welcome! This guide explains how to stream to multiple platforms simultaneously with customized audio tracks and video resolutions.</p>

	<div style="background:#20202c; border-left:4px solid #58a6ff; padding:10px; margin-bottom:14px; border-radius:4px;">
		<b style="color:#ffffff;">Quick Track Selector:</b> You can change any stream's audio track instantly using the <b>Audio Track dropdown</b> in the Streams table row with zero dialogs!
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
		<li>Under the <i>Recording</i> or <i>Audio</i> tab, you can name Tracks 1 through 6 (e.g., "Twitch Mix", "No-Copyright", "Voice Only").</li>
		<li>Our Streams dock automatically reads these names from your OBS profile and displays them in the dropdown!</li>
	</ol>

	<h3 style="color:#3ddc84;">4. Adding Streaming Platforms</h3>
	<p>Click <b>Add stream</b> below to add any platform:</p>
	<ul>
		<li><b>Twitch:</b> Select Twitch preset &gt; paste Stream Key from Twitch Dashboard &gt; Settings &gt; Stream.</li>
		<li><b>Kick:</b> Select Kick preset &gt; paste Stream Key from Kick Creator Dashboard &gt; Settings &gt; Stream URL &amp; Key.</li>
		<li><b>YouTube:</b> Select YouTube preset &gt; paste Stream Key from YouTube Studio &gt; Go Live.</li>
		<li><b>TikTok:</b> Select TikTok preset &gt; paste Server URL and Stream Key from TikTok LIVE Studio or Live access.</li>
		<li><b>Instagram Live:</b> Select Instagram preset &gt; paste Stream URL and Key from Instagram Live Producer.</li>
		<li><b>X (Twitter):</b> Select X preset &gt; paste Stream Key from X Media Studio / Live Producer.</li>
		<li><b>Restream / Custom:</b> Pre-configured RTMP endpoints for multi-broadcasting.</li>
	</ul>

	<h3 style="color:#3ddc84;">5. Vertical Video &amp; Multi-Canvas (TikTok / Instagram)</h3>
	<p>If you use Aitum Stream Suite or the OBS Vertical Canvas plugin, you can select that canvas in <b>Edit &gt; Canvas</b>. The destination will automatically encode at vertical 1080x1920 while your main stream remains horizontal 1920x1080!</p>
	)HTML";

	tb->setHtml(html);
	l->addWidget(tb, 1);
	QHBoxLayout *bl = new QHBoxLayout();
	bl->addStretch(1);
	QPushButton *close_btn = new QPushButton("Got it, Close", &dlg);
	close_btn->setStyleSheet("padding:6px 18px; font-weight:bold; background:#2f7d4f; color:white; border-radius:4px;");
	QObject::connect(close_btn, &QPushButton::clicked, &dlg, &QDialog::accept);
	bl->addWidget(close_btn);
	l->addLayout(bl);
	dlg.exec();
}

} // namespace oms
