// Owns the voice helper (node voice\index.js) as a hidden child process, the way ReaderProcess owns
// the chat reader:
//   - started only when there is a line to speak; no console window
//   - stdin is a pipe for request lines; stdout is a pipe of frames, read on a thread of our own;
//     stderr goes to voice-helper.log (diagnostics only, the helper never writes chat text there)
//   - the chat token is removed from its environment
//   - a Windows job object (kill-on-close) means that if OBS dies, the helper dies too
#pragma once
#include "voice.hpp"

#include <functional>
#include <string>
#include <thread>

namespace oms {

class VoiceProcess {
public:
	using OnFrame = std::function<void(const voice::Frame &)>; // called on the reader thread

	VoiceProcess();
	~VoiceProcess();
	VoiceProcess(const VoiceProcess &) = delete;
	VoiceProcess &operator=(const VoiceProcess &) = delete;

	// node_path: configured absolute path to node.exe, or "" to search PATH.
	bool start(const std::string &node_path, const std::string &voice_js, const std::string &packs_dir,
		   const std::string &log_file, OnFrame on_frame, std::string &err);
	bool send(const std::string &line); // false when not running or the pipe broke
	bool running();
	// Closes stdin (the helper leaves), waits up to wait_ms, then kills the job. Joins the reader.
	void stop_blocking(int wait_ms = 3000);

private:
	void release();
	void *process_;
	void *job_;
	void *stdin_write_;
	void *stdout_read_;
	std::thread reader_;
};

} // namespace oms
