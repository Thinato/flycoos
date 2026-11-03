#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "session.h"

// Player saves (.flys, ADR-025): what the play changed on top of the authored world file, which
// only the editor writes. The player, their inventory and the NPCs (keyed by spawn marker id and
// slot), in the .flyw's tagged-section style; see docs/ARCHITECTURE.md. Every new runtime state
// gets its own section and a round-trip test.

// What a load restored and dropped, for the log and the debug window's Save tab.
struct SaveReport {
    std::string world;          // the world file the save was made on
    bool worldChanged = false;  // that file's checksum isn't the one of the world loaded now
    bool fromBackup = false;    // the save didn't load; its .bak did
    int npcs = 0, droppedNpcs = 0;
    std::vector<std::string> notes;  // one line per warning: world changed, NPCs dropped, items moved or lost
};

// Where a world's player save lives: next to it, worlds/story.flyw -> worlds/story.flys.
inline std::filesystem::path savePathFor(const std::filesystem::path& world) {
    return std::filesystem::path(world).replace_extension(".flys");
}
// The checksum a world file ends with, which hashes all of its content; 0 if it can't be read.
uint32_t worldFileChecksum(const char* path);

// Both return an empty string on success, else why it failed. Saving is durable (replaceFile):
// the previous save is kept as PATH.bak. `worldName` and `worldHash` (worldFileChecksum) name
// the world it belongs to.
std::string savePlayer(const char* path, const Session& s, const std::string& worldName, uint32_t worldHash);
// Takes the save's time of day (spawns may depend on it), resets `s` as startSession does (keeping the
// player's movement tuning), then applies the save: the clock, the player, the inventory and every
// NPC whose spawn marker and slot still spawn its kind; the rest, and items or creatures of ids the
// data doesn't know, are dropped with a note. Missing sections keep startSession's state.
// A file that doesn't parse exactly (or fails a bound) falls back to PATH.bak; if that fails too,
// `s` is untouched. `worldHash` is the checksum of the world loaded now.
std::string loadPlayer(const char* path, Session& s, uint32_t worldHash, SaveReport& report);
