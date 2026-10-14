#include "reader_process.hpp"

#include <chrono>
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

namespace {

long long now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
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
#endif

} // namespace

std::string describe_reader_exit(int code)
{
	switch (code) {
	case 0: return "Chat reader stopped.";
	case 2: return "Chat reader could not start: its configuration or token was refused (exit 2). See chat-reader.log.";
	case 3: return "Chat reader was refused by the local chat server (exit 3). Press Start again.";
	case 4: return "Chat reader lost the local chat server (exit 4).";
	case 5: return "Chat reader could not start Google Chrome (exit 5). Is Chrome installed? See chat-reader.log.";
	case 1: return "Chat reader stopped by force.";
	default: return "Chat reader ended unexpectedly (exit " + std::to_string(code) + "). See chat-reader.log.";
	}
}

ReaderProcess::ReaderProcess() : process_(nullptr), job_(nullptr), stdin_write_(nullptr), stop_requested_at_(0), exit_code_(-1) {}

ReaderProcess::~ReaderProcess()
{
	stop_blocking(1500);
}

void ReaderProcess::release()
{
#ifdef _WIN32
	if (stdin_write_) {
		CloseHandle(static_cast<HANDLE>(stdin_write_));
		stdin_write_ = nullptr;
	}
	if (process_) {
		CloseHandle(static_cast<HANDLE>(process_));
		process_ = nullptr;
	}
	if (job_) {
		CloseHandle(static_cast<HANDLE>(job_)); // kill-on-close: nothing of ours survives
		job_ = nullptr;
	}
#endif
	stop_requested_at_ = 0;
}

#ifdef _WIN32

bool ReaderProcess::start(const std::string &node_path, const std::string &reader_js, const std::string &config_json,
			  const std::string &log_file, const std::string &token, std::string &err)
{
	if (running()) {
		err = "the chat reader is already running";
		return false;
	}
	release();
	exit_code_ = -1;
	if (token.size() < 16) {
		err = "no session token (random generator unavailable)";
		return false;
	}
	if (has_bad_char(reader_js) || has_bad_char(config_json) || has_bad_char(log_file) || has_bad_char(node_path)) {
		err = "a path contains characters that cannot be passed safely";
		return false;
	}
	std::wstring node;
	if (!node_path.empty()) {
		node = widen(node_path);
		if (GetFileAttributesW(node.c_str()) == INVALID_FILE_ATTRIBUTES) {
			err = "node.exe was not found at the configured path";
			return false;
		}
	} else {
		wchar_t buf[MAX_PATH * 2];
		DWORD n = SearchPathW(nullptr, L"node.exe", nullptr, static_cast<DWORD>(sizeof buf / sizeof buf[0]), buf, nullptr);
		if (n == 0 || n >= sizeof buf / sizeof buf[0]) {
			err = "Node.js (node.exe) was not found on PATH. Install Node 22 or newer, or set its path in the config.";
			return false;
		}
		node = buf;
	}
	std::wstring js = widen(reader_js);
	if (GetFileAttributesW(js.c_str()) == INVALID_FILE_ATTRIBUTES) {
		err = "reader\\index.js is missing from the plugin folder";
		return false;
	}

	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;
	HANDLE in_rd = nullptr, in_wr = nullptr;
	if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) {
		err = "could not create a pipe";
		return false;
	}
	SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
	HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
	if (nul == INVALID_HANDLE_VALUE) {
		CloseHandle(in_rd);
		CloseHandle(in_wr);
		err = "could not open NUL";
		return false;
	}

	// environment: the current one plus the token (and no stale token)
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
	std::wstring tok = L"OMSC_CHAT_TOKEN=" + widen(token);
	env.insert(env.end(), tok.c_str(), tok.c_str() + tok.size() + 1);
	env.push_back(L'\0');

	std::wstring cmd = L"\"" + node + L"\" \"" + js + L"\" --config \"" + widen(config_json) + L"\" --log \"" + widen(log_file) +
			   L"\" --exit-on-stdin-close";
	std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
	cmdbuf.push_back(L'\0');
	std::wstring cwd = js.substr(0, js.find_last_of(L"\\/"));

	// only the three handles the child needs are inherited
	HANDLE inherit[2] = {in_rd, nul};
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
	std::vector<char> attr_mem(attr_size);
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_mem.data());
	bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
			UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(HANDLE) * 2, nullptr, nullptr);
	STARTUPINFOEXW si;
	ZeroMemory(&si, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = SW_HIDE;
	si.StartupInfo.hStdInput = in_rd;
	si.StartupInfo.hStdOutput = nul;
	si.StartupInfo.hStdError = nul;
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
	CloseHandle(nul);
	SecureZeroMemory(env.data(), env.size() * sizeof(wchar_t)); // the token was in there
	SecureZeroMemory(&tok[0], tok.size() * sizeof(wchar_t));
	if (!ok) {
		CloseHandle(in_wr);
		err = "could not start node.exe (Windows error " + std::to_string(gle) + ")";
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
		// without the job a dead OBS could leave a Chrome behind: refuse rather than risk it
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		CloseHandle(in_wr);
		if (job)
			CloseHandle(job);
		err = "could not put the reader in a job object";
		return false;
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	process_ = pi.hProcess;
	job_ = job;
	stdin_write_ = in_wr;
	return true;
}

void ReaderProcess::request_stop()
{
	if (!process_)
		return;
	if (stdin_write_) {
		CloseHandle(static_cast<HANDLE>(stdin_write_)); // the reader leaves when its stdin ends
		stdin_write_ = nullptr;
	}
	if (!stop_requested_at_)
		stop_requested_at_ = now_ms();
}

bool ReaderProcess::running()
{
	if (!process_)
		return false;
	DWORD code = 0;
	if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT) {
		if (stop_requested_at_ && now_ms() - stop_requested_at_ > 6000) {
			TerminateJobObject(static_cast<HANDLE>(job_), 1);
			WaitForSingleObject(static_cast<HANDLE>(process_), 1000);
		} else {
			return true;
		}
	}
	GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
	exit_code_ = static_cast<int>(code);
	release();
	return false;
}

void ReaderProcess::stop_blocking(int wait_ms)
{
	if (!process_)
		return;
	request_stop();
	WaitForSingleObject(static_cast<HANDLE>(process_), static_cast<DWORD>(wait_ms));
	if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT)
		TerminateJobObject(static_cast<HANDLE>(job_), 1);
	DWORD code = 1;
	WaitForSingleObject(static_cast<HANDLE>(process_), 1000);
	GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
	exit_code_ = static_cast<int>(code);
	release();
}

#else

bool ReaderProcess::start(const std::string &, const std::string &, const std::string &, const std::string &, const std::string &,
			  std::string &err)
{
	err = "the chat reader is only available on Windows";
	return false;
}
void ReaderProcess::request_stop() {}
bool ReaderProcess::running() { return false; }
void ReaderProcess::stop_blocking(int) {}

#endif

} // namespace oms
