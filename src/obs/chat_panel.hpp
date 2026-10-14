// The "Chat" tab of the dock: chat sources, reader start/stop, filters, overlay URL.
// No Q_OBJECT (functor connects only), like the rest of the dock.
#pragma once
#include <QWidget>

#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace oms {

class ChatPanel : public QWidget {
public:
	explicit ChatPanel(QWidget *parent);
	void refresh(); // every 500 ms from the plugin timer

private:
	void rebuild_rows(const std::vector<std::string> &ids);
	void add_source();
	void edit_selected();
	void remove_selected();
	void toggle_reader();
	void apply_filters();
	void copy_chat_url();
	std::string selected_id() const;
	void save();

	QLabel *notice_;
	QLabel *reader_state_;
	QTableWidget *table_;
	QPushButton *reader_btn_;
	QPushButton *edit_btn_;
	QPushButton *remove_btn_;
	QCheckBox *start_with_obs_;
	QCheckBox *hide_commands_;
	QCheckBox *hide_bots_;
	QPlainTextEdit *words_;
	QPlainTextEdit *users_;
	QLabel *url_label_;
	std::vector<std::string> row_ids_;
	bool filters_loaded_ = false;
};

} // namespace oms
