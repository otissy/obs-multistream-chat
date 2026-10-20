// The chat side of the dock (0.5.0): three pages the dock puts in its own tab bar (Chat = the live merged
// feed, Chat sources = channels + reader + filters, Voice = voice reader and chime) and a footer line
// (version, updates). This widget itself stays hidden; it owns the pages and the timers.
// No Q_OBJECT (functor connects only), like the rest of the dock.
#pragma once
#include "chat_hub.hpp"
#include "tts.hpp"

#include <QImage>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QSlider;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class QTimer;

namespace oms {

QImage create_platform_icon(const std::string &platform, int sz = 36);

class ChatPanel : public QWidget {
public:
	explicit ChatPanel(QWidget *parent);
	void refresh(); // every 500 ms from the plugin timer
	QWidget *live_page() const { return live_page_; }
	QWidget *sources_page() const { return sources_page_; }
	QWidget *voice_page() const { return voice_page_; }
	QWidget *footer() const { return footer_; }
	// the dock's tab bar: the unread count goes on tab `live_index`; go_sources switches to the Chat sources tab
	void attach(QTabWidget *tabs, int live_index, std::function<void()> go_sources);

private:
	void poll_chat();
	void append_message(const ChatMessage &m);
	void clear_chat();
	void set_font_scale(int delta);
	void rebuild_rows(const std::vector<std::string> &ids);
	void add_source();
	void edit_selected();
	void remove_selected();
	void toggle_reader();
	void apply_filters();
	std::string selected_id() const;
	void add_quick();
	void add_from_destinations();
	void update_add_hint();
	void show_add_info(const QString &text, bool good);
	void start_reader_if_wanted();
	void reset_unread();
	void play_tts(const std::string &author, const std::string &platform, const std::string &message);
	void play_alert_sound(const std::string &type);
	void check_for_updates();
	void save();

	// pages (owned by the dock's tab bar once attached)
	QWidget *live_page_ = nullptr;
	QWidget *sources_page_ = nullptr;
	QWidget *voice_page_ = nullptr;
	QWidget *footer_ = nullptr;
	QTabWidget *host_tabs_ = nullptr;
	int live_index_ = 0;
	QString live_label_;
	std::function<void()> go_sources_;
	QWidget *empty_chat_ = nullptr;

	// --- Live Chat tab widgets ---
	QTextBrowser *chat_browser_;
	QLabel *live_reader_status_;
	QPushButton *live_reader_btn_;
	QPushButton *pause_scroll_btn_;
	QPushButton *clear_btn_;
	QPushButton *font_smaller_btn_;
	QPushButton *font_bigger_btn_;
	QLabel *font_size_label_;
	QComboBox *add_platform_ = nullptr;
	QLineEdit *add_input_ = nullptr;
	QLabel *add_hint_ = nullptr;
	QLabel *add_info_ = nullptr;
	QPushButton *from_dest_btn_ = nullptr;
	bool autoscroll_ = true;
	int font_size_ = 14;
	uint64_t last_seq_ = 0;
	int unread_count_ = 0;
	QTimer *chat_poll_timer_ = nullptr;

	// --- Sources & Settings tab widgets ---
	QLabel *notice_;
	QLabel *reader_state_;
	QTableWidget *table_;
	QPushButton *reader_btn_;
	QPushButton *edit_btn_;
	QPushButton *remove_btn_;
	QComboBox *start_mode_ = nullptr;
	QCheckBox *hide_commands_;
	QCheckBox *hide_bots_;
	QPlainTextEdit *words_;
	QPlainTextEdit *users_;
	std::vector<std::string> row_ids_;
	bool filters_loaded_ = false;

	// --- Voice & Alerts (TTS) tab widgets ---
	QCheckBox *tts_enabled_ = nullptr;
	QComboBox *tts_voice_ = nullptr;
	QSlider *tts_volume_ = nullptr;
	QSlider *tts_speed_ = nullptr;
	QPushButton *tts_test_btn_ = nullptr;
	QCheckBox *tts_include_user_ = nullptr;
	QCheckBox *tts_include_platform_ = nullptr;
	QCheckBox *sound_alert_enabled_ = nullptr;
	QComboBox *sound_alert_type_ = nullptr;
	QPushButton *sound_test_btn_ = nullptr;
	QPushButton *check_updates_btn_ = nullptr;
	QLabel *update_status_lbl_ = nullptr;
	QPushButton *download_update_btn_ = nullptr;
	std::unique_ptr<ChatSpeaker> speaker_; // created on the first line to read
};

} // namespace oms
