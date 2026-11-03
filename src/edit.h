#pragma once
#include <optional>
#include <vector>

#include "world.h"

// World editing: one data-driven entry point (an EditOp in, the changed box and an undo record
// out) shared by the in-game editor and, later, remote commands. See docs/ARCHITECTURE.md.

// Inclusive voxel box; empty when hi < lo on any axis.
struct Box {
    glm::ivec3 lo{0}, hi{-1};
    bool empty() const { return glm::any(glm::lessThan(hi, lo)); }
    glm::ivec3 size() const { return hi - lo + 1; }
};

// Brushes work on whole columns inside a circle (center, radius) around the column's ground top,
// its highest opaque voxel (so trees and rocks count as terrain). Water is never created or
// drained: lowering a column that is under water fills the removed cells with water, lowering dry
// land leaves a dry pit even below sea level, and raising pushes ground up through water.
enum class Tool : uint8_t {
    Raise,    // add up to `strength` blocks on top (surface material on top, subsoil below), most at the center
    Lower,    // remove up to `strength` blocks from the top; the new top gets the old surface material
    Smooth,   // move columns toward their neighbourhood's average height, at most `strength` blocks
    Flatten,  // move columns toward ground top `height`, at most `strength` blocks
    Paint,    // recolor the surface voxel of every column to `block`
    Fill,     // set every voxel in [min, max] to `block` (Air clears)
    Count
};

struct EditOp {
    Tool tool = Tool::Raise;
    glm::ivec3 min{}, max{};  // Fill (inclusive; swapped corners are fine)
    glm::ivec2 center{};      // brushes: column x, z
    int radius = 6;           // brushes, 0..kMaxBrushRadius
    int strength = 2;         // brushes, 1..kMaxStrength
    int height = kSeaLevel;   // Flatten: target ground top y
    Block block = Grass;      // Fill and Paint (Paint needs an opaque block)
};

constexpr int kMaxBrushRadius = 32;
constexpr int kMaxStrength = 64;
constexpr int64_t kMaxEditVolume = int64_t(1) << 21;  // voxels a Fill may request (128^3); keeps one op ~ms
constexpr size_t kMaxUndo = 256;                      // oldest records are dropped beyond this
constexpr size_t kMaxUndoBytes = size_t(256) << 20;    // or beyond this much cell data (the newest step is always kept)

// One undo step: what each box held before its edit (undo stack) or after it (redo stack). Most
// steps are one edit; a remote command's edits form one step (see applyEdit's `join`).
struct EditRecord {
    struct Part {
        Box box;
        std::vector<Block> cells;  // x fastest, then z, then y
    };
    std::vector<Part> parts;  // in the order they were applied
    // Prefab instances the step placed, removed or moved: the id and that instance on the other side
    // of the step (none: it doesn't exist there). Swapped with the live list like the cells.
    std::vector<std::pair<uint32_t, std::optional<Instance>>> instances;
    size_t bytes = 0;  // cells in all parts, plus the instances' cell lists
};
struct EditHistory {
    std::vector<EditRecord> undo, redo;
};

struct EditResult {
    Box dirty;                    // exactly the voxels that changed; empty if nothing did
    const char* error = nullptr;  // set when the op was rejected (nothing changed)
    uint32_t id = 0;              // placePrefab: the instance's id
};

// Why the op would be rejected, or nullptr. applyEdit checks this first.
const char* checkEdit(const Grid& g, const EditOp& op);
// Every voxel the op may write (clamped to the world); applyEdit's cost grows with its volume.
Box editBox(const Grid& g, const EditOp& op);
// Applies the op. If anything changed, pushes an undo record (or, with `join`, adds to the last one)
// and clears the redo stack; the oldest records go past kMaxUndo steps or kMaxUndoBytes.
EditResult applyEdit(Grid& g, const EditOp& op, EditHistory& history, bool join = false);
// Restore the boxes and instances of the last undo/redo record exactly; return their bounding box
// (empty when there's none).
Box undoEdit(Grid& g, EditHistory& history, std::vector<Instance>& instances);
Box redoEdit(Grid& g, EditHistory& history, std::vector<Instance>& instances);

// Building blocks for edits outside applyEdit (prefab instances):
// a copy of the box (inside g), x fastest, then z, then y;
std::vector<Block> readBox(const Grid& g, Box b);
// the part of `box` that differs from its copy `before`, cropped to what changed (empty box: nothing);
EditRecord::Part changedPart(const Grid& g, Box box, const std::vector<Block>& before);
// push `rec` as a new undo step (or add it to the last one with `join`), clear the redo stack and
// drop the oldest steps past the caps.
void pushRecord(EditHistory& history, EditRecord rec, bool join);
// The bounding box of the voxels an instance changed.
Box instanceBox(const Instance& in);

// First opaque voxel along the ray (Amanatides-Woo DDA), with the face it was entered through
// (normal points back toward the ray; zero if `from` is inside it). Water is passed through.
struct RayHit {
    glm::ivec3 cell, normal;
    float distance;
};
std::optional<RayHit> raycast(const Grid& g, glm::vec3 from, glm::vec3 dir, float maxDistance);

// Highest opaque voxel of a column, or -1.
int groundTop(const Grid& g, int x, int z);
