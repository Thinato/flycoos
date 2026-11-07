#pragma once
#include <cassert>
#include <bit>
#include <algorithm>
#include <array>
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
                       Brick, MossyCobble, DarkPlanks, Hay, Clay, Slate, Lantern,  // building
                       // Tree leaves: 7 kinds x 4 shades, dark to light (leafBlock). Oak's shade 2 is Leaves.
                       OakLeaves0, OakLeaves1, OakLeaves3, BirchLeaves0, BirchLeaves1, BirchLeaves2, BirchLeaves3,
                       PineNeedles0, PineNeedles1, PineNeedles2, PineNeedles3,
                       AraucariaLeaves0, AraucariaLeaves1, AraucariaLeaves2, AraucariaLeaves3,
                       MapleRed0, MapleRed1, MapleRed2, MapleRed3, MapleOrange0, MapleOrange1, MapleOrange2, MapleOrange3,
                       MapleGold0, MapleGold1, MapleGold2, MapleGold3,
                       BirchBark, PineBark, AraucariaBark,  // trunks (oak and maple use Wood)
                       // Plants (isPlant): stand in the cell above the ground, drawn as small models (plants.h)
                       ShortGrass, TallGrass, FlowerWhite, FlowerYellow, FlowerRed, FlowerViolet,
                       Blade, TallBlade, PetalWhite, PetalYellow, PetalRed, PetalViolet, Stem, FlowerHeart,  // plant models
                       BlockCount };  // the palette holds 128 (shaders/world.metal packs ids into 7 bits)

// Leaf kinds; a tree's crown steps through 4 shades of its kind, darkest at the bottom (docs/ART.md).
enum class Leaf : uint8_t { Oak, Birch, Pine, Araucaria, MapleRed, MapleOrange, MapleGold, Count };
constexpr int kLeafShades = 4;
constexpr Block leafBlock(Leaf kind, int shade) {
    int i = int(kind) * kLeafShades + shade;  // 0..27; oak shade 2 is the old Leaves block
    return i == 2 ? Leaves : Block(OakLeaves0 + i - (i > 2));
}
struct LeafOf { int8_t shade = -1; Leaf kind{}; };  // shade -1: not a leaf
inline constexpr auto kLeafOf = [] {
    std::array<LeafOf, BlockCount> t{};
    for (int k = 0; k < int(Leaf::Count); ++k)
        for (int s = 0; s < kLeafShades; ++s) t[leafBlock(Leaf(k), s)] = {int8_t(s), Leaf(k)};
    return t;
}();
constexpr bool isLeaf(Block b) { return kLeafOf[b].shade >= 0; }
constexpr bool isTreeBlock(Block b) { return b == Wood || b == BirchBark || b == PineBark || b == AraucariaBark || isLeaf(b); }

// Plants don't collide, don't stop light and aren't ground: to everything but drawing they are air.
constexpr bool isPlant(Block b) { return b >= ShortGrass && b <= FlowerViolet; }
inline bool isOpaque(Block b) { return b != Air && b != Water && !isPlant(b); }
constexpr bool isClear(Block b) { return b == Air || isPlant(b); }  // nothing in the way

inline constexpr const char* kBlockNames[BlockCount] = {
    "Air", "Grass", "Dirt", "Stone", "Sand", "Snow", "Wood", "Leaves", "Water", "Skin", "Tunic", "Belt", "Boots", "Hair",
    "Eye", "Rock", "Robe", "DarkHair", "Straw", "Burlap", "Steel", "Iron", "Hide", "Tusk", "Snout", "Hood", "Leather", "Plate",
    "Planks", "Cobble", "Thatch", "Ember", "Brick", "MossyCobble", "DarkPlanks", "Hay", "Clay", "Slate", "Lantern",
    "OakLeaves0", "OakLeaves1", "OakLeaves3", "BirchLeaves0", "BirchLeaves1", "BirchLeaves2", "BirchLeaves3",
    "PineNeedles0", "PineNeedles1", "PineNeedles2", "PineNeedles3",
    "AraucariaLeaves0", "AraucariaLeaves1", "AraucariaLeaves2", "AraucariaLeaves3",
    "MapleRed0", "MapleRed1", "MapleRed2", "MapleRed3", "MapleOrange0", "MapleOrange1", "MapleOrange2", "MapleOrange3",
    "MapleGold0", "MapleGold1", "MapleGold2", "MapleGold3", "BirchBark", "PineBark", "AraucariaBark",
    "ShortGrass", "TallGrass", "FlowerWhite", "FlowerYellow", "FlowerRed", "FlowerViolet",
    "Blade", "TallBlade", "PetalWhite", "PetalYellow", "PetalRed", "PetalViolet", "Stem", "FlowerHeart"};

// Leaf shades are the kind's colour (and patch tones) times kLeafRamp[shade]; shade 2 is the kind's
// own colour. Oak's is the old Leaves colour, so old worlds keep their look.
inline constexpr float kLeafRamp[kLeafShades] = {0.55f, 0.78f, 1.0f, 1.25f};
struct LeafColour { float rgb[3], a[3], b[3]; };  // sRGB; a, b the patch variants
inline constexpr LeafColour kLeafColours[int(Leaf::Count)] = {
    {{0.20f, 0.48f, 0.18f}, {0.38f, 0.60f, 0.20f}, {0.12f, 0.36f, 0.14f}},  // oak: warm green
    {{0.42f, 0.62f, 0.22f}, {0.56f, 0.70f, 0.26f}, {0.30f, 0.50f, 0.18f}},  // birch: light yellow-green
    {{0.10f, 0.32f, 0.24f}, {0.16f, 0.40f, 0.28f}, {0.07f, 0.24f, 0.20f}},  // pine: dark blue-green
    {{0.12f, 0.38f, 0.16f}, {0.20f, 0.46f, 0.18f}, {0.08f, 0.28f, 0.13f}},  // araucaria: deep green
    {{0.70f, 0.14f, 0.10f}, {0.80f, 0.26f, 0.12f}, {0.54f, 0.09f, 0.09f}},  // maple red
    {{0.86f, 0.42f, 0.10f}, {0.92f, 0.56f, 0.16f}, {0.74f, 0.30f, 0.08f}},  // maple orange
    {{0.88f, 0.68f, 0.16f}, {0.94f, 0.80f, 0.30f}, {0.76f, 0.54f, 0.12f}},  // maple gold
};

// Block colors in sRGB, indexed by Block. The renderer converts them to linear; the map command uses them as is.
inline constexpr auto kPalette = [] {
    std::array<std::array<float, 3>, BlockCount> p{{
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
    }};
    for (int k = 0; k < int(Leaf::Count); ++k)
        for (int s = 0; s < kLeafShades; ++s)
            for (int c = 0; c < 3; ++c)
                p[leafBlock(Leaf(k), s)][size_t(c)] = std::min(1.0f, kLeafColours[k].rgb[c] * kLeafRamp[s]);
    p[BirchBark] = {0.86f, 0.84f, 0.78f};
    p[PineBark] = {0.36f, 0.22f, 0.14f};
    p[AraucariaBark] = {0.40f, 0.33f, 0.28f};
    // A plant block's colour is its main one (it never draws as a cube); its models use the model blocks after it.
    p[ShortGrass] = p[Blade] = {0.42f, 0.68f, 0.26f};  // blades: a little lighter than the ground (docs/ART.md)
    p[TallGrass] = p[TallBlade] = {0.40f, 0.64f, 0.24f};
    p[FlowerWhite] = p[PetalWhite] = {0.95f, 0.94f, 0.88f};
    p[FlowerYellow] = p[PetalYellow] = {0.98f, 0.80f, 0.20f};
    p[FlowerRed] = p[PetalRed] = {0.84f, 0.16f, 0.12f};
    p[FlowerViolet] = p[PetalViolet] = {0.58f, 0.38f, 0.84f};
    p[Stem] = {0.28f, 0.52f, 0.18f};
    p[FlowerHeart] = {0.96f, 0.74f, 0.18f};
    return p;
}();

// Patch tones (docs/ART.md, ADR-028), indexed by Block: inside a block, soft patches about `size`
// voxels across fade its kPalette colour toward variant a and variant b (sRGB). `strength` 0..1:
// natural blocks clear, man-made faint, 0 plain (water, light sources, model-only blocks).
struct BlockTone { float a[3], b[3]; float size, strength; };
inline constexpr auto kTones = [] {
    std::array<BlockTone, BlockCount> t{{
    {},                                                     // Air
    {{0.60f, 0.68f, 0.26f}, {0.20f, 0.46f, 0.17f}, 24, 0.9f},  // Grass: dry yellow-green, lush dark green
    {{0.36f, 0.24f, 0.14f}, {0.56f, 0.42f, 0.27f}, 12, 0.7f},  // Dirt: damp, dry
    {{0.60f, 0.60f, 0.62f}, {0.41f, 0.41f, 0.44f}, 10, 1.0f},  // Stone (ground): soft lighter, darker grey
    {{0.88f, 0.82f, 0.60f}, {0.74f, 0.62f, 0.40f}, 20, 0.6f},  // Sand: pale, warm
    {{0.82f, 0.87f, 0.96f}, {1.00f, 1.00f, 1.00f}, 24, 0.6f},  // Snow: cool shadow, pure white
    {{0.30f, 0.19f, 0.10f}, {0.50f, 0.35f, 0.20f}, 5, 0.8f},   // Wood: dark spots, lighter brown
    {{0.38f, 0.60f, 0.20f}, {0.12f, 0.36f, 0.14f}, 12, 0.8f},  // Leaves: yellow-green, dark green
    {},                                                     // Water
    {}, {}, {}, {}, {}, {},                                 // Skin, Tunic, Belt, Boots, Hair, Eye
    {{0.66f, 0.64f, 0.60f}, {0.39f, 0.37f, 0.34f}, 5, 1.0f},   // Rock: lighter, darker grey
    {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},         // Robe .. Plate (models)
    {{0.56f, 0.40f, 0.24f}, {0.70f, 0.56f, 0.38f}, 6, 0.3f},   // Planks: darker, faded
    {{0.36f, 0.36f, 0.36f}, {0.54f, 0.53f, 0.51f}, 5, 0.3f},   // Cobble
    {{0.46f, 0.36f, 0.20f}, {0.64f, 0.54f, 0.34f}, 8, 0.3f},   // Thatch
    {},                                                     // Ember
    {{0.50f, 0.25f, 0.19f}, {0.66f, 0.42f, 0.34f}, 5, 0.3f},   // Brick
    {{0.30f, 0.40f, 0.23f}, {0.46f, 0.48f, 0.42f}, 6, 0.35f},  // MossyCobble: mossier, bare stone
    {{0.40f, 0.26f, 0.16f}, {0.55f, 0.40f, 0.28f}, 6, 0.3f},   // DarkPlanks
    {{0.80f, 0.62f, 0.24f}, {0.94f, 0.80f, 0.44f}, 6, 0.25f},  // Hay
    {{0.64f, 0.44f, 0.32f}, {0.78f, 0.60f, 0.47f}, 8, 0.25f},  // Clay
    {{0.28f, 0.30f, 0.35f}, {0.42f, 0.44f, 0.48f}, 8, 0.25f},  // Slate
    {},                                                     // Lantern
    }};
    for (int k = 0; k < int(Leaf::Count); ++k)
        for (int s = 0; s < kLeafShades; ++s) {
            BlockTone& o = t[leafBlock(Leaf(k), s)];
            for (int c = 0; c < 3; ++c) {
                o.a[c] = std::min(1.0f, kLeafColours[k].a[c] * kLeafRamp[s]);
                o.b[c] = std::min(1.0f, kLeafColours[k].b[c] * kLeafRamp[s]);
            }
            o.size = 12, o.strength = 0.8f;
        }
    t[BirchBark] = {{0.24f, 0.23f, 0.22f}, {0.95f, 0.94f, 0.90f}, 4, 0.9f};  // dark knots, bright white
    t[PineBark] = {{0.26f, 0.15f, 0.09f}, {0.48f, 0.32f, 0.20f}, 5, 0.8f};
    t[AraucariaBark] = {{0.30f, 0.24f, 0.20f}, {0.52f, 0.44f, 0.38f}, 4, 0.8f};  // scaly plates
    // Blades follow the Grass patches under them (same size; plant_fs samples Grass's noise at the root).
    t[Blade] = {{0.66f, 0.72f, 0.30f}, {0.24f, 0.52f, 0.19f}, 24, 0.9f};
    t[TallBlade] = {{0.64f, 0.70f, 0.28f}, {0.22f, 0.48f, 0.18f}, 24, 0.9f};
    return t;
}();

constexpr int kChunk = 32;
constexpr glm::ivec3 kWorldSize{1024, 192, 1024};  // ponytail: dense grid (192 MiB); chunk streaming in M1
constexpr int kSeaLevel = 34;
static_assert(kChunk <= 32, "quad positions are 6-bit; see mesher.h");
static_assert(kWorldSize.x % kChunk == 0 && kWorldSize.y % kChunk == 0 && kWorldSize.z % kChunk == 0);

// Dense voxel volume, x fastest, in world coordinates. Reads outside the volume return Air.
// A streamed grid (streamed()) is a window onto the endless world: `lo` is its low corner and its
// storage wraps around in x and z, so moving the window only rewrites the chunks that left it (a
// voxel lives at index(p) whatever `lo` is). Any other grid is fixed at `lo` (usually 0) and doesn't wrap.
struct Grid {
    glm::ivec3 size{};
    glm::ivec3 lo{};             // world position of the volume's low corner; y is always 0
    glm::ivec2 wrap{-1, -1};     // x, z storage masks (size - 1) of a streamed grid; -1: no wrap
    glm::ivec3 org{};            // subtracted before indexing: lo for a fixed grid, 0 for a streamed one
    std::vector<Block> cells;

    explicit Grid(glm::ivec3 s, glm::ivec3 at = {}) : size(s), lo(at), org(at), cells(size_t(s.x) * s.y * s.z, Air) {}
    // A window of `s` (x and z powers of two) for streaming, its low corner at `origin`.
    static Grid streamed(glm::ivec3 s, glm::ivec3 origin) {
        assert(std::has_single_bit(unsigned(s.x)) && std::has_single_bit(unsigned(s.z)) && origin % kChunk == glm::ivec3(0));
        Grid g(s);
        g.lo = origin;
        g.wrap = {s.x - 1, s.z - 1};
        g.org = {};
        return g;
    }
    bool streaming() const { return wrap.x >= 0; }
    glm::ivec3 hi() const { return lo + size - 1; }  // the highest voxel inside
    bool inside(glm::ivec3 p) const {
        glm::ivec3 q = p - lo;
        return q.x >= 0 && q.y >= 0 && q.z >= 0 && q.x < size.x && q.y < size.y && q.z < size.z;
    }
    // Of a voxel inside. Along x, storage is contiguous up to the wrap seam (rowRun).
    size_t index(glm::ivec3 p) const {
        return (size_t(p.y) * size.z + size_t((p.z - org.z) & wrap.y)) * size.x + size_t((p.x - org.x) & wrap.x);
    }
    // How many voxels from p along +x (at most n) are contiguous in storage from index(p).
    int rowRun(glm::ivec3 p, int n) const { return std::min(n, size.x - ((p.x - org.x) & wrap.x)); }
    Block get(glm::ivec3 p) const { return inside(p) ? cells[index(p)] : Air; }
    void set(glm::ivec3 p, Block b) { if (inside(p)) cells[index(p)] = b; }

    // Chunk slots: kChunk^3 blocks of storage numbered x fastest, then y, then z (a streamed grid's
    // `lo` is chunk-aligned, so each slot holds one whole chunk of the world).
    size_t chunkSlot(glm::ivec3 p) const {  // the slot of voxel p (inside)
        glm::ivec3 n = size / kChunk, s = ((p - org) >> 5) & glm::ivec3(wrap.x >> 5, -1, wrap.y >> 5);  // >> 5: floor / kChunk
        return size_t(s.x + n.x * (s.y + n.y * s.z));
    }
    glm::ivec3 chunkOrigin(size_t slot) const {  // world position of the chunk in a slot
        glm::ivec3 n = size / kChunk, s(int(slot) % n.x, int(slot) / n.x % n.y, int(slot) / (n.x * n.y));
        return lo + ((s * kChunk + org - lo) & glm::ivec3(wrap.x, -1, wrap.y));
    }
};
static_assert(kChunk == 32, "Grid::chunkSlot divides by shifting");

// Origin of chunk i of a grid of `size` (chunks numbered x fastest, then y, then z).
inline glm::ivec3 chunkOrigin(glm::ivec3 size, int i) {
    glm::ivec3 n = size / kChunk;
    return glm::ivec3{i % n.x, i / n.x % n.y, i / (n.x * n.y)} * kChunk;
}

// Runs fn(0..count-1) across all hardware threads.
void parallelFor(int count, const std::function<void(int)>& fn);

// The world of `seed`, chunk column by chunk column (in parallel) over every chunk g holds.
void generateTerrain(Grid& world, uint32_t seed);
// One chunk column (chunk coordinates x, z: kChunk-wide) of the world of `seed`, into g where g
// holds it. A pure function of (chunk, seed): any order, any grid, the same voxels.
void generateChunkColumn(Grid& g, glm::ivec2 chunk, uint32_t seed);

// Rocks, shared by generateTerrain and the generated prefabs (trees: trees.h). Each grows on the
// ground voxel `at` and overwrites. The ragged surface is seeded by `seed` at positions relative to
// `jitterOrigin` (terrain passes 0; a prefab instance passes `at`, so moving it keeps its shape).
void growRock(Grid& g, glm::ivec3 at, bool spire, float radius, int height, uint32_t seed, glm::ivec3 jitterOrigin);

// Feet position on the grass column nearest `near` (default: the world center).
glm::vec3 findSpawn(const Grid& world);
glm::vec3 findSpawn(const Grid& world, glm::ivec2 near);

// A placed prefab (see prefab.h). Its voxels are in the grid like any others; `cells` lists every
// voxel the placement changed, so removing it can put back what was there.
// Whether `p` is under water: in a Water voxel, and below the surface the shader draws 0.12 under
// the top of a Water voxel with no water above it (shaders/world.metal).
inline bool underWater(const Grid& g, glm::vec3 p) {
    glm::ivec3 c(glm::floor(p));
    return g.get(c) == Water && (g.get(c + glm::ivec3(0, 1, 0)) == Water || p.y - float(c.y) < 0.88f);
}

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
