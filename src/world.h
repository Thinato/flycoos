#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// Block ids double as palette indices (kPalette below). They are saved in world files: only append.
// Water's id is also hard-coded as kWater in shaders/world.metal.
enum Block : uint8_t { Air, Grass, Dirt, Stone, Sand, Snow, Wood, Leaves, Water,
                       Skin, Tunic, Belt, Boots, Hair, Eye, Rock,
                       Robe, DarkHair, Straw, Burlap,  // NPC models
                       Steel, Iron,                    // items
                       Hide, Tusk, Snout, Hood, Leather,  // boar, bandit
                       Plate,                             // armor
                       Planks, Cobble, Thatch, Ember,     // prefabs
                       Brick, MossyCobble, DarkPlanks, Hay, Clay, Slate, Lantern,  // building (the palette holds 64)
                       BlockCount };

inline bool isOpaque(Block b) { return b != Air && b != Water; }

inline constexpr const char* kBlockNames[BlockCount] = {
    "Air", "Grass", "Dirt", "Stone", "Sand", "Snow", "Wood", "Leaves", "Water", "Skin", "Tunic", "Belt", "Boots", "Hair",
    "Eye", "Rock", "Robe", "DarkHair", "Straw", "Burlap", "Steel", "Iron", "Hide", "Tusk", "Snout", "Hood", "Leather", "Plate",
    "Planks", "Cobble", "Thatch", "Ember", "Brick", "MossyCobble", "DarkPlanks", "Hay", "Clay", "Slate", "Lantern"};

// Block colors in sRGB, indexed by Block. The renderer converts them to linear; the map command uses them as is.
inline constexpr float kPalette[BlockCount][3] = {
    {0, 0, 0},              // Air
    {0.36f, 0.62f, 0.24f},  // Grass
    {0.47f, 0.33f, 0.20f},  // Dirt
    {0.50f, 0.50f, 0.52f},  // Stone
    {0.80f, 0.72f, 0.48f},  // Sand
    {0.95f, 0.96f, 1.00f},  // Snow
    {0.42f, 0.28f, 0.15f},  // Wood
    {0.20f, 0.48f, 0.18f},  // Leaves
    {0.08f, 0.30f, 0.55f},  // Water
    {0.93f, 0.74f, 0.58f},  // Skin
    {0.25f, 0.45f, 0.72f},  // Tunic
    {0.35f, 0.22f, 0.12f},  // Belt
    {0.28f, 0.18f, 0.12f},  // Boots
    {0.80f, 0.45f, 0.20f},  // Hair
    {0.08f, 0.08f, 0.10f},  // Eye
    {0.52f, 0.50f, 0.47f},  // Rock
    {0.42f, 0.52f, 0.30f},  // Robe
    {0.25f, 0.17f, 0.12f},  // DarkHair
    {0.86f, 0.74f, 0.40f},  // Straw
    {0.66f, 0.55f, 0.38f},  // Burlap
    {0.78f, 0.80f, 0.84f},  // Steel
    {0.30f, 0.30f, 0.33f},  // Iron
    {0.36f, 0.25f, 0.18f},  // Hide
    {0.92f, 0.88f, 0.76f},  // Tusk
    {0.78f, 0.52f, 0.50f},  // Snout
    {0.42f, 0.12f, 0.10f},  // Hood
    {0.45f, 0.32f, 0.20f},  // Leather
    {0.50f, 0.52f, 0.56f},  // Plate
    {0.66f, 0.48f, 0.29f},  // Planks
    {0.45f, 0.44f, 0.43f},  // Cobble
    {0.56f, 0.45f, 0.26f},  // Thatch
    {1.00f, 0.42f, 0.10f},  // Ember
    {0.62f, 0.31f, 0.23f},  // Brick
    {0.40f, 0.47f, 0.33f},  // MossyCobble
    {0.50f, 0.33f, 0.20f},  // DarkPlanks
    {0.90f, 0.72f, 0.30f},  // Hay
    {0.74f, 0.52f, 0.38f},  // Clay
    {0.35f, 0.37f, 0.42f},  // Slate
    {1.00f, 0.80f, 0.45f},  // Lantern
};

constexpr int kChunk = 32;
constexpr glm::ivec3 kWorldSize{1024, 192, 1024};  // ponytail: dense grid (192 MiB); chunk streaming in M1
constexpr int kSeaLevel = 34;
static_assert(kChunk <= 32, "quad positions are 6-bit; see mesher.h");
static_assert(kWorldSize.x % kChunk == 0 && kWorldSize.y % kChunk == 0 && kWorldSize.z % kChunk == 0);

// Dense voxel volume, x fastest. Reads outside the volume return Air.
struct Grid {
    glm::ivec3 size{};
    std::vector<Block> cells;

    explicit Grid(glm::ivec3 s) : size(s), cells(size_t(s.x) * s.y * s.z, Air) {}
    bool inside(glm::ivec3 p) const {
        return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < size.x && p.y < size.y && p.z < size.z;
    }
    size_t index(glm::ivec3 p) const { return (size_t(p.y) * size.z + p.z) * size.x + p.x; }
    Block get(glm::ivec3 p) const { return inside(p) ? cells[index(p)] : Air; }
    void set(glm::ivec3 p, Block b) { if (inside(p)) cells[index(p)] = b; }
};

// Origin of chunk i of a grid of `size` (chunks numbered x fastest, then y, then z).
inline glm::ivec3 chunkOrigin(glm::ivec3 size, int i) {
    glm::ivec3 n = size / kChunk;
    return glm::ivec3{i % n.x, i / n.x % n.y, i / (n.x * n.y)} * kChunk;
}

// Runs fn(0..count-1) across all hardware threads.
void parallelFor(int count, const std::function<void(int)>& fn);

void generateTerrain(Grid& world, uint32_t seed);

// Natural shapes, shared by generateTerrain and the generated prefabs. Each grows on the ground
// voxel `at`; trunks grow down to the ground on slopes, canopies only fill air, rocks overwrite.
// Their ragged surfaces are seeded by `seed` at positions relative to `jitterOrigin` (terrain
// passes 0; a prefab instance passes `at`, so moving it keeps its shape).
void growTree(Grid& g, glm::ivec3 at, int trunk, float radius, uint32_t seed, glm::ivec3 jitterOrigin);
void growRock(Grid& g, glm::ivec3 at, bool spire, float radius, int height, uint32_t seed, glm::ivec3 jitterOrigin);
void growPine(Grid& g, glm::ivec3 at, int height, uint32_t seed, glm::ivec3 jitterOrigin);
void growBush(Grid& g, glm::ivec3 at, float radius, uint32_t seed, glm::ivec3 jitterOrigin);

// Feet position on the grass column nearest `near` (default: the world center).
glm::vec3 findSpawn(const Grid& world);
glm::vec3 findSpawn(const Grid& world, glm::ivec2 near);

// A placed prefab (see prefab.h). Its voxels are in the grid like any others; `cells` lists every
// voxel the placement changed, so removing it can put back what was there.
struct Instance {
    struct Cell {
        uint16_t x, y, z;
        Block before, after;
    };
    uint32_t id = 0;
    std::string prefab;
    glm::ivec3 pos{};  // the ground voxel under the footprint center; the bottom layer sits on it
    int rotation = 0;  // quarter turns counterclockwise seen from above (structures only)
    uint32_t seed = 0;
    std::vector<float> params;  // generated prefabs: one value per declared parameter
    std::vector<Cell> cells;
};

// Caps on instances, enforced when placing and loading, so memory and files stay bounded.
constexpr size_t kMaxInstances = 10000;
constexpr size_t kMaxInstanceCells = size_t(8) << 20;  // of all instances together (64 MB)
constexpr size_t kMaxInstanceParams = 8;
constexpr int kMaxInstanceExtent = 96;  // the cells of one instance span at most this per axis

// World files (.flyw): the whole grid, its seed and the prefab instances (sorted by id), plus the
// sections other modules own (story.h's markers), in one versioned file; see docs/ARCHITECTURE.md.
// Both return an empty string on success, else why it failed. Most code calls story.h's
// saveWorld/loadWorld instead. A rejected read leaves `g`, `instances` and `extra` unspecified.
struct Section {
    char tag[4];
    std::vector<uint8_t> bytes;
};
// The FNV-1a hash the file containers (.flyw, .flys) end with, of every byte before it.
uint32_t fnv1a(const uint8_t* p, size_t n);
// Writes `bytes` to PATH.tmp and renames it over PATH (creating the folder), so a failed write never
// destroys the last good file; empty on success, else why it failed. `durable` (player saves): the
// temp file is flushed to the disk (F_FULLFSYNC on macOS), the old PATH is kept as PATH.bak, and
// the folder is synced after the rename.
std::string replaceFile(const char* path, const std::vector<uint8_t>& bytes, bool durable = false);
// Writes `extra` after the voxels and instances, in order.
std::string writeWorldFile(const char* path, const Grid& g, uint32_t seed, const std::vector<Instance>& instances,
                           const std::vector<Section>& extra);
// `extra` gets every section other than the voxels and instances, in file order.
std::string readWorldFile(const char* path, Grid& g, uint32_t& seed, std::vector<Instance>& instances,
                          std::vector<Section>& extra);
