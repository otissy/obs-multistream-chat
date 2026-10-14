#include "dock.hpp"
#include "destination.hpp"
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
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

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

class DestinationDialog : public QDialog {
public:
	DestinationDialog(QWidget *parent, const Destination &d, bool editing) : QDialog(parent), dest_(d), editing_(editing)
	{
		setWindowTitle(editing ? T("Dialog.EditTitle") : T("Dialog.AddTitle"));
		setMinimumWidth(460);
		QFormLayout *form = new QFormLayout();

		platform_ = new QComboBox();
		const std::vector<PlatformPreset> &p = platform_presets();
		for (size_t i = 0; i < p.size(); ++i)
			platform_->addItem(qs(p[i].label), qs(p[i].id));
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
			key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
			reveal_->setText(on ? T("Dialog.Hide") : T("Dialog.Reveal"));
		});

		handle_ = new QLineEdit(qs(d.handle));
		handle_->setMaxLength(100);
		handle_->setPlaceholderText(T("Dialog.HandleHint"));
		form->addRow(T("Dialog.Handle"), handle_);

		encoder_ = new QComboBox();
		encoder_->addItem(T("Dialog.EncShared"), "shared");
		encoder_->addItem(T("Dialog.EncX264"), "x264");
		encoder_->setCurrentIndex(d.encoder == "x264" ? 1 : 0);
		form->addRow(T("Dialog.Encoder"), encoder_);

		bitrate_ = new QSpinBox();
		bitrate_->setRange(300, 51000);
		bitrate_->setSuffix(" kbps");
		bitrate_->setValue(d.bitrate_kbps >= 300 ? d.bitrate_kbps : 2500);
		form->addRow(T("Dialog.Bitrate"), bitrate_);

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

		QVBoxLayout *root = new QVBoxLayout(this);
		root->addLayout(form);
		root->addWidget(error_);
		root->addWidget(bb);

		prev_default_ = default_url_for(current_platform());
		QObject::connect(platform_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { on_platform_changed(); });
		QObject::connect(encoder_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](int) { update_enabled(); });
		update_hint();
		update_enabled();
	}

	Destination result() const { return dest_; }

private:
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
		bitrate_->setEnabled(ss(encoder_->currentData().toString()) == "x264");
		bitrate_->setToolTip(bitrate_->isEnabled() ? QString() : T("Dialog.BitrateShared"));
	}
	void on_platform_changed()
	{
		std::string cur = ss(url_->text().trimmed());
		std::string next = default_url_for(current_platform());
		if (cur.empty() || cur == prev_default_)
			url_->setText(qs(next));
		prev_default_ = next;
		update_hint();
	}
	void try_accept()
	{
		Destination d = dest_;
		d.platform = current_platform();
		d.name = ss(name_->text().trimmed());
		d.url = ss(url_->text().trimmed());
		d.handle = ss(handle_->text().trimmed());
		d.encoder = ss(encoder_->currentData().toString());
		d.bitrate_kbps = d.encoder == "x264" ? bitrate_->value() : 0;
		d.enabled = enabled_->isChecked();
		d.auto_start = auto_->isChecked();
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
	std::string prev_default_;
	QComboBox *platform_;
	QLineEdit *name_, *url_, *key_, *handle_;
	QLabel *hint_, *error_;
	QPushButton *reveal_;
	QComboBox *encoder_;
	QSpinBox *bitrate_;
	QCheckBox *enabled_, *auto_;
};

} // namespace

// ---------------------------------------------------------------------------------------------

MultistreamDock::MultistreamDock(QWidget *parent) : QWidget(parent)
{
	QVBoxLayout *root = new QVBoxLayout(this);
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

	table_ = new QTableWidget(0, 8);
	QStringList heads;
	heads << T("Col.On") << T("Col.Status") << T("Col.Name") << T("Col.Handle") << T("Col.Bitrate") << T("Col.Dropped")
	      << T("Col.Uptime") << QString();
	table_->setHorizontalHeaderLabels(heads);
	table_->verticalHeader()->hide();
	table_->setSelectionBehavior(QAbstractItemView::SelectRows);
	table_->setSelectionMode(QAbstractItemView::SingleSelection);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->horizontalHeader()->setStretchLastSection(false);
	table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
	root->addWidget(table_, 1);

	QHBoxLayout *row1 = new QHBoxLayout();
	QPushButton *add = new QPushButton(T("Btn.Add"));
	edit_btn_ = new QPushButton(T("Btn.Edit"));
	remove_btn_ = new QPushButton(T("Btn.Remove"));
	QPushButton *start_all = new QPushButton(T("Btn.StartAll"));
	QPushButton *stop_all = new QPushButton(T("Btn.StopAll"));
	row1->addWidget(add);
	row1->addWidget(edit_btn_);
	row1->addWidget(remove_btn_);
	row1->addStretch(1);
	row1->addWidget(start_all);
	row1->addWidget(stop_all);
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
	QObject::connect(start_all, &QPushButton::clicked, []() { plugin().mgr->start_all(); });
	QObject::connect(stop_all, &QPushButton::clicked, []() { plugin().mgr->stop_all(); });
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
		QPushButton *btn = new QPushButton();
		QObject::connect(btn, &QPushButton::clicked, [id, btn]() {
			if (btn->property("act").toString() == "stop")
				plugin().mgr->stop_destination(id);
			else
				plugin().mgr->start_destination(id);
		});
		table_->setCellWidget(row, 7, btn);
		for (int c = 1; c <= 6; ++c)
			table_->setItem(row, c, new QTableWidgetItem());
	}
	table_->resizeColumnsToContents();
}

void MultistreamDock::refresh()
{
	PluginState &st = plugin();
	std::vector<RuntimeView> views = st.mgr->views();

	std::string banner = st.mgr->bottleneck_banner();
	banner_->setVisible(!banner.empty());
	banner_->setText(qs(banner));
	notice_->setVisible(!st.notice.empty());
	notice_->setText(qs(st.notice));
	std::string url = plugin_overlay_url();
	url_label_->setText(url.empty() ? T("Msg.NoServer") : qs(url));

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
		if (!v.last_error.empty())
			tip += (tip.isEmpty() ? "" : "\n") + qs(v.last_error);
		status->setToolTip(tip);
		table_->item(row, 2)->setText(qs(v.dest.name));
		table_->item(row, 3)->setText(qs(v.dest.handle));
		bool live = v.state == State::Live || v.state == State::Degraded;
		table_->item(row, 4)->setText(live ? QString("%1 kbps").arg(static_cast<int>(v.bitrate_kbps)) : "-");
		table_->item(row, 5)->setText(live ? QString("%1 %").arg(v.dropped_pct, 0, 'f', 1) : "-");
		table_->item(row, 6)->setText(fmt_uptime(v.uptime_ms));
		QPushButton *btn = qobject_cast<QPushButton *>(table_->cellWidget(row, 7));
		if (btn) {
			bool stop = v.can_stop;
			btn->setProperty("act", stop ? "stop" : "start");
			btn->setText(stop ? T("Btn.Stop") : T("Btn.Start"));
			btn->setEnabled(stop || v.can_start);
		}
	}
}

} // namespace oms
