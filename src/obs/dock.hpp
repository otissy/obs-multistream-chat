// Qt6 dock for the streamer. No Q_OBJECT anywhere (functor connects only), like the other
// production-suite plugins, so the build has no moc step.
#pragma once
#include <QWidget>

#include <string>
#include <vector>

class QLabel;
class QPushButton;
class QTableWidget;

namespace oms {

class ChatPanel;
struct RuntimeView;

class MultistreamDock : public QWidget {
public:
	explicit MultistreamDock(QWidget *parent);
	void refresh(); // every 500 ms from the plugin timer

private:
	void rebuild_rows(const std::vector<std::string> &ids);
	void add_destination();
	void edit_selected();
	void remove_selected();
	void copy_overlay_url();
	std::string selected_id() const;
	void save_and_sync();
	void update_glance(const std::vector<RuntimeView> &views);

	QLabel *banner_;
	QLabel *notice_;
	QLabel *double_;
	QTableWidget *table_;
	QLabel *url_label_;
	QPushButton *big_btn_;
	QLabel *glance_;
	QPushButton *edit_btn_;
	QPushButton *remove_btn_;
	std::vector<std::string> row_ids_;
	ChatPanel *chat_panel_ = nullptr;
};

} // namespace oms
