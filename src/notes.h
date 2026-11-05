#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "commands.h"
#include "session.h"

// Debug notes (F2 in a --debug session): the player writes what they saw, the game pauses and
// keeps the state of the moment the box opened, plus a screenshot. Notes go to
// debug_notes/notes.jsonl (one JSON object per line) with the PNGs next to it, for whoever works
// on the game next to read. Each note starts {"id": line, "status": "open"}; whoever handles it
// sets "status": "done" and "resolution" (the commit and what changed) in place.

constexpr float kNoteNearby = 48;  // NPCs within this many blocks of the player are listed
constexpr size_t kNoteNearbyMax = 12, kNoteEvents = 10;

// The state a note keeps: world, clock, player (place, motion, health, combat, stats, gear),
// camera, region, nearby NPCs (state, health, latest AI transitions), quests, flags and the latest
// events. `view` is what the `state` command reports.
nlohmann::json noteContext(const Session& s, const GameView& view);

// Appends {"id", "status": "open", "note": text, ...context} to `dir`/notes.jsonl, and the screenshot (RGB8, w x h; none
// when empty) as `dir`/<stamp>.png, named in the note's "screenshot". Returns the note's line
// number (1 = first), or 0 on failure with `error` set.
int saveNote(const std::filesystem::path& dir, const std::string& text, nlohmann::json context,
             const std::vector<uint8_t>& rgb, int w, int h, std::string& error);
