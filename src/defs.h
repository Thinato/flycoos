#pragma once
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "item.h"

// Definitions loaded from data/ (ADR-018): one JSON file per type, each an array of records keyed
// by a snake_case string id. Runtime code holds dense handles (an ItemId is the record's save_id),
// never strings. Move-only: the ItemType names point into `itemNames`.
struct Defs {
    std::vector<ItemType> items{ItemType{"", SlotNone, {0, 0, 0}, 0, 0}};  // by save id; 0 is NoItem
    std::vector<std::string> itemIds{""}, itemNames{""};                     // parallel to `items`
    std::map<std::string, ItemId, std::less<>> itemById;                     // string id -> save id, for references

    Defs() = default;
    Defs(Defs&&) = default;
    Defs& operator=(Defs&&) = default;
};

// data/ in the working folder, else the source tree's (FLYCOOS_SOURCE_DIR).
std::filesystem::path dataDir();

// Parses every definition file in `dir` (pass 1), then resolves `copy_from` and checks every field
// and reference (pass 2). On failure returns false, leaves `out` alone, and `err` names the file,
// record and field.
bool loadDefs(const std::filesystem::path& dir, Defs& out, std::string& err);

// The registry itemType() reads; empty (NoItem only) until set. Swap it only between ticks.
const Defs& defs();
void setDefs(Defs&& d);
// Loads `dir` and swaps it in, unless it fails or would change what live ids mean: every loaded
// save id must keep its string id. On a refusal the current defs stay and `err` says why.
bool reloadDefs(const std::filesystem::path& dir, std::string& err);
