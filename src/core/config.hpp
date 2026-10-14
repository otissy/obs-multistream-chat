// config.json: schema, load (with migration and validation), save (atomic, keys sealed).
#pragma once
#include "clock.hpp"
#include "destination.hpp"
#include "secret.hpp"

#include <string>
#include <vector>

namespace oms {

constexpr int kConfigSchema = 3;
constexpr int kMaxChatSources = 10;
constexpr size_t kMaxChatListEntries = 200;

struct ServerConfig {
	int port = 0;      // 0 = pick a random free port once and remember it in last_port
	int last_port = 0; // the port actually used last run
};

// One chat page the reader watches. target is a bare channel / video / broadcast id (see
// chat_targets.hpp), never a URL. No login, token or cookie is ever stored here.
struct ChatSource {
	std::string id; // c1, c2... [A-Za-z0-9_-]{1,32}
	std::string platform;
	std::string target;
	bool enabled = true;
};

struct ChatConfig {
	bool start_with_obs = false;      // start the reader when OBS starts (default: only on the button)
	std::string node_path;            // "" = look for node.exe on PATH
	int quiet_minutes = 5;            // zero messages for this long -> "quiet, layout changed or login needed"
	bool hide_commands = false;
	bool hide_bots = false;
	std::vector<std::string> blocked_words;
	std::vector<std::string> blocked_users;
	std::vector<ChatSource> sources;
};

struct Config {
	int schema = kConfigSchema;
	ServerConfig server;
	bool auto_start_with_stream = true;
	std::vector<Destination> destinations;
	ChatConfig chat;
};

enum class LoadStatus {
	Ok,        // read and valid
	Created,   // no file: defaults
	Migrated,  // older schema converted in memory; save to persist
	Malformed, // unreadable JSON or wrong shape: defaults, original moved aside (file loader)
	TooNew,    // written by a newer plugin: defaults, read_only = true, never overwritten
	IoError    // file exists but could not be read
};

struct LoadResult {
	LoadStatus status = LoadStatus::Created;
	Config config;
	bool read_only = false;  // caller must not save
	bool needs_save = false; // migrated or sealed something: caller should save
	std::string message;     // for the dock notice; never contains key material
	std::vector<std::string> warnings; // per-destination problems that were skipped
};

// Pure: text in, result out. Plaintext keys found in the text (schema 1, or a hand-edited "key")
// are sealed with `protector` immediately and the plaintext wiped.
LoadResult parse_config(const std::string &text, const SecretProtector &protector);

// File wrapper: missing file -> Created; malformed -> the file is renamed to <path>.bad-<now_ms>
// and defaults returned; TooNew and IoError are left untouched.
LoadResult load_config_file(const std::string &path, const SecretProtector &protector, const Clock &clock);

// Seals every destination whose plaintext `key` is set (key_token := protect(key); key wiped).
bool seal_keys(Config &cfg, const SecretProtector &protector, std::string &err);

// Fails (returns false) if any plaintext key remains: nothing unsealed can reach disk.
bool serialize_config(const Config &cfg, std::string &out, std::string &err);
bool save_config_file(const std::string &path, const Config &cfg, std::string &err);

// Decrypts d.key_token. false: no token, foreign token, or another Windows user/machine.
bool unlock_key(const Destination &d, const SecretProtector &protector, std::string &plain);

// "" or an absolute path ending in node.exe without quotes or control characters.
bool valid_node_path(const std::string &p);

// The file the reader gets with --config (never contains a token): {"v":1,"port":N,"quiet_minutes":N,
// "sources":[{"id","platform","target"}]} for the enabled sources only.
std::string build_reader_config(const Config &cfg, int port);

bool read_file(const std::string &path, std::string &out);
bool write_file_atomic(const std::string &path, const std::string &data, std::string &err);

} // namespace oms
