#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "edit.h"

// Prefabs: ready-made trees, rocks, houses and camps, placed into the world as instances (Instance
// in world.h) that can be moved and removed later. Generated prefabs grow from a few parameters
// with the same code as terrain decoration; structures are hand-written text files in prefabs/
// (format in the README). See docs/ARCHITECTURE.md, "Prefabs".

constexpr Block kKeep = BlockCount;  // a structure cell that leaves the world's voxel as it is
constexpr int kMaxPrefabSize = 64;   // structure cells per axis
constexpr int kMaxFoundation = 32;   // how far a foundation reaches down to the ground
constexpr size_t kMaxPrefabFiles = 256;
constexpr uintmax_t kMaxPrefabFileBytes = 512 << 10;
constexpr uintmax_t kMaxPrefabBytes = 2 << 20;  // all files together, so a rescan stays a few ms
static_assert(kMaxPrefabSize + kMaxFoundation <= kMaxInstanceExtent);

struct PrefabParam {
    std::string name;
    float lo, hi, fallback;  // range and default
    bool whole = false;      // an integer (generators truncate)
};

struct Prefab {
    std::string name, description;
    std::vector<PrefabParam> params;  // generated prefabs (each has at least one)
    // Structures: cells x fastest, then z (a layer's rows), then y (layers, bottom first).
    glm::ivec3 size{0};
    std::vector<Block> cells;
    Block foundation = kKeep;  // fills down from the bottom layer's solid cells to the ground
    int y = 0;                 // the bottom layer sits at ground + 1 + y
    bool generated() const { return !params.empty(); }
};

struct PrefabLibrary {
    std::filesystem::path dir;        // where the structure files come from
    std::vector<Prefab> prefabs;      // the generated ones, then the files by name
    std::vector<std::string> errors;  // files that didn't load: "name.txt: line N: why"
    const Prefab* find(std::string_view name) const;
};

// Parses a structure file into `out` (its name is kept); returns "" or "line N: why".
std::string parsePrefab(std::string_view text, Prefab& out);
// The generated prefabs plus every regular .txt file directly in `dir` (within the caps above).
PrefabLibrary loadPrefabs(const std::filesystem::path& dir);

// Why placing `p` as `inst` describes (pos, rotation, params) would be rejected, or nullptr.
const char* checkPlacement(const Grid& g, const Prefab& p, const Instance& inst);
// Every voxel that placement may change, clipped to the world; empty if checkPlacement rejects it.
Box prefabBox(const Grid& g, const Prefab& p, const Instance& inst);

// One undo step each, restoring voxels and the instance list together.
// placePrefab stamps the prefab at column inst.pos.x, z standing on ground voxel `y` (default: the
// column's highest solid voxel) and adds the instance with its changed cells and a new id (or
// inst.id if set). It fails if nothing would change, or past kMaxInstances or kMaxInstanceCells.
EditResult placePrefab(Grid& g, const Prefab& p, Instance inst, std::optional<int> y, std::vector<Instance>& list,
                       EditHistory& h, bool join = false);
// Puts back what each voxel held before, where it still holds what the prefab wrote (so later
// edits on top stay).
EditResult removePrefab(Grid& g, uint32_t id, std::vector<Instance>& list, EditHistory& h);
// Remove, then place again at `column` with `rotation`, keeping the id, seed and params; the
// ground is found after the removal. On failure nothing changes, the history included.
EditResult movePrefab(Grid& g, const Prefab& p, uint32_t id, glm::ivec2 column, std::optional<int> y, int rotation,
                      std::vector<Instance>& list, EditHistory& h);

// The instance that wrote `cell` and still owns it, or nullptr.
const Instance* instanceAt(const std::vector<Instance>& list, const Grid& g, glm::ivec3 cell);
