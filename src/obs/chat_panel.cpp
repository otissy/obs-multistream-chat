#include "chat_panel.hpp"
#include "setup_flow.hpp"
#include "chat_targets.hpp"
#include "plugin.hpp"

#include <obs-module.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <wininet.h>
#endif

#include <cmath>
#include <thread>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QTableWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace oms {

namespace {

QString qs(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}
std::string ss(const QString &q)
{
	return q.toUtf8().toStdString();
}

struct PlatformBadge {
	const char *label;
	const char *bg;
	const char *fg;
};

PlatformBadge badge_for(const std::string &platform)
{
	if (platform == "twitch")
		return {"Twitch", "#9146ff", "#ffffff"};
	if (platform == "youtube")
		return {"YouTube", "#ff0000", "#ffffff"};
	if (platform == "kick")
		return {"Kick", "#53fc18", "#121212"};
	if (platform == "tiktok")
		return {"TikTok", "#25f4ee", "#121212"};
	if (platform == "x")
		return {"X", "#f2f2f5", "#121212"};
	if (platform == "facebook")
		return {"Facebook", "#1877f2", "#ffffff"};
	if (platform == "instagram")
		return {"Instagram", "#e1306c", "#ffffff"};
	if (platform == "restream")
		return {"Restream", "#0070f3", "#ffffff"};
	if (platform == "linkedin")
		return {"LinkedIn", "#0a66c2", "#ffffff"};
	return {"Chat", "#8a8a96", "#ffffff"};
}

} // namespace

QImage create_platform_icon(const std::string &platform, int sz)
{
	QImage img(sz, sz, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing, true);

	qreal s = sz / 24.0;
	p.scale(s, s);

	if (platform == "twitch") {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#9146ff"));
		p.drawRoundedRect(QRectF(1, 1, 22, 22), 5, 5);

		QPainterPath path;
		path.moveTo(5, 4);
		path.lineTo(19, 4);
		path.lineTo(19, 14);
		path.lineTo(16, 17);
		path.lineTo(13, 17);
		path.lineTo(11, 19);
		path.lineTo(11, 17);
		path.lineTo(5, 17);
		path.closeSubpath();
		p.setBrush(Qt::white);
		p.drawPath(path);

		p.setBrush(QColor("#9146ff"));
		p.drawRect(QRectF(9.5, 7.5, 2, 5));
		p.drawRect(QRectF(14.5, 7.5, 2, 5));
	} else if (platform == "youtube") {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#ff0000"));
		p.drawRoundedRect(QRectF(1, 3, 22, 18), 5.5, 5.5);

		QPolygonF tri;
		tri << QPointF(9.5, 7.5) << QPointF(16.5, 12.0) << QPointF(9.5, 16.5);
		p.setBrush(Qt::white);
		p.drawPolygon(tri);
	} else if (platform == "kick") {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#53fc18"));
		p.drawRoundedRect(QRectF(1, 1, 22, 22), 5, 5);

		p.setBrush(QColor("#000000"));
		p.drawRect(QRectF(5, 5, 4, 14));
		p.drawRect(QRectF(14, 5, 5, 4.5));
		p.drawRect(QRectF(9, 9.5, 5, 5));
		p.drawRect(QRectF(14, 14.5, 5, 4.5));
	} else if (platform == "tiktok") {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#16161c"));
		p.drawRoundedRect(QRectF(1, 1, 22, 22), 5, 5);

		p.setPen(QPen(QColor("#25f4ee"), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.drawLine(QPointF(13, 5), QPointF(13, 15));
		p.drawLine(QPointF(13, 5), QPointF(17, 7));

		p.setPen(QPen(QColor("#fe2c55"), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.drawLine(QPointF(12, 6), QPointF(12, 16));
		p.drawLine(QPointF(12, 6), QPointF(16, 8));

		p.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.drawLine(QPointF(12.5, 5.5), QPointF(12.5, 15.5));
		p.drawLine(QPointF(12.5, 5.5), QPointF(16.5, 7.5));

		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#25f4ee"));
		p.drawEllipse(QRectF(8, 13, 7, 6));
		p.setBrush(QColor("#fe2c55"));
		p.drawEllipse(QRectF(7, 14, 7, 6));
		p.setBrush(Qt::white);
		p.drawEllipse(QRectF(7.5, 13.5, 6.5, 5.5));
	} else if (platform == "x") {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#1e1e24"));
		p.drawRoundedRect(QRectF(1, 1, 22, 22), 5, 5);

		p.setPen(QPen(Qt::white, 2.2, Qt::SolidLine, Qt::SquareCap));
		p.drawLine(QPointF(6, 6), QPointF(18, 18));
		p.drawLine(QPointF(18, 6), QPointF(6, 18));
	} else if (platform == "facebook") {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#1877f2"));
		p.drawRoundedRect(QRectF(1, 1, 22, 22), 5, 5);

		p.setPen(QPen(Qt::white, 2.5, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(13, 20), QPointF(13, 10));
		p.drawLine(QPointF(13, 10), QPointF(16, 7));
		p.drawLine(QPointF(11, 13), QPointF(15, 13));
	} else if (platform == "instagram") {
		p.setPen(Qt::NoPen);
		QLinearGradient grad(2, 22, 22, 2);
		grad.setColorAt(0.0, QColor("#f09433"));
		grad.setColorAt(0.3, QColor("#e6683c"));
		grad.setColorAt(0.6, QColor("#dc2743"));
		grad.setColorAt(0.8, QColor("#cc2366"));
		grad.setColorAt(1.0, QColor("#bc1888"));
		p.setBrush(grad);
		p.drawRoundedRect(QRectF(1, 1, 22, 22), 6, 6);

		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(Qt::white, 1.8));
		p.drawRoundedRect(QRectF(5, 5, 14, 14), 4, 4);
		p.drawEllipse(QPointF(12, 12), 3.5, 3.5);
		p.setBrush(Qt::white);
		p.setPen(Qt::NoPen);
		p.drawEllipse(QRectF(15, 6.5, 1.8, 1.8));
	} else {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#3a3a46"));
		p.drawRoundedRect(QRectF(1, 1, 22, 22), 5, 5);
		p.setBrush(Qt::white);
		QPainterPath path;
		path.moveTo(6, 6);
		path.lineTo(18, 6);
		path.lineTo(18, 14);
		path.lineTo(12, 14);
		path.lineTo(9, 17);
		path.lineTo(9, 14);
		path.lineTo(6, 14);
		path.closeSubpath();
		p.drawPath(path);
	}

	p.end();
	return img;
}

namespace {

QString hash_color(const std::string &name)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < name.size(); ++i) {
		h ^= static_cast<unsigned char>(name[i]);
		h *= 16777619u;
	}
	int hue = static_cast<int>((h ^ (h >> 16)) % 360);
	QColor c = QColor::fromHsl(hue, 185, 175);
	return c.name();
}

QString author_color(const ChatMessage &m)
{
	if (valid_chat_color(m.color)) {
		QColor c(qs(m.color));
		if (c.isValid()) {
			if (c.lightness() < 125)
				c = c.lighter(160);
			return c.name();
		}
	}
	return hash_color(m.author);
}

struct StateLook {
	const char *label;
	const char *color;
};

StateLook look_for(const std::string &state)
{
	if (state == "reading")
		return {"Reading", "#3ddc84"};
	if (state == "waiting")
		return {"Waiting for live stream (auto-detecting)", "#38bdf8"};
	if (state == "starting")
		return {"Connecting / checking live...", "#ffb020"};
	if (state == "quiet")
		return {"Quiet: no messages yet", "#ffb020"};
	if (state == "layout")
		return {"Page layout changed or login needed", "#ff4d4f"};
	if (state == "login")
		return {"Needs login", "#ff4d4f"};
	if (state == "error")
		return {"Error", "#ff4d4f"};
	if (state == "stub")
		return {"Empty page served (automation detection or unavailable stream)", "#ff4d4f"};
	if (state == "consent")
		return {"Blocked by the platform's cookie-consent page", "#ff4d4f"};
	if (state == "disabled")
		return {"Chat is disabled for this stream", "#ffb020"};
	if (state == "replay")
		return {"Replay only: not a live stream", "#ffb020"};
	if (state == "offline")
		return {"Channel offline (no messages)", "#ffb020"};
	if (state == "starting")
		return {"Starting", "#ffb020"};
	return {"Stopped", "#8a8a96"};
}

class SourceDialog : public QDialog {
public:
	SourceDialog(QWidget *parent, const ChatSource &s, bool editing) : QDialog(parent), src_(s)
	{
		setWindowTitle(editing ? "Edit chat source" : "Add chat source");
		setMinimumWidth(460);
		QFormLayout *form = new QFormLayout();
		platform_ = new QComboBox();
		const std::vector<ChatPlatform> &pl = chat_platforms();
		for (size_t i = 0; i < pl.size(); ++i)
			platform_->addItem(QIcon(QPixmap::fromImage(create_platform_icon(pl[i].id, 20))), qs(pl[i].label), qs(pl[i].id));
		int idx = platform_->findData(qs(s.platform.empty() ? "twitch" : s.platform));
		platform_->setCurrentIndex(idx < 0 ? 0 : idx);
		form->addRow("Platform", platform_);
		target_ = new QLineEdit(qs(s.target));
		target_->setMaxLength(300);
		form->addRow("Channel, video or link", target_);
		hint_ = new QLabel();
		hint_->setWordWrap(true);
		hint_->setStyleSheet("color: #8a8a96;");
		form->addRow(QString(), hint_);
		enabled_ = new QCheckBox("Enabled");
		enabled_->setChecked(s.enabled);
		form->addRow(QString(), enabled_);
		error_ = new QLabel();
		error_->setWordWrap(true);
		error_->setStyleSheet("color: #ff4d4f;");
		error_->hide();
		QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
		QObject::connect(bb, &QDialogButtonBox::rejected, [this]() { reject(); });
		QObject::connect(bb, &QDialogButtonBox::accepted, [this]() { try_accept(); });
		QVBoxLayout *root = new QVBoxLayout(this);
		root->addLayout(form);
		root->addWidget(error_);
		root->addWidget(bb);
		QObject::connect(platform_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_hint(); });
		update_hint();
	}
	ChatSource result() const { return src_; }

private:
	void update_hint()
	{
		const ChatPlatform *p = find_chat_platform(ss(platform_->currentData().toString()));
		QString t = p ? qs(p->hint) : QString();
		if (p && p->experimental)
			t += "\nEXPERIMENTAL: this platform may need a login or block automated browsers. Nothing is ever logged in.";
		hint_->setText(t);
	}
	void try_accept()
	{
		std::string platform = ss(platform_->currentData().toString());
		std::string target, err;
		if (!normalize_chat_target(platform, ss(target_->text()), target, err)) {
			error_->setText(qs(err));
			error_->show();
			return;
		}
		src_.platform = platform;
		src_.target = target;
		src_.enabled = enabled_->isChecked();
		accept();
	}
	ChatSource src_;
	QComboBox *platform_;
	QLineEdit *target_;
	QLabel *hint_, *error_;
	QCheckBox *enabled_;
};

// "Twitch", "TikTok" (the dock label without "(experimental)")
QString platform_name(const std::string &id)
{
	const ChatPlatform *p = find_chat_platform(id);
	QString l = p ? qs(p->label) : qs(id);
	int cut = l.indexOf(" (");
	return cut > 0 ? l.left(cut) : l;
}

// The Voice tab is kept in ui-state.ini next to config.json (per user, not part of the config schema).
QString voice_state_path()
{
	char *p = obs_module_config_path("ui-state.ini");
	QString s = p ? QString::fromUtf8(p) : QString();
	bfree(p);
	return s;
}

std::vector<std::string> lines_of(const QString &text)
{
	std::vector<std::string> out;
	const QStringList l = text.split('\n');
	for (int i = 0; i < l.size() && out.size() < kMaxChatListEntries; ++i) {
		std::string e = ss(l[i].trimmed());
		if (!e.empty() && e.size() <= 60)
			out.push_back(e);
	}
	return out;
}

QString join_lines(const std::vector<std::string> &v)
{
	QString o;
	for (size_t i = 0; i < v.size(); ++i)
		o += qs(v[i]) + "\n";
	return o;
}

} // namespace

ChatPanel::ChatPanel(QWidget *parent) : QWidget(parent)
{
	// Since 0.5.0 this widget only owns the pages and timers: the dock puts the pages in its own tab bar
	// (Chat, Chat sources, Voice) and the version / update line in its footer.
	hide();

	// --- Chat: the live merged feed ---------------------------------------------------------------
	live_page_ = new QWidget();
	QVBoxLayout *ll = new QVBoxLayout(live_page_);
	ll->setContentsMargins(6, 6, 6, 6);

	QHBoxLayout *top_live = new QHBoxLayout();
	live_reader_status_ = new QLabel("Reader stopped");
	live_reader_status_->setStyleSheet("color:#8a8a96; font-weight:bold;");
	live_reader_btn_ = new QPushButton("Start reader");
	live_reader_btn_->setMinimumHeight(26);

	font_smaller_btn_ = new QPushButton("A-");
	font_smaller_btn_->setFixedWidth(38);
	font_smaller_btn_->setStyleSheet("padding:2px 0;");
	font_smaller_btn_->setToolTip("Smaller text");
	font_size_label_ = new QLabel("14px");
	font_size_label_->setStyleSheet("color:#8a8a96; min-width:32px; text-align:center;");
	font_bigger_btn_ = new QPushButton("A+");
	font_bigger_btn_->setFixedWidth(38);
	font_bigger_btn_->setStyleSheet("padding:2px 0;");
	font_bigger_btn_->setToolTip("Bigger text");
	clear_btn_ = new QPushButton("Clear");

	top_live->addWidget(live_reader_status_);
	top_live->addWidget(live_reader_btn_);
	top_live->addStretch(1);
	top_live->addWidget(font_smaller_btn_);
	top_live->addWidget(font_size_label_);
	top_live->addWidget(font_bigger_btn_);
	top_live->addWidget(clear_btn_);
	ll->addLayout(top_live);

	// nothing to read yet: say what to do instead of an empty black box
	empty_chat_ = new QWidget();
	QVBoxLayout *el = new QVBoxLayout(empty_chat_);
	el->setContentsMargins(12, 24, 12, 24);
	QLabel *empty_lbl = new QLabel("<b>No chat channels yet.</b><br>Add the channels whose chat you want to see here: "
				       "your own, and a collaborator's if you stream together.");
	empty_lbl->setWordWrap(true);
	empty_lbl->setAlignment(Qt::AlignCenter);
	empty_lbl->setStyleSheet("color:#c8c8d0; font-size:13px;");
	QPushButton *empty_btn = new QPushButton("Add chat channels");
	empty_btn->setStyleSheet("background-color:#5865F2; color:#ffffff; font-weight:bold; padding:6px 16px; border-radius:3px;");
	el->addWidget(empty_lbl);
	el->addWidget(empty_btn, 0, Qt::AlignHCenter);
	el->addStretch(1);
	ll->addWidget(empty_chat_);

	chat_browser_ = new QTextBrowser();
	chat_browser_->setOpenExternalLinks(false);
	chat_browser_->setReadOnly(true);
	chat_browser_->document()->setMaximumBlockCount(500);
	chat_browser_->setStyleSheet(
		"QTextBrowser { background-color: #121217; color: #f2f2f5; border: 1px solid #282832; border-radius: 4px; padding: 6px; font-family: 'Segoe UI', system-ui, sans-serif; }"
	);
	const char *init_plats[] = {"twitch", "kick", "youtube", "tiktok", "x", "facebook", "chat"};
	for (const char *plat : init_plats) {
		QImage icon = create_platform_icon(plat, 48);
		chat_browser_->document()->addResource(QTextDocument::ImageResource, QUrl(QString("platform://%1").arg(plat)), icon);
	}
	ll->addWidget(chat_browser_, 1);

	pause_scroll_btn_ = new QPushButton(QString::fromUtf8("\xE2\xAC\x87 Chat paused - click to resume autoscroll"));
	pause_scroll_btn_->setStyleSheet("background-color: #ffb020; color: #15151c; font-weight: bold; padding: 5px; border-radius: 3px; font-size: 13px;");
	pause_scroll_btn_->hide();
	ll->addWidget(pause_scroll_btn_);

	// --- Chat sources: which channels, the reader, filters ----------------------------------------
	sources_page_ = new QWidget();
	QVBoxLayout *sl = new QVBoxLayout(sources_page_);
	sl->setContentsMargins(6, 6, 6, 6);

	notice_ = new QLabel();
	notice_->setWordWrap(true);
	notice_->setStyleSheet("background:#3a2530; color:#ffb3c1; padding:6px; border-radius:4px;");
	notice_->hide();
	sl->addWidget(notice_);

	// 1. add a channel: one way, the same for every platform
	QGroupBox *add_grp = new QGroupBox("Add a chat channel");
	QVBoxLayout *agl = new QVBoxLayout(add_grp);
	QHBoxLayout *arow = new QHBoxLayout();
	add_platform_ = new QComboBox();
	add_platform_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	const std::vector<ChatPlatform> &cpl = chat_platforms();
	for (size_t i = 0; i < cpl.size(); ++i)
		add_platform_->addItem(QIcon(QPixmap::fromImage(create_platform_icon(cpl[i].id, 20))), qs(cpl[i].label), qs(cpl[i].id));
	add_input_ = new QLineEdit();
	add_input_->setMaxLength(300);
	add_input_->setMinimumHeight(26);
	QPushButton *add_btn = new QPushButton("Add");
	add_btn->setStyleSheet("background-color:#5865F2; color:#ffffff; font-weight:bold; padding:4px 14px; border-radius:3px;");
	add_btn->setMinimumHeight(26);
	arow->addWidget(add_platform_);
	arow->addWidget(add_input_, 1);
	arow->addWidget(add_btn);
	agl->addLayout(arow);
	add_hint_ = new QLabel();
	add_hint_->setWordWrap(true);
	add_hint_->setStyleSheet("color:#8a8a96; font-size:11px;");
	agl->addWidget(add_hint_);
	add_info_ = new QLabel();
	add_info_->setWordWrap(true);
	add_info_->hide();
	agl->addWidget(add_info_);
	from_dest_btn_ = new QPushButton("Add my channels from Streams");
	from_dest_btn_->setToolTip("One chat channel per Twitch, Kick, TikTok or YouTube destination, from its handle (YouTube: your "
				   "@channel; the live stream is found by itself). Adds only what is missing; never changes a channel you added yourself.");
	agl->addWidget(from_dest_btn_, 0, Qt::AlignLeft);
	sl->addWidget(add_grp);

	// 2. the channels
	table_ = new QTableWidget(0, 4);
	QStringList heads;
	heads << "On" << "Status" << "Platform" << "Channel / video";
	table_->setHorizontalHeaderLabels(heads);
	table_->verticalHeader()->hide();
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
	table_->setMinimumHeight(130);
	sl->addWidget(table_, 1);

	QHBoxLayout *row = new QHBoxLayout();
	edit_btn_ = new QPushButton("Edit...");
	remove_btn_ = new QPushButton("Remove");
	row->addWidget(edit_btn_);
	row->addWidget(remove_btn_);
	row->addStretch(1);
	sl->addLayout(row);

	// 3. the reader: one start/stop, one "when" choice
	QGroupBox *rd_grp = new QGroupBox("Chat reader");
	QVBoxLayout *rgl = new QVBoxLayout(rd_grp);
	QHBoxLayout *top = new QHBoxLayout();
	reader_btn_ = new QPushButton("Start chat reader");
	reader_btn_->setMinimumHeight(26);
	reader_state_ = new QLabel();
	top->addWidget(reader_btn_);
	top->addWidget(reader_state_, 1);
	rgl->addLayout(top);
	QHBoxLayout *mrow = new QHBoxLayout();
	mrow->addWidget(new QLabel("Start it:"));
	start_mode_ = new QComboBox();
	start_mode_->addItem("Only when I press Start", 0);
	start_mode_->addItem("When OBS starts", 1);
	start_mode_->addItem("When I go live", 2);
	mrow->addWidget(start_mode_, 1);
	rgl->addLayout(mrow);
	QLabel *about = new QLabel("The reader opens each channel's public chat page in a hidden browser of its own: no account, "
				   "no login, no platform API. Automated reading may be against a platform's terms; you decide.");
	about->setWordWrap(true);
	about->setStyleSheet("color:#8a8a96; font-size:11px;");
	rgl->addWidget(about);
	sl->addWidget(rd_grp);

	// 4. filters
	QGroupBox *flt_grp = new QGroupBox("Filters");
	QVBoxLayout *fgl = new QVBoxLayout(flt_grp);
	hide_commands_ = new QCheckBox("Hide commands (!word, /word)");
	hide_bots_ = new QCheckBox("Hide well-known bots");
	fgl->addWidget(hide_commands_);
	fgl->addWidget(hide_bots_);
	QHBoxLayout *lists = new QHBoxLayout();
	QVBoxLayout *wl = new QVBoxLayout();
	wl->addWidget(new QLabel("Blocked words (one per line)"));
	words_ = new QPlainTextEdit();
	words_->setMaximumHeight(70);
	wl->addWidget(words_);
	QVBoxLayout *ul = new QVBoxLayout();
	ul->addWidget(new QLabel("Blocked users (one per line)"));
	users_ = new QPlainTextEdit();
	users_->setMaximumHeight(70);
	ul->addWidget(users_);
	lists->addLayout(wl);
	lists->addLayout(ul);
	fgl->addLayout(lists);
	QPushButton *apply = new QPushButton("Save word and user lists");
	fgl->addWidget(apply, 0, Qt::AlignLeft);
	sl->addWidget(flt_grp);

	// --- Voice: read chat aloud, chimes -----------------------------------------------------------
	voice_page_ = new QWidget();
	QVBoxLayout *vl = new QVBoxLayout(voice_page_);
	vl->setContentsMargins(10, 10, 10, 10);
	vl->setSpacing(8);

	QLabel *voice_desc = new QLabel("Read incoming chat aloud with neural voices that run on this PC (no internet, no account), "
					"or play a short chime on each new line.");
	voice_desc->setWordWrap(true);
	voice_desc->setStyleSheet("color:#8a8a96; font-size:12px; margin-bottom:4px;");
	vl->addWidget(voice_desc);

	QGroupBox *tts_grp = new QGroupBox("Voice reader");
	QVBoxLayout *tgl = new QVBoxLayout(tts_grp);
	tts_enabled_ = new QCheckBox("Read chat aloud");
	tts_enabled_->setStyleSheet("font-weight:bold; color:#3ddc84;");
	tgl->addWidget(tts_enabled_);

	QHBoxLayout *vrow1 = new QHBoxLayout();
	vrow1->addWidget(new QLabel("Voice:"));
	tts_voice_ = new QComboBox();
	tts_voice_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	tts_voice_->setMinimumContentsLength(12);
	std::vector<TtsVoice> installed_voices = ChatSpeaker::voices();
	for (const TtsVoice &v : installed_voices)
		tts_voice_->addItem(qs(v.name), qs(v.id));
	if (installed_voices.empty())
		tts_voice_->addItem("No voices installed: run the installer again and tick a language", QString());
	vrow1->addWidget(tts_voice_, 1);

	tts_test_btn_ = new QPushButton("Preview");
	tts_test_btn_->setStyleSheet("background-color:#5865F2; color:#ffffff; font-weight:bold; padding:4px 12px; border-radius:3px;");
	vrow1->addWidget(tts_test_btn_);
	tgl->addLayout(vrow1);
	QLabel *voice_hint = new QLabel("More languages and accents: run the installer again and tick them under Voices. "
					 "The voice reader needs Node.js, like the chat reader.");
	voice_hint->setWordWrap(true);
	voice_hint->setStyleSheet("color:#8a8a96; font-size:11px;");
	tgl->addWidget(voice_hint);

	QHBoxLayout *vrow2 = new QHBoxLayout();
	vrow2->addWidget(new QLabel("Volume:"));
	tts_volume_ = new QSlider(Qt::Horizontal);
	tts_volume_->setRange(0, 100);
	tts_volume_->setValue(85);
	vrow2->addWidget(tts_volume_, 1);
	vrow2->addWidget(new QLabel("Speed:"));
	tts_speed_ = new QSlider(Qt::Horizontal);
	tts_speed_->setRange(-5, 5);
	tts_speed_->setValue(0);
	vrow2->addWidget(tts_speed_, 1);
	tgl->addLayout(vrow2);

	tts_include_user_ = new QCheckBox("Say who wrote it (\"Viewer says\")");
	tts_include_user_->setChecked(true);
	tts_include_platform_ = new QCheckBox("Say where (\"on Twitch\")");
	tts_include_platform_->setChecked(false);
	tgl->addWidget(tts_include_user_);
	tgl->addWidget(tts_include_platform_);
	vl->addWidget(tts_grp);

	// one voice per platform, so you hear where a line came from ("Same as the main voice" by default)
	QGroupBox *pp_grp = new QGroupBox("Voice per platform");
	QGridLayout *ppl = new QGridLayout(pp_grp);
	ppl->setColumnStretch(1, 1);
	const std::vector<ChatPlatform> &vpl = chat_platforms();
	for (size_t i = 0; i < vpl.size(); ++i) {
		const std::string pid = vpl[i].id;
		QLabel *lbl = new QLabel(platform_name(pid));
		lbl->setMinimumWidth(64);
		QLabel *ico = new QLabel();
		ico->setPixmap(QPixmap::fromImage(create_platform_icon(pid, 18)));
		QComboBox *cb = new QComboBox();
		cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		cb->setMinimumContentsLength(10);
		cb->addItem("Same as the main voice", QString());
		for (const TtsVoice &v : installed_voices)
			cb->addItem(qs(v.name), qs(v.id));
		QPushButton *play = new QPushButton();
		play->setIcon(play->style()->standardIcon(QStyle::SP_MediaPlay)); // the theme's font has no play-symbol glyph
		play->setFixedWidth(30);
		play->setToolTip("Hear this voice");
		int prow = static_cast<int>(i);
		QHBoxLayout *name = new QHBoxLayout();
		name->setContentsMargins(0, 0, 0, 0);
		name->addWidget(ico);
		name->addWidget(lbl);
		ppl->addLayout(name, prow, 0);
		ppl->addWidget(cb, prow, 1);
		ppl->addWidget(play, prow, 2);
		platform_voice_.push_back(std::make_pair(pid, cb));
		QObject::connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { save_voice_settings(); });
		QObject::connect(play, &QPushButton::clicked, [this, pid]() {
			if (speaker_)
				speaker_->interrupt();
			play_tts("Viewer", pid, "This is how chat from " + ss(platform_name(pid)) + " sounds.");
		});
	}
	vl->addWidget(pp_grp);

	QGroupBox *snd_grp = new QGroupBox("Chime");
	QVBoxLayout *sgl = new QVBoxLayout(snd_grp);
	sound_alert_enabled_ = new QCheckBox("Play a chime on each new chat line");
	sound_alert_enabled_->setStyleSheet("font-weight:bold; color:#38bdf8;");
	sgl->addWidget(sound_alert_enabled_);
	QHBoxLayout *srow = new QHBoxLayout();
	srow->addWidget(new QLabel("Sound:"));
	sound_alert_type_ = new QComboBox();
	sound_alert_type_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	sound_alert_type_->setMinimumContentsLength(12);
	sound_alert_type_->addItem("Gentle Chime (Dual Note)", "chime");
	sound_alert_type_->addItem("Modern Ding (Crystal Bell)", "ding");
	sound_alert_type_->addItem("Soft Pop (Bubble)", "pop");
	sound_alert_type_->addItem("Subtle Click (Minimal)", "subtle");
	srow->addWidget(sound_alert_type_, 1);
	sound_test_btn_ = new QPushButton("Test");
	srow->addWidget(sound_test_btn_);
	sgl->addLayout(srow);
	vl->addWidget(snd_grp);
	vl->addStretch(1);

	// --- footer: version and updates (the dock shows it under the tabs) ---------------------------
	footer_ = new QWidget();
	QHBoxLayout *urow = new QHBoxLayout(footer_);
	urow->setContentsMargins(6, 0, 6, 2);
	QLabel *ver_lbl = new QLabel("OBS Multistream Chat " "v" PLUGIN_VERSION);
	ver_lbl->setStyleSheet("color:#8a8a96; font-size:11px;");
	urow->addWidget(ver_lbl);
	check_updates_btn_ = new QPushButton("Check for updates");
	check_updates_btn_->setFlat(true);
	check_updates_btn_->setStyleSheet("color:#58a6ff; font-size:11px; padding:0 4px;");
	urow->addWidget(check_updates_btn_);
	update_status_lbl_ = new QLabel();
	update_status_lbl_->setStyleSheet("color:#8a8a96; font-size:11px;");
	update_status_lbl_->setWordWrap(true);
	urow->addWidget(update_status_lbl_, 1);
	download_update_btn_ = new QPushButton("Download update");
	download_update_btn_->setStyleSheet("background-color:#3ddc84; color:#121217; font-weight:bold; padding:2px 8px; border-radius:3px; font-size:11px;");
	download_update_btn_->hide();
	urow->addWidget(download_update_btn_);

	// Connect signals
	QObject::connect(empty_btn, &QPushButton::clicked, [this]() {
		if (go_sources_)
			go_sources_();
		add_input_->setFocus();
	});
	QObject::connect(add_platform_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_add_hint(); });
	QObject::connect(add_input_, &QLineEdit::returnPressed, [this]() { add_quick(); });
	QObject::connect(add_btn, &QPushButton::clicked, [this]() { add_quick(); });
	QObject::connect(from_dest_btn_, &QPushButton::clicked, [this]() { add_from_destinations(); });
	QObject::connect(tts_test_btn_, &QPushButton::clicked, [this]() {
		if (speaker_)
			speaker_->interrupt();
		play_tts("Viewer", "", "Welcome to the stream! The voice reader is ready."); // "" = the main voice
		if (speaker_ && !speaker_->problem().empty())
			QMessageBox::warning(this, "OBS Multistream Chat", qs(speaker_->problem()));
	});
	QObject::connect(sound_test_btn_, &QPushButton::clicked, [this]() {
		std::string st = sound_alert_type_ ? ss(sound_alert_type_->currentData().toString()) : "chime";
		play_alert_sound(st);
	});
	load_voice_settings();
	QObject::connect(tts_enabled_, &QCheckBox::toggled, [this](bool) { save_voice_settings(); });
	QObject::connect(tts_voice_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { save_voice_settings(); });
	QObject::connect(tts_volume_, &QSlider::valueChanged, [this](int) { save_voice_settings(); });
	QObject::connect(tts_speed_, &QSlider::valueChanged, [this](int) { save_voice_settings(); });
	QObject::connect(tts_include_user_, &QCheckBox::toggled, [this](bool) { save_voice_settings(); });
	QObject::connect(tts_include_platform_, &QCheckBox::toggled, [this](bool) { save_voice_settings(); });
	QObject::connect(sound_alert_enabled_, &QCheckBox::toggled, [this](bool) { save_voice_settings(); });
	QObject::connect(sound_alert_type_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { save_voice_settings(); });
	QObject::connect(check_updates_btn_, &QPushButton::clicked, [this]() { check_for_updates(); });
	QObject::connect(download_update_btn_, &QPushButton::clicked, [this]() {
		QDesktopServices::openUrl(QUrl("https://github.com/otissy/obs-multistream-chat/releases/latest"));
	});
	QObject::connect(live_reader_btn_, &QPushButton::clicked, [this]() { toggle_reader(); });
	QObject::connect(clear_btn_, &QPushButton::clicked, [this]() { clear_chat(); });
	QObject::connect(font_smaller_btn_, &QPushButton::clicked, [this]() { set_font_scale(-2); });
	QObject::connect(font_bigger_btn_, &QPushButton::clicked, [this]() { set_font_scale(+2); });

	QScrollBar *bar = chat_browser_->verticalScrollBar();
	QObject::connect(bar, &QScrollBar::valueChanged, [this, bar](int val) {
		if (val >= bar->maximum() - 25) {
			autoscroll_ = true;
			pause_scroll_btn_->hide();
		} else {
			autoscroll_ = false;
			pause_scroll_btn_->show();
		}
	});
	QObject::connect(pause_scroll_btn_, &QPushButton::clicked, [this, bar]() {
		autoscroll_ = true;
		bar->setValue(bar->maximum());
		pause_scroll_btn_->hide();
	});

	QObject::connect(edit_btn_, &QPushButton::clicked, [this]() { edit_selected(); });
	QObject::connect(remove_btn_, &QPushButton::clicked, [this]() { remove_selected(); });
	QObject::connect(reader_btn_, &QPushButton::clicked, [this]() { toggle_reader(); });
	QObject::connect(apply, &QPushButton::clicked, [this]() { apply_filters(); });
	QObject::connect(start_mode_, QOverload<int>::of(&QComboBox::activated), [this](int) {
		set_reader_start_mode(plugin().cfg.chat, start_mode_->currentData().toInt());
		save();
	});
	QObject::connect(hide_commands_, &QCheckBox::clicked, [this](bool) { apply_filters(); });
	QObject::connect(hide_bots_, &QCheckBox::clicked, [this](bool) { apply_filters(); });
	QObject::connect(table_, &QTableWidget::itemSelectionChanged, [this]() {
		bool any = !selected_id().empty();
		edit_btn_->setEnabled(any);
		remove_btn_->setEnabled(any);
	});
	edit_btn_->setEnabled(false);
	remove_btn_->setEnabled(false);
	update_add_hint();

	// Pre-load backlog messages
	PluginState &st = plugin();
	std::vector<ChatMessage> bl = st.chat_hub.backlog(50);
	for (size_t i = 0; i < bl.size(); ++i) {
		if (bl[i].seq > last_seq_)
			last_seq_ = bl[i].seq;
		append_message(bl[i]);
	}
	if (last_seq_ == 0)
		last_seq_ = st.chat_hub.last_seq();

	chat_poll_timer_ = new QTimer(this);
	QObject::connect(chat_poll_timer_, &QTimer::timeout, [this]() { poll_chat(); });
	chat_poll_timer_->start(250);

	refresh();
}

// Voice tab settings survive an OBS restart (before 0.5.0 every start was "voice reader off, first voice").
void ChatPanel::load_voice_settings()
{
	QString path = voice_state_path();
	if (path.isEmpty())
		return;
	loading_voice_ = true;
	QSettings s(path, QSettings::IniFormat);
	s.beginGroup("voice");
	auto pick = [](QComboBox *cb, const QString &id) {
		int i = cb->findData(id);
		if (i >= 0)
			cb->setCurrentIndex(i);
	};
	tts_enabled_->setChecked(s.value("enabled", false).toBool());
	pick(tts_voice_, s.value("voice").toString());
	tts_volume_->setValue(s.value("volume", 85).toInt());
	tts_speed_->setValue(s.value("speed", 0).toInt());
	tts_include_user_->setChecked(s.value("say_user", true).toBool());
	tts_include_platform_->setChecked(s.value("say_platform", false).toBool());
	sound_alert_enabled_->setChecked(s.value("chime", false).toBool());
	pick(sound_alert_type_, s.value("chime_sound", "chime").toString());
	for (size_t i = 0; i < platform_voice_.size(); ++i)
		pick(platform_voice_[i].second, s.value(QString::fromStdString("voice_" + platform_voice_[i].first)).toString());
	s.endGroup();
	loading_voice_ = false;
}

void ChatPanel::save_voice_settings()
{
	QString path = voice_state_path();
	if (loading_voice_ || path.isEmpty() || !tts_voice_)
		return;
	QSettings s(path, QSettings::IniFormat);
	s.beginGroup("voice");
	s.setValue("enabled", tts_enabled_->isChecked());
	s.setValue("voice", tts_voice_->currentData().toString());
	s.setValue("volume", tts_volume_->value());
	s.setValue("speed", tts_speed_->value());
	s.setValue("say_user", tts_include_user_->isChecked());
	s.setValue("say_platform", tts_include_platform_->isChecked());
	s.setValue("chime", sound_alert_enabled_->isChecked());
	s.setValue("chime_sound", sound_alert_type_->currentData().toString());
	for (size_t i = 0; i < platform_voice_.size(); ++i)
		s.setValue(QString::fromStdString("voice_" + platform_voice_[i].first), platform_voice_[i].second->currentData().toString());
	s.endGroup();
}

void ChatPanel::attach(QTabWidget *tabs, int live_index, std::function<void()> go_sources)
{
	host_tabs_ = tabs;
	live_index_ = live_index;
	live_label_ = tabs ? tabs->tabText(live_index) : QString();
	go_sources_ = std::move(go_sources);
	if (tabs)
		QObject::connect(tabs, &QTabWidget::currentChanged, [this](int idx) {
			if (idx == live_index_)
				reset_unread();
		});
}

void ChatPanel::reset_unread()
{
	unread_count_ = 0;
	if (host_tabs_)
		host_tabs_->setTabText(live_index_, live_label_);
}

void ChatPanel::update_add_hint()
{
	const ChatPlatform *p = find_chat_platform(ss(add_platform_->currentData().toString()));
	QString t = p ? "Enter the " + qs(p->hint) + "." : QString();
	if (p && p->experimental)
		t += " Experimental: this platform may need a login or block automated browsers; nothing is ever logged in.";
	add_hint_->setText(t);
	add_input_->setPlaceholderText(p && std::string(p->id) == "youtube" ? "@yourchannel"
					: p && std::string(p->id) == "x"     ? "x.com/i/broadcasts/..."
					: p && std::string(p->id) == "tiktok" ? "@username"
									      : "channel name");
}

void ChatPanel::show_add_info(const QString &text, bool good)
{
	add_info_->setText(text);
	add_info_->setStyleSheet(good ? "color:#3ddc84; font-size:12px;" : "color:#ff4d4f; font-size:12px;");
	add_info_->show();
}

void ChatPanel::start_reader_if_wanted()
{
	PluginState &st = plugin();
	if (!st.reader_active) {
		std::string cerr;
		if (!plugin_start_reader(cerr))
			notice_->setText(qs(cerr));
	}
}

std::string ChatPanel::selected_id() const
{
	QList<QTableWidgetSelectionRange> r = table_->selectedRanges();
	if (r.isEmpty())
		return std::string();
	int row = r.first().topRow();
	return (row >= 0 && row < static_cast<int>(row_ids_.size())) ? row_ids_[static_cast<size_t>(row)] : std::string();
}

// One way to add a channel, the same for every platform (replaces "Auto-Connect", which added the typed
// name on Twitch, Kick, TikTok and X at once, and "+ Add Collaborator", which was Twitch only).
void ChatPanel::add_quick()
{
	PluginState &st = plugin();
	std::string platform = ss(add_platform_->currentData().toString());
	std::string target, err;
	AddResult r = add_chat_source(st.cfg.chat.sources, platform, ss(add_input_->text()), target, err);
	const ChatPlatform *p = find_chat_platform(platform);
	QString where = p ? qs(p->label) : qs(platform);
	switch (r) {
	case AddResult::Invalid:
		show_add_info(add_input_->text().trimmed().isEmpty() ? "Type a channel first." : qs(err), false);
		return;
	case AddResult::Full:
		show_add_info("At most 32 chat channels.", false);
		return;
	case AddResult::Duplicate:
		show_add_info(QString("%1 on %2 is already in the list.").arg(qs(target), where), false);
		return;
	case AddResult::Enabled:
		show_add_info(QString("%1 on %2 was in the list but off; it is on again.").arg(qs(target), where), true);
		break;
	case AddResult::Added:
		show_add_info(QString("Added %1 on %2.").arg(qs(target), where), true);
		break;
	}
	add_input_->clear();
	save();
	start_reader_if_wanted();
	refresh();
}

// "Add my channels from Streams": add-only (the old "Auto-sync" replaced the first source of the same
// platform, so a collaborator's channel was overwritten by your own).
void ChatPanel::add_from_destinations()
{
	PluginState &st = plugin();
	int n = add_sources_from_destinations(st.cfg.destinations, st.cfg.chat.sources);
	if (n == 0) {
		show_add_info(st.cfg.destinations.empty()
				      ? "No stream destinations yet: add them on the Streams tab first."
				      : "Nothing to add: every Twitch, Kick and TikTok destination with a handle is already here.",
			      false);
		return;
	}
	show_add_info(QString("Added %1 channel(s) from your stream destinations.").arg(n), true);
	save();
	start_reader_if_wanted();
	refresh();
}

void ChatPanel::save()
{
	std::string err;
	if (!plugin_save_config(err))
		QMessageBox::warning(this, "OBS Multistream Chat", "Could not save the configuration:\n" + qs(err));
	refresh();
}

void ChatPanel::add_source()
{
	PluginState &st = plugin();
	if (st.cfg.chat.sources.size() >= static_cast<size_t>(kMaxChatSources)) {
		QMessageBox::information(this, "OBS Multistream Chat", "At most 32 chat sources allowed.");
		return;
	}
	ChatSource s;
	unsigned n = 1;
	for (;;) {
		s.id = "c" + std::to_string(n++);
		bool used = false;
		for (size_t i = 0; i < st.cfg.chat.sources.size(); ++i)
			if (st.cfg.chat.sources[i].id == s.id)
				used = true;
		if (!used)
			break;
	}
	s.platform = "twitch";
	SourceDialog dlg(this, s, false);
	if (dlg.exec() == QDialog::Accepted) {
		st.cfg.chat.sources.push_back(dlg.result());
		save();
	}
}

void ChatPanel::edit_selected()
{
	PluginState &st = plugin();
	std::string id = selected_id();
	for (size_t i = 0; i < st.cfg.chat.sources.size(); ++i) {
		if (st.cfg.chat.sources[i].id != id)
			continue;
		SourceDialog dlg(this, st.cfg.chat.sources[i], true);
		if (dlg.exec() == QDialog::Accepted) {
			st.cfg.chat.sources[i] = dlg.result();
			save();
		}
		return;
	}
}

void ChatPanel::remove_selected()
{
	PluginState &st = plugin();
	std::string id = selected_id();
	for (size_t i = 0; i < st.cfg.chat.sources.size(); ++i) {
		if (st.cfg.chat.sources[i].id != id)
			continue;
		st.cfg.chat.sources.erase(st.cfg.chat.sources.begin() + static_cast<long>(i));
		save();
		return;
	}
}

void ChatPanel::toggle_reader()
{
	if (plugin_reader_running()) {
		plugin_stop_reader();
	} else {
		std::string err;
		if (!plugin_start_reader(err))
			notice_->setText(qs(err));
		else
			notice_->clear();
	}
	refresh();
}

void ChatPanel::apply_filters()
{
	ChatConfig &c = plugin().cfg.chat;
	c.hide_commands = hide_commands_->isChecked();
	c.hide_bots = hide_bots_->isChecked();
	c.blocked_words = lines_of(words_->toPlainText());
	c.blocked_users = lines_of(users_->toPlainText());
	plugin_apply_chat_filter();
	save();
}

void ChatPanel::rebuild_rows(const std::vector<std::string> &ids)
{
	row_ids_ = ids;
	table_->setRowCount(static_cast<int>(ids.size()));
	for (size_t i = 0; i < ids.size(); ++i) {
		int row = static_cast<int>(i);
		std::string id = ids[i];
		QCheckBox *cb = new QCheckBox();
		cb->setStyleSheet("margin-left:8px;");
		QObject::connect(cb, &QCheckBox::clicked, [this, id](bool on) {
			PluginState &st = plugin();
			for (size_t k = 0; k < st.cfg.chat.sources.size(); ++k)
				if (st.cfg.chat.sources[k].id == id) {
					st.cfg.chat.sources[k].enabled = on;
					save();
					return;
				}
		});
		table_->setCellWidget(row, 0, cb);
		for (int c = 1; c <= 3; ++c)
			table_->setItem(row, c, new QTableWidgetItem());
	}
	table_->resizeColumnsToContents();
}

void ChatPanel::refresh()
{
	PluginState &st = plugin();
	if (!filters_loaded_) {
		filters_loaded_ = true;
		hide_commands_->setChecked(st.cfg.chat.hide_commands);
		hide_bots_->setChecked(st.cfg.chat.hide_bots);
		words_->setPlainText(join_lines(st.cfg.chat.blocked_words));
		users_->setPlainText(join_lines(st.cfg.chat.blocked_users));
	}
	int mode_idx = start_mode_->findData(reader_start_mode(st.cfg.chat));
	if (mode_idx >= 0 && start_mode_->currentIndex() != mode_idx)
		start_mode_->setCurrentIndex(mode_idx);
	bool no_sources = st.cfg.chat.sources.empty();
	empty_chat_->setVisible(no_sources && chat_browser_->document()->isEmpty());
	chat_browser_->setVisible(!empty_chat_->isVisible());
	bool running = plugin_reader_running();
	reader_btn_->setText(running ? "Stop chat reader" : "Start chat reader");
	reader_btn_->setEnabled(running || !no_sources);
	live_reader_btn_->setEnabled(running || !no_sources);
	reader_state_->setText(running ? "Running" : no_sources ? "Add a channel first" : "Stopped");
	reader_state_->setStyleSheet(running ? "color:#3ddc84;" : "color:#8a8a96;");
	live_reader_btn_->setText(running ? "Stop reader" : "Start reader");

	std::vector<SourceStatus> live = st.chat_hub.sources();
	bool any_reading = false;
	bool any_waiting = false;
	for (size_t k = 0; k < live.size(); ++k) {
		if (live[k].state == "reading")
			any_reading = true;
		else if (live[k].state == "waiting" || live[k].state == "starting")
			any_waiting = true;
	}

	if (!running) {
		live_reader_status_->setText(QString::fromUtf8("\xE2\x97\x8F Reader stopped"));
		live_reader_status_->setStyleSheet("color:#8a8a96; font-weight:bold;");
	} else if (any_reading) {
		live_reader_status_->setText(QString::fromUtf8("\xE2\x97\x8F Chat Live"));
		live_reader_status_->setStyleSheet("color:#3ddc84; font-weight:bold;");
	} else if (any_waiting) {
		live_reader_status_->setText(QString::fromUtf8("\xE2\x97\x8F Auto-detecting live..."));
		live_reader_status_->setStyleSheet("color:#38bdf8; font-weight:bold;");
	} else if (running) {
		live_reader_status_->setText(QString::fromUtf8("\xE2\x97\x8F Reader active"));
		live_reader_status_->setStyleSheet("color:#38bdf8; font-weight:bold;");
	} else {
		live_reader_status_->setText(QString::fromUtf8("\xE2\x97\x8F Reader stopped"));
		live_reader_status_->setStyleSheet("color:#8a8a96; font-weight:bold;");
	}

	if (st.cfg.chat.sources.empty()) {
		notice_->hide();
	} else if (!st.chat_notice.empty()) {
		notice_->setText(qs(st.chat_notice));
		notice_->show();
	} else {
		notice_->hide();
	}

	std::vector<std::string> ids;
	for (size_t i = 0; i < st.cfg.chat.sources.size(); ++i)
		ids.push_back(st.cfg.chat.sources[i].id);
	if (ids != row_ids_)
		rebuild_rows(ids);
	live = st.chat_hub.sources();
	for (size_t i = 0; i < st.cfg.chat.sources.size(); ++i) {
		const ChatSource &s = st.cfg.chat.sources[i];
		int row = static_cast<int>(i);
		QCheckBox *cb = qobject_cast<QCheckBox *>(table_->cellWidget(row, 0));
		if (cb && cb->isChecked() != s.enabled) {
			bool prev = cb->blockSignals(true);
			cb->setChecked(s.enabled);
			cb->blockSignals(prev);
		}
		std::string state = running ? "starting" : "stopped";
		std::string detail;
		if (running) {
			for (size_t k = 0; k < live.size(); ++k)
				if (live[k].id == s.id) {
					state = live[k].state;
					detail = live[k].detail;
				}
		}
		if (!s.enabled) {
			state = "stopped";
			detail = "disabled";
		}
		StateLook lk = look_for(state);
		QTableWidgetItem *status = table_->item(row, 1);
		status->setText(QString::fromUtf8("\xE2\x97\x8F ") + lk.label);
		status->setForeground(QColor(lk.color));
		status->setToolTip(qs(detail));
		const ChatPlatform *p = find_chat_platform(s.platform);
		table_->item(row, 2)->setText(p ? qs(p->label) : qs(s.platform));
		table_->item(row, 2)->setIcon(QIcon(QPixmap::fromImage(create_platform_icon(s.platform, 24))));
		table_->item(row, 3)->setText(qs(s.target));
		table_->item(row, 3)->setToolTip(qs(detail));
	}
	poll_chat();
}

void ChatPanel::poll_chat()
{
	PluginState &st = plugin();
	std::vector<ChatMessage> msgs;
	if (st.chat_hub.wait_for(last_seq_, 0, msgs, 50)) {
		for (size_t i = 0; i < msgs.size(); ++i) {
			if (msgs[i].seq > last_seq_)
				last_seq_ = msgs[i].seq;
			append_message(msgs[i]);
		}
	}
}

void ChatPanel::append_message(const ChatMessage &m)
{
	if (m.author.empty() && m.parts.empty())
		return;

	PlatformBadge b = badge_for(m.platform);
	QString c = author_color(m);
	QString author = qs(m.author).toHtmlEscaped();

	QString time_str;
	if (m.arrival_ms > 0) {
		QDateTime dt = QDateTime::fromMSecsSinceEpoch(m.arrival_ms);
		time_str = dt.toString("HH:mm");
	} else {
		time_str = QDateTime::currentDateTime().toString("HH:mm");
	}

	QString content;
	for (size_t i = 0; i < m.parts.size(); ++i) {
		const ChatPart &p = m.parts[i];
		if (p.emote && !p.url.empty()) {
			int img_h = std::max(14, static_cast<int>(font_size_ * 1.35));
			QString alt = qs(p.text).toHtmlEscaped();
			content += QString("<img src=\"%1\" height=\"%2\" style=\"vertical-align:middle;margin:0 1px;\" alt=\"%3\" title=\"%3\">")
					   .arg(qs(p.url), QString::number(img_h), alt);
		} else {
			content += qs(p.text).toHtmlEscaped();
		}
	}

	int ico_size = std::max(14, static_cast<int>(font_size_ * 1.15));
	QString ico_html = QString("<img src=\"platform://%1\" width=\"%2\" height=\"%2\" style=\"vertical-align:middle;\" alt=\"%3\" title=\"%3\">&nbsp;")
				   .arg(qs(m.platform), QString::number(ico_size), qs(b.label));

	QString html = QString(
		"<div style=\"margin-bottom:4px; line-height:1.35; font-size:%1px;\">"
		"%2"
		"<span style=\"color:#8a8a96; font-size:80%;\">%3</span>&nbsp;&nbsp;"
		"<span style=\"color:%4; font-weight:bold;\">%5:</span>&nbsp;"
		"<span style=\"color:#f2f2f5;\">%6</span>"
		"</div>")
		.arg(QString::number(font_size_), ico_html, time_str, c, author, content);

	QScrollBar *bar = chat_browser_->verticalScrollBar();
	bool was_at_bottom = (bar->value() >= bar->maximum() - 30);

	chat_browser_->append(html);

	if (autoscroll_ && was_at_bottom) {
		bar->setValue(bar->maximum());
		pause_scroll_btn_->hide();
	} else if (!was_at_bottom) {
		pause_scroll_btn_->show();
	}

	if (host_tabs_ && host_tabs_->currentIndex() != live_index_) {
		unread_count_++;
		host_tabs_->setTabText(live_index_, QString("%1 (%2)").arg(live_label_).arg(unread_count_));
	}

	if (sound_alert_enabled_ && sound_alert_enabled_->isChecked()) {
		std::string st = sound_alert_type_ ? ss(sound_alert_type_->currentData().toString()) : "chime";
		play_alert_sound(st);
	}

	if (tts_enabled_ && tts_enabled_->isChecked()) {
		std::string plain;
		for (size_t p = 0; p < m.parts.size(); ++p) {
			if (!m.parts[p].emote && !m.parts[p].text.empty())
				plain += m.parts[p].text + " ";
		}
		play_tts(m.author, m.platform, plain);
	}
}

void ChatPanel::clear_chat()
{
	chat_browser_->clear();
	reset_unread();
}

void ChatPanel::set_font_scale(int delta)
{
	font_size_ = std::max(10, std::min(28, font_size_ + delta));
	font_size_label_->setText(QString("%1px").arg(font_size_));
}

void ChatPanel::play_alert_sound(const std::string &type)
{
#ifdef _WIN32
	const int sample_rate = 44100;
	int duration_samples = static_cast<int>(sample_rate * 0.25);
	if (type == "ding")
		duration_samples = static_cast<int>(sample_rate * 0.35);
	else if (type == "pop")
		duration_samples = static_cast<int>(sample_rate * 0.08);
	else if (type == "subtle")
		duration_samples = static_cast<int>(sample_rate * 0.05);

	int data_size = duration_samples * 2;
	std::vector<uint8_t> wav(44 + data_size);

	// WAV header
	memcpy(&wav[0], "RIFF", 4);
	uint32_t file_size = 36 + data_size;
	memcpy(&wav[4], &file_size, 4);
	memcpy(&wav[8], "WAVEfmt ", 8);
	uint32_t fmt_len = 16;
	memcpy(&wav[16], &fmt_len, 4);
	uint16_t fmt_tag = 1; // PCM
	memcpy(&wav[20], &fmt_tag, 2);
	uint16_t channels = 1;
	memcpy(&wav[22], &channels, 2);
	uint32_t srate = sample_rate;
	memcpy(&wav[24], &srate, 4);
	uint32_t brate = sample_rate * 2;
	memcpy(&wav[28], &brate, 4);
	uint16_t balign = 2;
	memcpy(&wav[32], &balign, 2);
	uint16_t bps = 16;
	memcpy(&wav[34], &bps, 2);
	memcpy(&wav[36], "data", 4);
	uint32_t dsize = data_size;
	memcpy(&wav[40], &dsize, 4);

	int16_t *samples = reinterpret_cast<int16_t *>(&wav[44]);
	const double pi = 3.14159265358979323846;

	for (int i = 0; i < duration_samples; ++i) {
		double t = static_cast<double>(i) / sample_rate;
		double env = std::exp(-t * (type == "ding" ? 8.0 : (type == "pop" ? 35.0 : 12.0)));
		double freq = 587.33; // D5
		if (type == "chime") {
			if (i >= duration_samples / 2) {
				freq = 880.0; // A5
				t = static_cast<double>(i - duration_samples / 2) / sample_rate;
				env = std::exp(-t * 12.0);
			}
		} else if (type == "ding") {
			freq = 1174.66; // D6
		} else if (type == "pop") {
			freq = 600.0 - 400.0 * (static_cast<double>(i) / duration_samples);
		} else if (type == "subtle") {
			freq = 440.0;
			env = std::exp(-t * 40.0);
		}
		double val = std::sin(2.0 * pi * freq * t) * env;
		samples[i] = static_cast<int16_t>(val * 24000.0);
	}

	PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), NULL, SND_MEMORY | SND_ASYNC);
#else
	(void)type;
#endif
}

void ChatPanel::play_tts(const std::string &author, const std::string &platform, const std::string &message)
{
#ifdef _WIN32
	if (message.empty())
		return;
	std::string text_to_say;
	bool inc_user = tts_include_user_ ? tts_include_user_->isChecked() : true;
	bool inc_plat = tts_include_platform_ ? tts_include_platform_->isChecked() : false;

	if (inc_user && inc_plat && !platform.empty())
		text_to_say = author + " on " + ss(platform_name(platform)) + " says: " + message;
	else if (inc_user)
		text_to_say = author + " says: " + message;
	else
		text_to_say = message;

	int vol = tts_volume_ ? tts_volume_->value() : 85;
	int rate = tts_speed_ ? tts_speed_->value() : 0;
	std::string main_voice = tts_voice_ ? ss(tts_voice_->currentData().toString()) : std::string();
	std::vector<std::pair<std::string, std::string>> per;
	for (size_t i = 0; i < platform_voice_.size(); ++i)
		per.push_back(std::make_pair(platform_voice_[i].first, ss(platform_voice_[i].second->currentData().toString())));
	std::vector<std::string> installed;
	for (int i = 0; tts_voice_ && i < tts_voice_->count(); ++i)
		installed.push_back(ss(tts_voice_->itemData(i).toString()));
	std::string voice = voice_for_platform(per, platform, main_voice, installed);

	if (!speaker_) {
		const PluginState &st = plugin();
		size_t slash = st.config_path.find_last_of("/\\");
		std::string dir = slash == std::string::npos ? std::string() : st.config_path.substr(0, slash + 1);
		speaker_ = std::make_unique<ChatSpeaker>(st.cfg.chat.node_path, dir + "voice-helper.log");
	}
	speaker_->say(text_to_say, voice, vol, rate);
#else
	(void)author;
	(void)platform;
	(void)message;
#endif
}

void ChatPanel::check_for_updates()
{
#ifdef _WIN32
	if (update_status_lbl_) {
		update_status_lbl_->setText("Checking GitHub for updates...");
		update_status_lbl_->setStyleSheet("color:#38bdf8; font-size:12px;");
	}
	std::thread([this]() {
		std::string tag;
		bool ok = false;
		HINTERNET hInternet = InternetOpenA("obs-multistream-chat", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
		if (hInternet) {
			HINTERNET hConnect = InternetOpenUrlA(hInternet,
				"https://api.github.com/repos/otissy/obs-multistream-chat/releases/latest",
				"User-Agent: obs-multistream-chat\r\n", static_cast<DWORD>(-1), INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
			if (hConnect) {
				char buf[4096];
				DWORD read = 0;
				std::string resp;
				while (InternetReadFile(hConnect, buf, sizeof(buf) - 1, &read) && read > 0) {
					buf[read] = '\0';
					resp.append(buf, read);
					if (resp.size() > 65536)
						break;
				}
				InternetCloseHandle(hConnect);
				size_t pos = resp.find("\"tag_name\":");
				if (pos != std::string::npos) {
					size_t q1 = resp.find('"', pos + 11);
					if (q1 != std::string::npos) {
						size_t q2 = resp.find('"', q1 + 1);
						if (q2 != std::string::npos) {
							tag = resp.substr(q1 + 1, q2 - q1 - 1);
							ok = true;
						}
					}
				}
			}
			InternetCloseHandle(hInternet);
		}
		QMetaObject::invokeMethod(this, [this, ok, tag]() {
			if (!update_status_lbl_)
				return;
			if (!ok) {
				update_status_lbl_->setText("Could not reach GitHub (offline or rate limited). You are running v" PLUGIN_VERSION ".");
				update_status_lbl_->setStyleSheet("color:#8a8a96; font-size:12px;");
			} else if (tag == "v" PLUGIN_VERSION || tag == PLUGIN_VERSION) {
				update_status_lbl_->setText(QString("You are on the latest version (%1)!").arg(qs(tag)));
				update_status_lbl_->setStyleSheet("color:#3ddc84; font-weight:bold; font-size:12px;");
			} else {
				update_status_lbl_->setText(QString("New update available: %1!").arg(qs(tag)));
				update_status_lbl_->setStyleSheet("color:#ffb020; font-weight:bold; font-size:12px;");
				if (download_update_btn_)
					download_update_btn_->show();
			}
		});
	}).detach();
#endif
}

} // namespace oms
