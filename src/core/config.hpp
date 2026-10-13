// config.json: schema, load (with migration and validation), save (atomic, keys sealed).
#pragma once
#include "clock.hpp"
#include "destination.hpp"
#include "secret.hpp"

#include <string>
#include <vector>

namespace oms {

constexpr int kConfigSchema = 2;

struct ServerConfig {
	int port = 0;      // 0 = pick a random free port once and remember it in last_port
	int last_port = 0; // the port actually used last run
};

struct Config {
	int schema = kConfigSchema;
	ServerConfig server;
	bool auto_start_with_stream = true;
	std::vector<Destination> destinations;
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

bool read_file(const std::string &path, std::string &out);
bool write_file_atomic(const std::string &path, const std::string &data, std::string &err);

} // namespace oms
