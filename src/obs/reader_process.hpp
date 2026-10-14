// Owns the chat reader (node reader\index.js) as a hidden child process.
//   - never started by itself: only by start() (the dock button, or "start with OBS" when ticked)
//   - no console window, stdout/stderr to NUL, stdin is a pipe: closing it asks the reader to leave
//   - the token goes in its environment only, never on the command line
//   - a Windows job object (kill-on-close) means that if OBS dies, the reader and its Chrome die too
#pragma once
#include <string>

namespace oms {

class ReaderProcess {
public:
	ReaderProcess();
	~ReaderProcess();
	ReaderProcess(const ReaderProcess &) = delete;
	ReaderProcess &operator=(const ReaderProcess &) = delete;

	// node_path: configured absolute path to node.exe, or "" to search PATH.
	bool start(const std::string &node_path, const std::string &reader_js, const std::string &config_json,
		   const std::string &log_file, const std::string &token, std::string &err);
	// Asks the reader to leave (closes its stdin); running() force-kills it after a few seconds.
	void request_stop();
	// Closes stdin, waits up to wait_ms, then kills the whole job. For OBS exit / module unload.
	void stop_blocking(int wait_ms = 4000);
	// true while the process is alive; also enforces the kill deadline after request_stop().
	bool running();
	// exit code of the last process that ended, -1 if none or still running
	int exit_code() const { return exit_code_; }

private:
	void release();
	void *process_;
	void *job_;
	void *stdin_write_;
	long long stop_requested_at_;
	int exit_code_;
};

// "reader exited with code 3" -> a sentence for the dock
std::string describe_reader_exit(int code);

} // namespace oms
