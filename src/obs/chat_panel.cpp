#include "chat_panel.hpp"
#include "chat_targets.hpp"
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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
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

struct StateLook {
	const char *label;
	const char *color;
};

StateLook look_for(const std::string &state)
{
	if (state == "reading")
		return {"Reading", "#3ddc84"};
	if (state == "quiet")
		return {"Quiet: no messages yet", "#ffb020"};
	if (state == "layout")
		return {"Page layout changed or login needed", "#ff4d4f"};
	if (state == "login")
		return {"Needs login", "#ff4d4f"};
	if (state == "error")
		return {"Error", "#ff4d4f"};
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
			platform_->addItem(qs(pl[i].label), qs(pl[i].id));
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
	QVBoxLayout *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);

	QLabel *about = new QLabel("Reads the public chat page of each channel below in a hidden Chrome of its own, "
				   "without any platform API, token or login. Automated reading may be against a platform's terms: "
				   "you decide. See docs/limits.md.");
	about->setWordWrap(true);
	about->setStyleSheet("color:#8a8a96;");
	root->addWidget(about);

	notice_ = new QLabel();
	notice_->setWordWrap(true);
	notice_->setStyleSheet("background:#3a2530; color:#ffb3c1; padding:6px; border-radius:4px;");
	notice_->hide();
	root->addWidget(notice_);

	QHBoxLayout *top = new QHBoxLayout();
	reader_btn_ = new QPushButton("Start chat reader");
	reader_state_ = new QLabel();
	start_with_obs_ = new QCheckBox("Start with OBS");
	top->addWidget(reader_btn_);
	top->addWidget(reader_state_, 1);
	top->addWidget(start_with_obs_);
	root->addLayout(top);

	table_ = new QTableWidget(0, 4);
	QStringList heads;
	heads << "On" << "Status" << "Platform" << "Channel / video";
	table_->setHorizontalHeaderLabels(heads);
	table_->verticalHeader()->hide();
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
	root->addWidget(table_, 1);

	QHBoxLayout *row = new QHBoxLayout();
	QPushButton *add = new QPushButton("Add...");
	edit_btn_ = new QPushButton("Edit...");
	remove_btn_ = new QPushButton("Remove");
	row->addWidget(add);
	row->addWidget(edit_btn_);
	row->addWidget(remove_btn_);
	row->addStretch(1);
	root->addLayout(row);

	hide_commands_ = new QCheckBox("Hide commands (!word, /word)");
	hide_bots_ = new QCheckBox("Hide well-known bots");
	root->addWidget(hide_commands_);
	root->addWidget(hide_bots_);
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
	root->addLayout(lists);
	QPushButton *apply = new QPushButton("Apply filters");
	root->addWidget(apply);

	QHBoxLayout *urlrow = new QHBoxLayout();
	url_label_ = new QLabel();
	url_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	url_label_->setStyleSheet("color:#8a8a96;");
	QPushButton *copy = new QPushButton("Copy chat URL");
	urlrow->addWidget(url_label_, 1);
	urlrow->addWidget(copy);
	root->addLayout(urlrow);

	QObject::connect(add, &QPushButton::clicked, [this]() { add_source(); });
	QObject::connect(edit_btn_, &QPushButton::clicked, [this]() { edit_selected(); });
	QObject::connect(remove_btn_, &QPushButton::clicked, [this]() { remove_selected(); });
	QObject::connect(reader_btn_, &QPushButton::clicked, [this]() { toggle_reader(); });
	QObject::connect(apply, &QPushButton::clicked, [this]() { apply_filters(); });
	QObject::connect(copy, &QPushButton::clicked, [this]() { copy_chat_url(); });
	QObject::connect(start_with_obs_, &QCheckBox::clicked, [this](bool on) {
		plugin().cfg.chat.start_with_obs = on;
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
	refresh();
}

std::string ChatPanel::selected_id() const
{
	QList<QTableWidgetSelectionRange> r = table_->selectedRanges();
	if (r.isEmpty())
		return std::string();
	int row = r.first().topRow();
	return (row >= 0 && row < static_cast<int>(row_ids_.size())) ? row_ids_[static_cast<size_t>(row)] : std::string();
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
		QMessageBox::information(this, "OBS Multistream Chat", "At most 10 chat sources.");
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

void ChatPanel::copy_chat_url()
{
	std::string url = plugin_chat_url();
	if (url.empty()) {
		QMessageBox::warning(this, "OBS Multistream Chat", "The local server is not running.");
		return;
	}
	QApplication::clipboard()->setText(qs(url + "?theme=dark&font=22&max=30"));
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
	start_with_obs_->setChecked(st.cfg.chat.start_with_obs);
	bool running = plugin_reader_running();
	reader_btn_->setText(running ? "Stop chat reader" : "Start chat reader");
	reader_state_->setText(running ? "Reader running" : "Reader stopped");
	reader_state_->setStyleSheet(running ? "color:#3ddc84;" : "color:#8a8a96;");
	if (!st.chat_notice.empty()) {
		notice_->setText(qs(st.chat_notice));
	}
	notice_->setVisible(!notice_->text().isEmpty());
	std::string url = plugin_chat_url();
	url_label_->setText(url.empty() ? "The local server is not running." : qs(url));

	std::vector<std::string> ids;
	for (size_t i = 0; i < st.cfg.chat.sources.size(); ++i)
		ids.push_back(st.cfg.chat.sources[i].id);
	if (ids != row_ids_)
		rebuild_rows(ids);
	std::vector<SourceStatus> live = st.chat_hub.sources();
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
		table_->item(row, 3)->setText(qs(s.target));
		table_->item(row, 3)->setToolTip(qs(detail));
	}
}

} // namespace oms
