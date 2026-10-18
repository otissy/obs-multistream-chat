// The "Chat" tab of the dock: live merged chat view, chat sources, reader start/stop, filters, overlay URL.
// No Q_OBJECT (functor connects only), like the rest of the dock.
#pragma once
#include "chat_hub.hpp"

#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class QTimer;

namespace oms {

class ChatPanel : public QWidget {
public:
	explicit ChatPanel(QWidget *parent);
	void refresh(); // every 500 ms from the plugin timer

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
	void copy_chat_url();
	std::string selected_id() const;
	void auto_connect_channel();
	void sync_from_destinations();
	void save();

	// Tabs inside Chat tab
	QTabWidget *tabs_;

	// --- Live Chat tab widgets ---
	QTextBrowser *chat_browser_;
	QLabel *live_reader_status_;
	QPushButton *live_reader_btn_;
	QPushButton *pause_scroll_btn_;
	QPushButton *clear_btn_;
	QPushButton *font_smaller_btn_;
	QPushButton *font_bigger_btn_;
	QLabel *font_size_label_;
	QLineEdit *channel_input_;
	QPushButton *auto_connect_btn_;
	QLabel *auto_connect_info_;
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
	QPushButton *sync_dest_btn_;
	QCheckBox *start_with_obs_;
	QCheckBox *auto_start_stream_;
	QCheckBox *hide_commands_;
	QCheckBox *hide_bots_;
	QPlainTextEdit *words_;
	QPlainTextEdit *users_;
	QLabel *url_label_;
	std::vector<std::string> row_ids_;
	bool filters_loaded_ = false;
};

} // namespace oms
