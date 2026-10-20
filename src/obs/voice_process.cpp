#include "voice_process.hpp"

#include <cwchar>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oms {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
	return w;
}

bool has_bad_char(const std::string &s)
{
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c < 0x20 || c == '"' || c == '%')
			return true;
	}
	return false;
}

bool locate_node(const std::string &node_path, std::wstring &node, std::string &err)
{
	if (!node_path.empty()) {
		node = widen(node_path);
		if (GetFileAttributesW(node.c_str()) == INVALID_FILE_ATTRIBUTES) {
			err = "node.exe was not found at the configured path";
			return false;
		}
		return true;
	}
	wchar_t buf[MAX_PATH * 2];
	DWORD n = SearchPathW(nullptr, L"node.exe", nullptr, static_cast<DWORD>(sizeof buf / sizeof buf[0]), buf, nullptr);
	if (n == 0 || n >= sizeof buf / sizeof buf[0]) {
		err = "Node.js (node.exe) was not found on PATH. Install Node 22 or newer, or set its path in the config.";
		return false;
	}
	node = buf;
	return true;
}

} // namespace

VoiceProcess::VoiceProcess() : process_(nullptr), job_(nullptr), stdin_write_(nullptr), stdout_read_(nullptr) {}

VoiceProcess::~VoiceProcess()
{
	stop_blocking(1500);
}

void VoiceProcess::release()
{
	if (stdin_write_) {
		CloseHandle(static_cast<HANDLE>(stdin_write_));
		stdin_write_ = nullptr;
	}
	if (job_) {
		CloseHandle(static_cast<HANDLE>(job_)); // kill-on-close: the helper goes, so its stdout ends
		job_ = nullptr;
	}
	if (reader_.joinable())
		reader_.join();
	if (stdout_read_) {
		CloseHandle(static_cast<HANDLE>(stdout_read_));
		stdout_read_ = nullptr;
	}
	if (process_) {
		CloseHandle(static_cast<HANDLE>(process_));
		process_ = nullptr;
	}
}

bool VoiceProcess::start(const std::string &node_path, const std::string &voice_js, const std::string &packs_dir,
			 const std::string &log_file, OnFrame on_frame, std::string &err)
{
	if (running()) {
		err = "the voice helper is already running";
		return false;
	}
	release();
	if (has_bad_char(voice_js) || has_bad_char(packs_dir) || has_bad_char(log_file) || has_bad_char(node_path)) {
		err = "a path contains characters that cannot be passed safely";
		return false;
	}
	std::wstring node;
	if (!locate_node(node_path, node, err))
		return false;
	std::wstring js = widen(voice_js);
	if (GetFileAttributesW(js.c_str()) == INVALID_FILE_ATTRIBUTES) {
		err = "voice\\index.js is missing from the plugin folder";
		return false;
	}

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;
	HANDLE in_rd = nullptr, in_wr = nullptr, out_rd = nullptr, out_wr = nullptr;
	if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) {
		err = "could not create a pipe";
		return false;
	}
	if (!CreatePipe(&out_rd, &out_wr, &sa, 1 << 20)) {
		CloseHandle(in_rd);
		CloseHandle(in_wr);
		err = "could not create a pipe";
		return false;
	}
	SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
	HANDLE log = CreateFileW(widen(log_file).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
				 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log == INVALID_HANDLE_VALUE)
		log = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
	auto close_all = [&]() {
		CloseHandle(in_rd);
		CloseHandle(in_wr);
		CloseHandle(out_rd);
		CloseHandle(out_wr);
		if (log != INVALID_HANDLE_VALUE)
			CloseHandle(log);
	};
	if (log == INVALID_HANDLE_VALUE) {
		close_all();
		err = "could not open the voice helper log";
		return false;
	}

	// environment: the current one minus the chat token (the helper never talks to the chat server)
	std::vector<wchar_t> env;
	LPWCH cur = GetEnvironmentStringsW();
	if (cur) {
		for (const wchar_t *p = cur; *p;) {
			size_t len = wcslen(p);
			if (_wcsnicmp(p, L"OMSC_CHAT_TOKEN=", 16) != 0)
				env.insert(env.end(), p, p + len + 1);
			p += len + 1;
		}
		FreeEnvironmentStringsW(cur);
	}
	if (env.empty())
		env.push_back(L'\0');
	env.push_back(L'\0');

	std::wstring cmd = L"\"" + node + L"\" \"" + js + L"\" --packs \"" + widen(packs_dir) + L"\"";
	std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
	cmdbuf.push_back(L'\0');
	std::wstring cwd = js.substr(0, js.find_last_of(L"\\/"));

	// only the three handles the child needs are inherited
	HANDLE inherit[3] = {in_rd, out_wr, log};
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
	std::vector<char> attr_mem(attr_size);
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_mem.data());
	bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
			UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(HANDLE) * 3, nullptr, nullptr);
	STARTUPINFOEXW si;
	ZeroMemory(&si, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = SW_HIDE;
	si.StartupInfo.hStdInput = in_rd;
	si.StartupInfo.hStdOutput = out_wr;
	si.StartupInfo.hStdError = log;
	si.lpAttributeList = attrs;
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof pi);
	DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
	BOOL ok = CreateProcessW(node.c_str(), cmdbuf.data(), nullptr, nullptr, TRUE, flags, env.data(), cwd.c_str(),
				 &si.StartupInfo, &pi);
	DWORD gle = GetLastError();
	if (attrs_ok)
		DeleteProcThreadAttributeList(attrs);
	CloseHandle(in_rd);
	CloseHandle(out_wr);
	CloseHandle(log);
	if (!ok) {
		CloseHandle(in_wr);
		CloseHandle(out_rd);
		err = "could not start node.exe for the voice helper (Windows error " + std::to_string(gle) + ")";
		return false;
	}
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	bool job_ok = false;
	if (job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
		ZeroMemory(&info, sizeof info);
		info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		job_ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info) &&
			 AssignProcessToJobObject(job, pi.hProcess);
	}
	if (!job_ok) {
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		CloseHandle(in_wr);
		CloseHandle(out_rd);
		if (job)
			CloseHandle(job);
		err = "could not put the voice helper in a job object";
		return false;
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	process_ = pi.hProcess;
	job_ = job;
	stdin_write_ = in_wr;
	stdout_read_ = out_rd;

	reader_ = std::thread([out_rd, on_frame]() {
		voice::FrameDecoder dec;
		std::vector<voice::Frame> frames;
		std::vector<char> buf(64 * 1024);
		for (;;) {
			DWORD got = 0;
			if (!ReadFile(out_rd, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0)
				break; // the helper ended (or was killed)
			frames.clear();
			bool ok = dec.push(buf.data(), got, frames);
			for (const voice::Frame &f : frames)
				on_frame(f);
			if (!ok)
				break; // garbage on stdout: stop reading; the next say() restarts the helper
		}
	});
	return true;
}

bool VoiceProcess::send(const std::string &line)
{
	if (!stdin_write_ || !running())
		return false;
	DWORD wrote = 0;
	BOOL ok = WriteFile(static_cast<HANDLE>(stdin_write_), line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
	return ok && wrote == line.size();
}

bool VoiceProcess::running()
{
	if (!process_)
		return false;
	return WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
}

void VoiceProcess::stop_blocking(int wait_ms)
{
	if (stdin_write_) {
		CloseHandle(static_cast<HANDLE>(stdin_write_)); // the helper leaves when stdin closes
		stdin_write_ = nullptr;
	}
	if (process_)
		WaitForSingleObject(static_cast<HANDLE>(process_), static_cast<DWORD>(wait_ms));
	release();
}

#else // !_WIN32

VoiceProcess::VoiceProcess() : process_(nullptr), job_(nullptr), stdin_write_(nullptr), stdout_read_(nullptr) {}
VoiceProcess::~VoiceProcess() {}
void VoiceProcess::release() {}
bool VoiceProcess::start(const std::string &, const std::string &, const std::string &, const std::string &, OnFrame,
			 std::string &err)
{
	err = "the voice helper runs on Windows only";
	return false;
}
bool VoiceProcess::send(const std::string &)
{
	return false;
}
bool VoiceProcess::running()
{
	return false;
}
void VoiceProcess::stop_blocking(int) {}

#endif

} // namespace oms
