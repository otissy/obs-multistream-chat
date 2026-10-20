// Setup flow (0.5.0): the "Get started" checklist and the ways a chat source is added.
// Portable: no OBS, no Qt. The dock and the chat panel call these; tests call them directly.
#pragma once

#include "config.hpp"

#include <string>
#include <vector>

namespace oms {

// The three first-run steps the dock's "Get started" card shows, in order.
struct SetupProgress {
	bool destination = false; // 1. at least one stream destination
	bool chat = false;        // 2. at least one chat source
	bool overlay = false;     // 3. an overlay URL was copied once (remembered by the dock, not in config.json)
	int done() const { return (destination ? 1 : 0) + (chat ? 1 : 0) + (overlay ? 1 : 0); }
	bool complete() const { return done() == 3; }
};

SetupProgress setup_progress(const Config &cfg, bool overlay_copied);

// The first free id "c1", "c2", ... (never one already in `sources`, whatever order they are in).
std::string next_chat_source_id(const std::vector<ChatSource> &sources);

enum class AddResult { Added, Enabled, Duplicate, Invalid, Full };

// Adds one chat source from what the user typed (a channel, @user, video id or the platform's own link).
// The same platform + target already there: enabled again (Enabled) or left alone (Duplicate), never twice.
// `target_out` gets the normalised target, `err` the reason for Invalid.
AddResult add_chat_source(std::vector<ChatSource> &sources, const std::string &platform, const std::string &input,
			  std::string &target_out, std::string &err);

// "Add from my stream destinations": one chat source per destination whose platform has a chat by channel
// name (Twitch, Kick, TikTok), from its handle (else its name). Add-only: an existing source is never changed
// or replaced, so a collaborator's channel on the same platform stays. Returns how many were added.
int add_sources_from_destinations(const std::vector<Destination> &dests, std::vector<ChatSource> &sources);

// Reader start mode, one choice instead of two checkboxes: 0 = manually, 1 = when OBS starts, 2 = when I go live.
int reader_start_mode(const ChatConfig &c);
void set_reader_start_mode(ChatConfig &c, int mode);

} // namespace oms
