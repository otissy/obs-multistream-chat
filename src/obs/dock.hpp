// Qt6 dock for the streamer. No Q_OBJECT anywhere (functor connects only), like the other
// production-suite plugins, so the build has no moc step.
#pragma once
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QTabWidget;
class QTableWidget;

namespace oms {

class ChatPanel;
struct RuntimeView;
struct Config;

class MultistreamDock : public QWidget {
public:
	explicit MultistreamDock(QWidget *parent);
	void refresh(); // every 500 ms from the plugin timer
	// OMSC_SHOTS: renders each page and dialog of the real dock to PNGs in `dir` (README step pictures)
	static void capture_screens(const QString &dir, bool demo);

private:
	void rebuild_rows(const std::vector<std::string> &ids);
	void add_destination();
	void edit_selected();
	void remove_selected();
	void copy_overlay_url(int which); // 0 = chat, 1 = platform icons
	QString overlay_url(int which) const;
	void update_setup(const Config &cfg);
	void show_audio_tutorial();
	std::string selected_id() const;
	void save_and_sync();
	void update_glance(const std::vector<RuntimeView> &views);

	QLabel *banner_;
	QLabel *notice_;
	QLabel *double_;
	QTableWidget *table_;
	QTabWidget *tabs_ = nullptr;
	int tab_chat_ = 0, tab_streams_ = 1, tab_sources_ = 2, tab_overlays_ = 3, tab_voice_ = 4;
	QLabel *streams_empty_ = nullptr;
	QFrame *setup_card_ = nullptr;
	QLabel *setup_title_ = nullptr;
	QLabel *step_lbl_[3] = {};
	QPushButton *step_btn_[3] = {};
	QString step_text_[3];
	QLabel *ov_url_[2] = {};
	QComboBox *ov_theme_[2] = {};
	QLabel *ov_copied_[2] = {};
	QPushButton *big_btn_;
	QLabel *glance_;
	QPushButton *edit_btn_;
	QPushButton *remove_btn_;
	std::vector<std::string> row_ids_;
	ChatPanel *chat_panel_ = nullptr;
};

} // namespace oms
