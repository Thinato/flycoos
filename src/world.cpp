#include "world.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <tuple>

#include <fcntl.h>
#include <unistd.h>

#include <FastNoiseLite.h>

#include "plants.h"
#include "trees.h"

void parallelFor(int count, const std::function<void(int)>& fn) {
    if (count <= 1) {  // not worth a thread
        if (count == 1) fn(0);
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::min(unsigned(count), std::max(1u, std::thread::hardware_concurrency())); ++t)
        pool.emplace_back([&] {
            for (int i; (i = next++) < count;) fn(i);
        });
    for (std::thread& t : pool) t.join();  // std::jthread needs -fexperimental-library on AppleClang 17
}

static uint32_t hash(int x, int z, uint32_t seed) {
    uint32_t h = seed ^ uint32_t(x) * 0x27d4eb2du ^ uint32_t(z) * 0x165667b1u;
    h ^= h >> 15; h *= 0x85ebca6bu;
    h ^= h >> 13; h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

// Per-voxel jitter in [-0.45, 0.45] to roughen rock surfaces (rocks only: trees use smooth noise, docs/ART.md).
static float jitter(glm::ivec3 p, uint32_t seed) {
    return float(hash(p.x, p.z, seed ^ uint32_t(p.y) * 0x9e3779b9u) & 255) / 255.0f * 0.9f - 0.45f;
}

static int topY(const Grid& g, int x, int z) {
    for (int y = g.size.y - 1; y >= 0; --y)
        if (!isClear(g.get({x, y, z}))) return y;
    return -1;
}

static FastNoiseLite makeNoise(uint32_t seed, float frequency, int octaves) {
    FastNoiseLite n{int(seed)};
    n.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    n.SetFrequency(frequency);
    if (octaves > 1) {
        n.SetFractalType(FastNoiseLite::FractalType_FBm);
        n.SetFractalOctaves(octaves);
    }
    return n;
}

// A tapering spire, or a half-buried boulder (`height` unused).
void growRock(Grid& g, glm::ivec3 at, bool spire, float radius, int height, uint32_t seed, glm::ivec3 jitterOrigin) {
    int x = at.x, y = at.y, z = at.z;
    if (!spire) height = int(radius * 1.5f);
    int r = int(std::ceil(radius)) + 1;
    for (int dy = -3; dy <= height; ++dy)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                glm::ivec3 p{x + dx, y + dy, z + dz};
                float t = std::max(0.0f, float(dy)) / float(height);
                float d = spire ? std::sqrt(float(dx * dx + dz * dz)) / (radius * (1 - 0.65f * std::pow(t, 1.3f)))
                                : glm::length(glm::vec3(dx, (dy - radius * 0.3f) / 0.75f, dz)) / radius;
                if (d + jitter(p - jitterOrigin, seed) * 0.25f <= 1) g.set(p, Rock);
            }
}

// Region strengths (0..1) at a column: x = forest, y = rocky land. Neither = open plains.
// Rocky land pushes forest out so the two stay mostly separate instead of cluttering together.
// The noises are low-frequency (features ~250-500 blocks) so each kind of land is vast.
struct Regions {
    FastNoiseLite warp, forest, rock;
    explicit Regions(uint32_t seed)
        : warp(makeNoise(seed + 10u, 0.003f, 1)), forest(makeNoise(seed + 1u, 0.002f, 2)), rock(makeNoise(seed + 2u, 0.002f, 2)) {
        warp.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
        warp.SetDomainWarpAmp(100);
    }
    glm::vec2 operator()(int x, int z) const {
        float wx = float(x), wz = float(z);
        warp.DomainWarp(wx, wz);  // organic region borders
        float rocky = glm::smoothstep(0.20f, 0.40f, rock.GetNoise(wx, wz));
        return {glm::smoothstep(0.10f, 0.30f, forest.GetNoise(wx, wz)) * (1 - 0.85f * rocky), rocky};
    }
};

// The bare terrain of a world seed, column by column: a pure function of (x, z), so any part of
// the endless world comes out the same whenever and in whatever order it's generated.
struct Terrain {
    Regions regions;
    FastNoiseLite hills, mountains, lakes, patches;
    int height;  // the world's
    Terrain(uint32_t seed, int worldHeight)
        : regions(seed), hills(makeNoise(seed, 0.006f, 4)), mountains(makeNoise(seed + 3u, 0.0015f, 3)),
          lakes(makeNoise(seed + 4u, 0.004f, 2)), patches(makeNoise(seed + 5u, 0.02f, 1)), height(worldHeight) {}
    struct Column { int h; Block top, sub; };
    Column operator()(int x, int z) const {
        float fx = float(x), fz = float(z);
        glm::vec2 region = regions(x, z);
        float hillHeight = 8 + 10 * region.x + 50 * region.y;  // plains gentle, rocky land tall
        float hill = hills.GetNoise(fx, fz) * 0.5f + 0.5f;
        float mountain = std::max(0.0f, mountains.GetNoise(fx, fz) * 0.5f + 0.5f - 0.6f) / 0.4f;
        float lake = std::max(0.0f, -lakes.GetNoise(fx, fz) - 0.5f) * 50;
        int h = int(kSeaLevel + 5 + hill * hill * hillHeight + mountain * mountain * 80 - lake);
        h = std::clamp(h, 2, std::max(2, height - 30));  // headroom for tall trees and spires
        bool bareRock = region.y > 0.5f && patches.GetNoise(fx, fz) > 0.3f;
        Block top = h <= kSeaLevel + 1 ? Sand : h > 130 ? Snow : h > 110 || bareRock ? Stone : Grass;
        return {h, top, top == Sand ? Sand : top == Grass ? Dirt : Stone};
    }
    // The column's highest voxel that isn't clear (lake water counts) and its block.
    std::pair<int, Block> ground(int x, int z) const {
        Column c = (*this)(x, z);
        return c.h < kSeaLevel ? std::pair(kSeaLevel, Water) : std::pair(c.h, c.top);
    }
    void fill(Grid& g, int x, int z) const {
        Column c = (*this)(x, z);
        for (int y = 0; y <= c.h; ++y) g.set({x, y, z}, y == c.h ? c.top : y > c.h - 4 ? c.sub : Stone);
        for (int y = c.h + 1; y <= kSeaLevel; ++y) g.set({x, y, z}, Water);
    }
};

static int floorDiv(int a, int b) { return a / b - (a % b < 0); }

// A tree for the land it stands on (ADR-029): Araucaria groves on high flat ground, conifers on
// high or rocky ground up to the snow line, otherwise the lowland mix of oak, maple and birch.
// A quarter are young (55-85% of full grown). Only on grass, under the world's top.
static std::optional<TreeSite> treeSite(const GroundAt& ground, int worldHeight, int x, int z, uint32_t rnd, glm::vec2 region) {
    auto [y, block] = ground(x, z);
    if (y < 0 || block != Grass) return std::nullopt;
    int flat = 0;  // the ground's largest step to points 4 out
    for (glm::ivec2 d : {glm::ivec2(4, 0), glm::ivec2(-4, 0), glm::ivec2(0, 4), glm::ivec2(0, -4)})
        flat = std::max(flat, std::abs(ground(x + d.x, z + d.y).first - y));
    float pick = float(rnd >> 20 & 1023) / 1024.0f;
    bool plateau = y >= 48 && flat <= 2 && region.y < 0.5f;
    Species s = plateau && pick < 0.5f                      ? Species::Araucaria
              : y > 70 || region.y > 0.3f                   ? Species::Conifer
              : region.x < 0.05f                            ? Species::Oak  // a lone plains tree
              : pick < 0.45f                                ? Species::Oak
              : pick < 0.70f                                ? Species::Maple
                                                            : Species::Birch;
    float grown = (rnd >> 12 & 3) == 0 ? 0.55f + 0.3f * float(rnd >> 14 & 63) / 63.0f : 1.0f;
    int lo = kFullGrown[int(s)][0], hi = kFullGrown[int(s)][1];
    TreeSite site{{x, y, z}, s, std::max(6, int(float(lo + int(rnd >> 4 & 255) % (hi - lo + 1)) * grown)), int(rnd >> 9 & 255) % 3, rnd};
    Box box = shapeBox(planTree(s, site.height, 0, site.colour, rnd), site.at);
    if (box.lo.y < 0 || box.hi.y >= worldHeight) return std::nullopt;
    return site;
}

GroundAt groundOf(const Grid& g, int margin) {
    return [&g, margin](int x, int z) {
        if (x < g.lo.x + margin || z < g.lo.z + margin || x > g.hi().x - margin || z > g.hi().z - margin) return std::pair(-1, Air);
        int y = topY(g, x, z);
        return std::pair(y, g.get({x, y, z}));
    };
}

void plantTrees(uint32_t seed, int worldHeight, glm::ivec2 lo, glm::ivec2 hi, const GroundAt& ground,
                const std::function<void(const TreeSite&)>& plant) {
    // One candidate per 13x13 cell (crowns are ~24 across, so neighbours touch); dense in forests, a
    // rare lone tree elsewhere. Seeded per cell. A candidate yields to any neighbouring cell's
    // candidate closer than kSpacing with a higher roll, so which trees grow depends only on the
    // cells around them, never on what was planted first.
    constexpr int kCell = 13;
    constexpr float kSpacing = 10;
    Regions regions(seed);
    auto candidate = [&](int cx, int cz) -> std::optional<TreeSite> {
        uint32_t rnd = hash(cx, cz, seed);
        int x = cx * kCell + 2 + int(rnd % 9), z = cz * kCell + 2 + int((rnd >> 8) % 9);
        float chance = float((rnd >> 16) & 0xFFFF) / 65536.0f;
        glm::vec2 region = regions(x, z);
        if (chance >= 0.85f * region.x + 0.006f) return std::nullopt;
        return treeSite(ground, worldHeight, x, z, hash(cx, cz, seed ^ 0x2c1b3c6du), region);
    };
    glm::ivec2 c0(floorDiv(lo.x, kCell), floorDiv(lo.y, kCell)), c1(floorDiv(hi.x, kCell), floorDiv(hi.y, kCell));
    // Candidates of the cells and a ring around, row by row.
    int w = c1.x - c0.x + 3;
    std::vector<std::optional<TreeSite>> sites(size_t(w) * size_t(c1.y - c0.y + 3));
    for (int cz = c0.y - 1; cz <= c1.y + 1; ++cz)
        for (int cx = c0.x - 1; cx <= c1.x + 1; ++cx) sites[size_t(cz - c0.y + 1) * size_t(w) + size_t(cx - c0.x + 1)] = candidate(cx, cz);
    auto at = [&](int cx, int cz) -> const std::optional<TreeSite>& { return sites[size_t(cz - c0.y + 1) * size_t(w) + size_t(cx - c0.x + 1)]; };
    auto beats = [](const TreeSite& a, const TreeSite& b) {  // a's roll over b's (ties: position)
        return std::tie(a.seed, a.at.x, a.at.z) > std::tie(b.seed, b.at.x, b.at.z);
    };
    for (int cz = c0.y; cz <= c1.y; ++cz)
        for (int cx = c0.x; cx <= c1.x; ++cx) {
            const std::optional<TreeSite>& site = at(cx, cz);
            if (!site || site->at.x < lo.x || site->at.z < lo.y || site->at.x > hi.x || site->at.z > hi.y) continue;
            bool yields = false;
            for (int dz = -1; dz <= 1 && !yields; ++dz)
                for (int dx = -1; dx <= 1 && !yields; ++dx) {
                    const std::optional<TreeSite>& n = at(cx + dx, cz + dz);
                    yields = (dx || dz) && n && beats(*n, *site) &&
                             glm::distance(glm::vec2(n->at.x, n->at.z), glm::vec2(site->at.x, site->at.z)) < kSpacing;
                }
            if (!yields) plant(*site);
        }
}

// Rock spire (10-23 tall, tapering) or a half-buried boulder, standing on ground voxel y; none in water.
static void placeRock(Grid& g, int x, int y, int z, uint32_t rnd, bool spire, uint32_t seed) {
    float radius = spire ? 2.5f + float((rnd >> 8) % 3) * 0.75f : 2.0f + float((rnd >> 8) % 3);
    int height = spire ? 10 + int((rnd >> 12) % 14) : int(radius * 1.5f);
    if (y < kSeaLevel || y + height + 1 >= g.size.y) return;
    growRock(g, {x, y, z}, spire, radius, height, seed, glm::ivec3(0));
}

void generateChunkColumn(Grid& g, glm::ivec2 chunk, uint32_t seed) {
    // Built in a padded scratch grid: the trees and rocks that stand within kMargin of the chunk
    // (more than any reaches) grow there whole, then only the chunk's columns are copied, so a tree
    // across a chunk border comes out the same from either side.
    constexpr int kMargin = 24;
    const glm::ivec3 o(chunk.x * kChunk, 0, chunk.y * kChunk);
    Grid pad({kChunk + 2 * kMargin, g.size.y, kChunk + 2 * kMargin}, o - glm::ivec3(kMargin, 0, kMargin));
    Terrain terrain(seed, g.size.y);
    for (int z = pad.lo.z; z <= pad.hi().z; ++z)
        for (int x = pad.lo.x; x <= pad.hi().x; ++x) terrain.fill(pad, x, z);

    glm::ivec2 lo(pad.lo.x, pad.lo.z), hi(pad.hi().x, pad.hi().z);
    GroundAt ground = [&](int x, int z) { return terrain.ground(x, z); };
    plantTrees(seed, g.size.y, lo, hi, ground, [&](const TreeSite& t) {
        TreeShape shape = planTree(t.species, t.height, 0, t.colour, t.seed);
        Box box = shapeBox(shape, t.at);  // only the ones reaching the chunk (one more: stray leaves look at neighbours)
        if (box.hi.x + 1 >= o.x && box.lo.x - 1 < o.x + kChunk && box.hi.z + 1 >= o.z && box.lo.z - 1 < o.z + kChunk)
            growShape(pad, t.at, shape, t.seed);
    });
    // Rocks: one candidate per 14x14 cell; spires and boulders on rocky land, a rare boulder elsewhere.
    for (int cz = floorDiv(lo.y, 14); cz <= floorDiv(hi.y, 14); ++cz)
        for (int cx = floorDiv(lo.x, 14); cx <= floorDiv(hi.x, 14); ++cx) {
            uint32_t rnd = hash(cx, cz, seed ^ 0x5bd1e995u);
            int x = cx * 14 + 2 + int(rnd % 10), z = cz * 14 + 2 + int((rnd >> 8) % 10);
            float chance = float((rnd >> 16) & 0xFFFF) / 65536.0f;
            float rocky = terrain.regions(x, z).y;
            if (chance < 0.4f * rocky + 0.006f) placeRock(pad, x, terrain(x, z).h, z, rnd, rocky > 0.5f && (rnd >> 4) % 4 != 0, seed);
        }
    // Grass and flowers last, on the open grass the trees and rocks left.
    for (int z = o.z; z < o.z + kChunk; ++z)
        for (int x = o.x; x < o.x + kChunk; ++x) plantColumn(pad, x, z, ShortGrass, 1);

    for (int y = 0; y < g.size.y; ++y)
        for (int z = o.z; z < o.z + kChunk; ++z)
            for (int x = o.x; x < o.x + kChunk; ++x)
                if (g.inside({x, y, z})) g.cells[g.index({x, y, z})] = pad.cells[pad.index({x, y, z})];
}

void generateTerrain(Grid& g, uint32_t seed) {
    glm::ivec2 c0 = glm::ivec2(g.lo.x, g.lo.z) >> 5, c1 = glm::ivec2(g.hi().x, g.hi().z) >> 5;  // chunks (floor / kChunk)
    int w = c1.x - c0.x + 1;
    parallelFor(w * (c1.y - c0.y + 1), [&](int i) { generateChunkColumn(g, c0 + glm::ivec2(i % w, i / w), seed); });
}

glm::vec3 findSpawn(const Grid& g) { return findSpawn(g, {g.lo.x + g.size.x / 2, g.lo.z + g.size.z / 2}); }

glm::vec3 findSpawn(const Grid& g, glm::ivec2 near) {
    int cx = near.x, cz = near.y;
    // The endless world: within what main waits for around the spawn (5 chunk columns), the rest may not be in yet.
    for (int r = 0; r < (g.streaming() ? 5 * kChunk : std::max(g.size.x, g.size.z) / 2); ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;  // ring only
                int x = cx + dx, z = cz + dz, y = topY(g, x, z);
                if (y >= 0 && g.get({x, y, z}) == Grass) return {x + 0.5f, y + 1.0f, z + 0.5f};
            }
    return {cx + 0.5f, float(g.size.y), cz + 0.5f};
}

// World file layout (little-endian):
//   "FLYW" | u32 version | i32 size x, y, z | u32 seed
//   sections, each: char tag[4] | u64 bytes | payload. Unknown tags are returned to the caller
//   (story.cpp skips any it doesn't know), which leaves room for later sections without a
//   version bump.
//   u32 FNV-1a of every byte before it: any corruption is rejected instead of loading a wrong world.
// "VOXL" section: per 32^3 chunk (in chunkOrigin order): u32 bytes, then runs of
//   (u8 block, LEB128 count - 1) over the chunk's voxels, x fastest, then z, then y. Terrain is
//   mostly horizontal layers, so whole layers of stone or air collapse into single runs.
// "INST" section (only when there are instances): u32 count, then per instance: u32 id | u8 name
//   length | name | i32 x, y, z | u8 rotation | u32 seed | u8 param count | f32 params |
//   u32 cell count | cells (u16 x, y, z | u8 before | u8 after). Read only; files now get INS2.
// "INS2" section: as INST, but after the params a u8 form: 0 = u32 cell count | cells as in INST;
//   1 = the cells run-length coded over their box: u16 lo x, y, z | u8 extent - 1 x, y, z | runs of
//   (u8 before, u8 after, LEB128 count - 1), x fastest, then z, then y; before == after is a voxel
//   that isn't a cell. Form 1 is for cells in that order, each a change; a big tree's thousands of
//   cells take a few kB instead of 8 bytes each.
// Every other section goes to and comes from the caller as `extra` (story.cpp: MARK, LINE).
constexpr uint32_t kWorldFileVersion = 1;
constexpr int kChunkVoxels = kChunk * kChunk * kChunk;

uint32_t fnv1a(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void put(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

static uint64_t get(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

static std::vector<uint8_t> encodeChunk(const Grid& g, glm::ivec3 o) {
    std::vector<uint8_t> out;
    Block run = Air;
    int count = 0;
    auto flush = [&] {
        out.push_back(run);
        uint32_t n = uint32_t(count - 1);
        for (; n >= 128; n >>= 7) out.push_back(uint8_t(n | 128));
        out.push_back(uint8_t(n));
    };
    for (int y = 0; y < kChunk; ++y)
        for (int z = 0; z < kChunk; ++z) {
            const Block* row = &g.cells[g.index(o + glm::ivec3(0, y, z))];
            for (int x = 0; x < kChunk; ++x) {
                if (count && row[x] == run) { ++count; continue; }
                if (count) flush();
                run = row[x];
                count = 1;
            }
        }
    flush();
    return out;
}

static bool decodeChunk(Grid& g, glm::ivec3 o, const uint8_t* p, const uint8_t* end) {
    int i = 0;  // voxel index within the chunk
    while (p < end) {
        Block b = Block(*p++);
        int count = 0;
        for (int shift = 0;; shift += 7) {
            if (p == end || shift > 14) return false;  // 15 bits cover 32^3 voxels
            count |= (*p & 127) << shift;
            if (!(*p++ & 128)) break;
        }
        ++count;
        if (b >= BlockCount || count > kChunkVoxels - i) return false;
        for (int stop = i + count; i < stop;) {  // fill row by row
            int x = i % kChunk, n = std::min(stop - i, kChunk - x);
            std::fill_n(&g.cells[g.index(o + glm::ivec3(x, i / (kChunk * kChunk), i / kChunk % kChunk))], n, b);
            i += n;
        }
    }
    return i == kChunkVoxels;
}

static void putCount(std::vector<uint8_t>& out, uint32_t n) {  // LEB128 of n - 1
    for (--n; n >= 128; n >>= 7) out.push_back(uint8_t(n | 128));
    out.push_back(uint8_t(n));
}

// Appends INS2 form 1 for `cells`, or returns false (nothing appended) if they aren't changes in box order.
static bool encodeCells(const std::vector<Instance::Cell>& cells, std::vector<uint8_t>& out) {
    if (cells.empty()) return false;
    glm::ivec3 lo(INT_MAX), hi(INT_MIN);
    for (const Instance::Cell& c : cells) lo = glm::min(lo, glm::ivec3(c.x, c.y, c.z)), hi = glm::max(hi, glm::ivec3(c.x, c.y, c.z));
    glm::ivec3 e = hi - lo + 1;
    if (glm::any(glm::greaterThan(e, glm::ivec3(256)))) return false;
    auto at = [&](const Instance::Cell& c) { return int64_t((c.y - lo.y) * e.z + (c.z - lo.z)) * e.x + (c.x - lo.x); };
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i].before == cells[i].after || (i && at(cells[i]) <= at(cells[i - 1]))) return false;
    out.push_back(1);
    for (int a = 0; a < 3; ++a) put(out, uint32_t(lo[a]), 2);
    for (int a = 0; a < 3; ++a) out.push_back(uint8_t(e[a] - 1));
    Block before = Air, after = Air;  // the current run; before == after: not a cell
    uint32_t count = 0;
    auto run = [&](Block b, Block a, uint32_t n) {
        if (count && b == before && a == after) return void(count += n);
        if (count) out.push_back(before), out.push_back(after), putCount(out, count);
        before = b, after = a, count = n;
    };
    int64_t next = 0;  // box index after the last cell
    for (const Instance::Cell& c : cells) {
        if (at(c) > next) run(Air, Air, uint32_t(at(c) - next));
        run(c.before, c.after, 1);
        next = at(c) + 1;
    }
    if (int64_t total = int64_t(e.x) * e.y * e.z; total > next) run(Air, Air, uint32_t(total - next));
    out.push_back(before), out.push_back(after), putCount(out, count);
    return true;
}

std::string writeWorldFile(const char* path, const Grid& g, uint32_t seed, const std::vector<Instance>& instances,
                           const std::vector<Section>& extra) {
    if (g.streaming()) return "the endless world isn't saved (yet)";  // its storage is a wrapped window, not a world
    if (g.size % kChunk != glm::ivec3(0)) return "world size is not a multiple of the chunk size";
    glm::ivec3 counts = g.size / kChunk;
    int n = counts.x * counts.y * counts.z;
    std::vector<std::vector<uint8_t>> chunks(static_cast<size_t>(n));
    parallelFor(n, [&](int i) { chunks[size_t(i)] = encodeChunk(g, chunkOrigin(g.size, i)); });

    std::vector<uint8_t> out{'F', 'L', 'Y', 'W'};
    put(out, kWorldFileVersion, 4);
    for (int a = 0; a < 3; ++a) put(out, uint32_t(g.size[a]), 4);
    put(out, seed, 4);
    out.insert(out.end(), {'V', 'O', 'X', 'L'});
    size_t sizeAt = out.size();
    put(out, 0, 8);
    for (const std::vector<uint8_t>& c : chunks) {
        put(out, c.size(), 4);
        out.insert(out.end(), c.begin(), c.end());
    }
    auto endSection = [&] {
        uint64_t sectionBytes = out.size() - sizeAt - 8;
        for (int i = 0; i < 8; ++i) out[sizeAt + size_t(i)] = uint8_t(sectionBytes >> (8 * i));
    };
    endSection();
    if (!instances.empty()) {
        out.insert(out.end(), {'I', 'N', 'S', '2'});
        sizeAt = out.size();
        put(out, 0, 8);
        put(out, instances.size(), 4);
        for (const Instance& in : instances) {
            put(out, in.id, 4);
            put(out, in.prefab.size(), 1);
            out.insert(out.end(), in.prefab.begin(), in.prefab.end());
            for (int a = 0; a < 3; ++a) put(out, uint32_t(in.pos[a]), 4);
            put(out, uint32_t(in.rotation), 1);
            put(out, in.seed, 4);
            put(out, in.params.size(), 1);
            for (float f : in.params) put(out, std::bit_cast<uint32_t>(f), 4);
            if (!encodeCells(in.cells, out)) {
                out.push_back(0);
                put(out, in.cells.size(), 4);
                for (const Instance::Cell& c : in.cells) {
                    put(out, c.x, 2), put(out, c.y, 2), put(out, c.z, 2);
                    out.push_back(c.before), out.push_back(c.after);
                }
            }
        }
        endSection();
    }
    for (const Section& sec : extra) {
        out.insert(out.end(), sec.tag, sec.tag + 4);
        put(out, sec.bytes.size(), 8);
        out.insert(out.end(), sec.bytes.begin(), sec.bytes.end());
    }
    put(out, fnv1a(out.data(), out.size()), 4);

    return replaceFile(path, out);
}

std::string replaceFile(const char* path, const std::vector<uint8_t>& bytes, bool durable) {
    std::filesystem::path target(path), tmp = target, bak = target;
    tmp += ".tmp";
    bak += ".bak";
    std::error_code ec;
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool ok = fd >= 0;
    for (size_t done = 0; ok && done < bytes.size();) {
        ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        ok = n > 0;
        done += ok ? size_t(n) : 0;
    }
#ifdef F_FULLFSYNC
    if (ok && durable && ::fcntl(fd, F_FULLFSYNC) != 0) ok = ::fsync(fd) == 0;  // fsync alone may stay in the drive's cache
#else
    if (ok && durable) ok = ::fsync(fd) == 0;
#endif
    if (fd >= 0 && ::close(fd) != 0) ok = false;
    if (!ok) {
        std::filesystem::remove(tmp, ec);
        return std::format("cannot write {}", tmp.string());
    }
    // ponytail: two renames leave a moment with only PATH.bak; the save loader falls back to it.
    if (durable && std::filesystem::exists(target, ec)) std::filesystem::rename(target, bak, ec);
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::string error = std::format("cannot replace {}: {}", target.string(), ec.message());
        std::filesystem::remove(tmp, ec);
        return error;
    }
    if (durable) {  // the renames themselves reach the disk with the folder's entry
        std::filesystem::path dir = target.has_parent_path() ? target.parent_path() : ".";
        if (int d = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC); d >= 0) {
#ifdef F_FULLFSYNC
            if (::fcntl(d, F_FULLFSYNC) != 0) ::fsync(d);  // best effort: not every file system syncs a folder
#else
            ::fsync(d);
#endif
            ::close(d);
        }
    }
    return {};
}

// Reads the INST or (`v2`) INS2 payload [p, end) into `out`; returns why it's malformed, or "".
static std::string readInstances(const uint8_t* p, const uint8_t* end, glm::ivec3 size, bool v2, std::vector<Instance>& out) {
    bool truncated = false;
    auto take = [&](int64_t bytes) -> uint64_t {  // a little-endian number, or 0 past the end
        if (end - p < bytes) return truncated = true, p = end, 0;
        p += bytes;
        return get(p - bytes, int(bytes));
    };
    uint64_t count = take(4), total = 0;
    if (count > kMaxInstances) return std::format("more than {} instances", kMaxInstances);
    for (uint64_t i = 0; i < count && !truncated; ++i) {
        Instance in;
        in.id = uint32_t(take(4));
        uint64_t nameBytes = take(1);
        if (end - p < int64_t(nameBytes)) return "truncated instances";
        in.prefab.assign(p, p + nameBytes);
        p += nameBytes;
        for (int a = 0; a < 3; ++a) in.pos[a] = int32_t(take(4));
        in.rotation = int(take(1));
        in.seed = uint32_t(take(4));
        uint64_t params = take(1);
        if (params > kMaxInstanceParams) return std::format("bad instance {}", in.id);
        for (uint64_t k = 0; k < params; ++k) in.params.push_back(std::bit_cast<float>(uint32_t(take(4))));
        uint64_t form = v2 ? take(1) : 0;
        if (form > 1) return std::format("bad instance {}", in.id);
        if (form == 1) {
            glm::ivec3 lo, e;
            for (int a = 0; a < 3; ++a) lo[a] = int(take(2));
            for (int a = 0; a < 3; ++a) e[a] = int(take(1)) + 1;
            if (truncated || glm::any(glm::greaterThan(lo + e, size))) return std::format("bad instance {}", in.id);
            for (int64_t i = 0, volume = int64_t(e.x) * e.y * e.z; i < volume;) {
                Block before = Block(take(1)), after = Block(take(1));
                int64_t count = 0;
                for (int shift = 0;; shift += 7) {
                    uint64_t b = take(1);
                    if (truncated || shift > 21) return "truncated instances";
                    count |= int64_t(b & 127) << shift;
                    if (!(b & 128)) break;
                }
                if (++count > volume - i || before >= BlockCount || after >= BlockCount) return "bad instance cell";
                if (before != after) {
                    if ((total += uint64_t(count)) > kMaxInstanceCells) return "instances hold too many cells";
                    for (int64_t k = i; k < i + count; ++k) {
                        glm::ivec3 c = lo + glm::ivec3(int(k % e.x), int(k / (int64_t(e.x) * e.z)), int(k / e.x % e.z));
                        in.cells.push_back({uint16_t(c.x), uint16_t(c.y), uint16_t(c.z), before, after});
                    }
                }
                i += count;
            }
        } else {
            uint64_t cells = take(4);
            if (truncated || uint64_t(end - p) / 8 < cells) return "truncated instances";
            if ((total += cells) > kMaxInstanceCells) return "instances hold too many cells";
            in.cells.resize(cells);
            for (Instance::Cell& c : in.cells) {
                c = {uint16_t(get(p, 2)), uint16_t(get(p + 2, 2)), uint16_t(get(p + 4, 2)), Block(p[6]), Block(p[7])};
                p += 8;
                if (c.x >= size.x || c.y >= size.y || c.z >= size.z || c.before >= BlockCount || c.after >= BlockCount)
                    return "bad instance cell";
            }
        }
        glm::ivec3 lo(INT_MAX), hi(INT_MIN);
        for (const Instance::Cell& c : in.cells) lo = glm::min(lo, glm::ivec3(c.x, c.y, c.z)), hi = glm::max(hi, glm::ivec3(c.x, c.y, c.z));
        if (in.id == 0 || in.id == UINT32_MAX || (!out.empty() && in.id <= out.back().id) || in.rotation > 3 || in.cells.empty() ||
            glm::any(glm::greaterThanEqual(hi - lo, glm::ivec3(kMaxInstanceExtent))) ||
            std::ranges::any_of(in.params, [](float f) { return !std::isfinite(f); }) || in.pos.x < 0 || in.pos.z < 0 ||
            in.pos.y < -1 || glm::any(glm::greaterThanEqual(in.pos, size)))
            return std::format("bad instance {}", in.id);
        out.push_back(std::move(in));
    }
    if (truncated) return "truncated instances";
    return p == end ? std::string() : "instance section size mismatch";
}

std::string readWorldFile(const char* path, Grid& g, uint32_t& seed, std::vector<Instance>& instances,
                          std::vector<Section>& extra) {
    std::error_code ec;
    uintmax_t fileBytes = std::filesystem::file_size(path, ec);
    std::ifstream f(path, std::ios::binary);
    if (ec || !f) return "cannot open the file";
    std::vector<uint8_t> data(fileBytes);
    if (!f.read(reinterpret_cast<char*>(data.data()), std::streamsize(fileBytes))) return "cannot read the file";

    constexpr size_t kHeader = 24;
    if (data.size() < kHeader + 4 || std::memcmp(data.data(), "FLYW", 4) != 0) return "not a flycoos world file";
    const uint8_t *p = data.data(), *end = p + data.size() - 4;
    if (fnv1a(p, data.size() - 4) != get(end, 4)) return "corrupt or truncated (checksum mismatch)";
    if (uint32_t version = uint32_t(get(p + 4, 4)); version != kWorldFileVersion)
        return std::format("unsupported version {} (this build reads {})", version, kWorldFileVersion);
    glm::ivec3 size{int32_t(get(p + 8, 4)), int32_t(get(p + 12, 4)), int32_t(get(p + 16, 4))};
    if (glm::any(glm::lessThan(size, glm::ivec3(kChunk))) || glm::any(glm::greaterThan(size, glm::ivec3(4096))) ||
        size % kChunk != glm::ivec3(0) || int64_t(size.x) * size.y * size.z > int64_t(1) << 30)
        return std::format("bad world size {}x{}x{}", size.x, size.y, size.z);
    seed = uint32_t(get(p + 20, 4));
    if (g.size != size) g = Grid(size);
    instances.clear();
    extra.clear();

    bool voxels = false;
    for (p += kHeader; p < end;) {
        if (end - p < 12) return "truncated section header";
        const uint8_t* tag = p;
        uint64_t bytes = get(p + 4, 8);
        p += 12;
        if (bytes > uint64_t(end - p)) return "truncated section";
        const uint8_t* next = p + bytes;
        if (std::memcmp(tag, "VOXL", 4) == 0) {
            glm::ivec3 counts = size / kChunk;
            int n = counts.x * counts.y * counts.z;
            std::vector<std::pair<const uint8_t*, const uint8_t*>> spans(static_cast<size_t>(n));
            for (auto& [from, to] : spans) {
                if (next - p < 4 || uint64_t(next - p - 4) < get(p, 4)) return "truncated chunk";
                from = p + 4;
                to = p = from + get(p, 4);
            }
            if (p != next) return "voxel section size mismatch";
            std::atomic<bool> ok{true};
            parallelFor(n, [&](int i) {
                if (!decodeChunk(g, chunkOrigin(size, i), spans[size_t(i)].first, spans[size_t(i)].second)) ok = false;
            });
            if (!ok) return "corrupt chunk data";
            voxels = true;
        } else if (bool v2 = std::memcmp(tag, "INS2", 4) == 0; v2 || std::memcmp(tag, "INST", 4) == 0) {
            if (!instances.empty()) return "two instance sections";
            if (std::string error = readInstances(p, next, size, v2, instances); !error.empty()) return error;
        } else {
            Section& sec = extra.emplace_back();
            std::memcpy(sec.tag, tag, 4);
            sec.bytes.assign(p, next);
        }
        p = next;
    }
    return voxels ? std::string() : "no voxel section";
}
