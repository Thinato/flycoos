#include "world.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>
#include <thread>

#include <FastNoiseLite.h>

void parallelFor(int count, const std::function<void(int)>& fn) {
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t)
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

// Per-voxel jitter in [-0.45, 0.45] to roughen rock and canopy surfaces.
static float jitter(glm::ivec3 p, uint32_t seed) {
    return float(hash(p.x, p.z, seed ^ uint32_t(p.y) * 0x9e3779b9u) & 255) / 255.0f * 0.9f - 0.45f;
}

static int topY(const Grid& g, int x, int z) {
    for (int y = g.size.y - 1; y >= 0; --y)
        if (g.get({x, y, z}) != Air) return y;
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

static bool nearEdge(const Grid& g, int x, int z, int margin) {
    return x < margin || z < margin || x >= g.size.x - margin || z >= g.size.z - margin;
}

void growTree(Grid& g, glm::ivec3 at, int trunk, float radius, uint32_t seed, glm::ivec3 jitterOrigin) {
    int x = at.x, y = at.y, z = at.z;
    int thick = trunk >= 12 ? 2 : 1, r = int(std::ceil(radius));
    for (int dz = 0; dz < thick; ++dz)
        for (int dx = 0; dx < thick; ++dx)
            for (int ty = y + trunk; ty > y - 3 && !isOpaque(g.get({x + dx, ty, z + dz})); --ty)
                g.set({x + dx, ty, z + dz}, Wood);  // grows down to the ground on slopes

    glm::vec3 center{x + thick * 0.5f, y + trunk + 0.5f, z + thick * 0.5f};
    for (int dy = -r; dy <= r; ++dy)
        for (int dz = -r - 1; dz <= r + 1; ++dz)
            for (int dx = -r - 1; dx <= r + 1; ++dx) {
                glm::ivec3 p = glm::ivec3(glm::floor(center)) + glm::ivec3(dx, dy, dz);
                glm::vec3 d = (glm::vec3(p) + 0.5f - center) / glm::vec3(radius, radius * 0.7f, radius);
                if (glm::dot(d, d) + jitter(p - jitterOrigin, seed) * 0.4f <= 1 && g.get(p) == Air) g.set(p, Leaves);
            }
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

// A conifer: stacked tiers of needles, each widest at its foot, narrowing to a spike above the trunk.
void growPine(Grid& g, glm::ivec3 at, int height, uint32_t seed, glm::ivec3 jitterOrigin) {
    int x = at.x, y = at.y, z = at.z, top = y + height - 3;
    for (int ty = top; ty > y - 3 && !isOpaque(g.get({x, ty, z})); --ty) g.set({x, ty, z}, Wood);
    float widest = std::clamp(float(height) * 0.3f, 2.0f, 6.0f);
    int r = int(std::ceil(widest + 0.5f)) + 1, base = y + std::max(2, height / 4), tip = y + height + 1;
    for (int ty = base; ty <= tip; ++ty) {
        float t = float(ty - base) / float(tip - base);
        float radius = widest * (1 - t) * (0.3f + 0.35f * float((tip - ty) % 3)) + 0.5f;
        if (ty <= top) radius = std::max(radius, 1.2f);  // needles hide the trunk
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                glm::ivec3 p{x + dx, ty, z + dz};
                if (std::sqrt(float(dx * dx + dz * dz)) + jitter(p - jitterOrigin, seed) * 0.3f <= radius && g.get(p) == Air)
                    g.set(p, Leaves);
            }
    }
}

// A low, round clump of leaves on the ground.
void growBush(Grid& g, glm::ivec3 at, float radius, uint32_t seed, glm::ivec3 jitterOrigin) {
    int x = at.x, y = at.y, z = at.z;
    glm::vec3 center{x + 0.5f, y + 1 + radius * 0.35f, z + 0.5f};
    int r = int(std::ceil(radius)) + 1;
    for (int ty = y - 2; ty <= y + 1 + r; ++ty)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                glm::ivec3 p{x + dx, ty, z + dz};
                glm::vec3 d = (glm::vec3(p) + 0.5f - center) / glm::vec3(radius, radius * 0.75f, radius);
                if (glm::dot(d, d) + jitter(p - jitterOrigin, seed) * 0.5f <= 1 && g.get(p) == Air) g.set(p, Leaves);
            }
}

// Tall tree: 8-14 block trunk (2x2 when 12+), flattened ragged canopy of radius ~4-5.
static void placeTree(Grid& g, int x, int z, uint32_t rnd, uint32_t seed) {
    int y = topY(g, x, z);
    int trunk = 8 + int((rnd >> 3) % 7);
    float radius = 2.5f + trunk * 0.2f;
    int r = int(std::ceil(radius));
    if (y < 0 || g.get({x, y, z}) != Grass || nearEdge(g, x, z, r + 2) || y + trunk + r + 1 >= g.size.y) return;
    growTree(g, {x, y, z}, trunk, radius, seed, glm::ivec3(0));
}

// Rock spire (10-23 tall, tapering) or a half-buried boulder.
static void placeRock(Grid& g, int x, int z, uint32_t rnd, bool spire, uint32_t seed) {
    int y = topY(g, x, z);
    float radius = spire ? 2.5f + float((rnd >> 8) % 3) * 0.75f : 2.0f + float((rnd >> 8) % 3);
    int height = spire ? 10 + int((rnd >> 12) % 14) : int(radius * 1.5f);
    int r = int(std::ceil(radius)) + 1;
    if (y < 0 || g.get({x, y, z}) == Water || nearEdge(g, x, z, r + 1) || y + height + 1 >= g.size.y) return;
    growRock(g, {x, y, z}, spire, radius, height, seed, glm::ivec3(0));
}

void generateTerrain(Grid& g, uint32_t seed) {
    // Region noises are low-frequency (features ~250-500 blocks) so each kind of land is vast.
    FastNoiseLite warp = makeNoise(seed + 10u, 0.003f, 1);
    warp.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
    warp.SetDomainWarpAmp(100);
    FastNoiseLite forestNoise = makeNoise(seed + 1u, 0.002f, 2);
    FastNoiseLite rockNoise = makeNoise(seed + 2u, 0.002f, 2);
    FastNoiseLite hills = makeNoise(seed, 0.006f, 4);
    FastNoiseLite mountains = makeNoise(seed + 3u, 0.0015f, 3);
    FastNoiseLite lakes = makeNoise(seed + 4u, 0.004f, 2);
    FastNoiseLite patches = makeNoise(seed + 5u, 0.02f, 1);

    // Region strengths (0..1) at a column: x = forest, y = rocky land. Neither = open plains.
    // Rocky land pushes forest out so the two stay mostly separate instead of cluttering together.
    auto regions = [&](int x, int z) {
        float wx = float(x), wz = float(z);
        warp.DomainWarp(wx, wz);  // organic region borders
        float rocky = glm::smoothstep(0.20f, 0.40f, rockNoise.GetNoise(wx, wz));
        float forest = glm::smoothstep(0.10f, 0.30f, forestNoise.GetNoise(wx, wz)) * (1 - 0.85f * rocky);
        return glm::vec2(forest, rocky);
    };

    // Columns are independent: fill terrain and water in parallel, one z-row per task.
    parallelFor(g.size.z, [&](int z) {
        for (int x = 0; x < g.size.x; ++x) {
            float fx = float(x), fz = float(z);
            glm::vec2 region = regions(x, z);
            float hillHeight = 8 + 10 * region.x + 50 * region.y;  // plains gentle, rocky land tall
            float hill = hills.GetNoise(fx, fz) * 0.5f + 0.5f;
            float mountain = std::max(0.0f, mountains.GetNoise(fx, fz) * 0.5f + 0.5f - 0.6f) / 0.4f;
            float lake = std::max(0.0f, -lakes.GetNoise(fx, fz) - 0.5f) * 50;
            int h = int(kSeaLevel + 5 + hill * hill * hillHeight + mountain * mountain * 80 - lake);
            h = std::clamp(h, 2, std::max(2, g.size.y - 30));  // headroom for tall trees and spires

            bool bareRock = region.y > 0.5f && patches.GetNoise(fx, fz) > 0.3f;
            Block top = h <= kSeaLevel + 1 ? Sand : h > 130 ? Snow : h > 110 || bareRock ? Stone : Grass;
            Block sub = top == Sand ? Sand : top == Grass ? Dirt : Stone;
            for (int y = 0; y <= h; ++y) g.set({x, y, z}, y == h ? top : y > h - 4 ? sub : Stone);
            for (int y = h + 1; y <= kSeaLevel; ++y) g.set({x, y, z}, Water);
        }
    });

    // Decoration is sequential (shapes cross column boundaries) and seeded per cell, so it's
    // deterministic. Trees: one candidate per 7x7 cell; dense in forests, a rare lone tree elsewhere.
    for (int cz = 0; cz < g.size.z / 7; ++cz)
        for (int cx = 0; cx < g.size.x / 7; ++cx) {
            uint32_t rnd = hash(cx, cz, seed);
            int x = cx * 7 + 1 + int(rnd % 5), z = cz * 7 + 1 + int((rnd >> 8) % 5);
            float chance = float((rnd >> 16) & 0xFFFF) / 65536.0f;
            if (chance < 0.75f * regions(x, z).x + 0.004f) placeTree(g, x, z, rnd, seed);
        }
    // Rocks: one candidate per 14x14 cell; spires and boulders on rocky land, a rare boulder elsewhere.
    for (int cz = 0; cz < g.size.z / 14; ++cz)
        for (int cx = 0; cx < g.size.x / 14; ++cx) {
            uint32_t rnd = hash(cx, cz, seed ^ 0x5bd1e995u);
            int x = cx * 14 + 2 + int(rnd % 10), z = cz * 14 + 2 + int((rnd >> 8) % 10);
            float chance = float((rnd >> 16) & 0xFFFF) / 65536.0f;
            float rocky = regions(x, z).y;
            if (chance < 0.4f * rocky + 0.006f) placeRock(g, x, z, rnd, rocky > 0.5f && (rnd >> 4) % 4 != 0, seed);
        }
}

glm::vec3 findSpawn(const Grid& g) { return findSpawn(g, {g.size.x / 2, g.size.z / 2}); }

glm::vec3 findSpawn(const Grid& g, glm::ivec2 near) {
    int cx = near.x, cz = near.y;
    for (int r = 0; r < std::max(g.size.x, g.size.z) / 2; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;  // ring only
                int x = cx + dx, z = cz + dz, y = topY(g, x, z);
                if (y >= 0 && g.get({x, y, z}) == Grass) return {x + 0.5f, y + 1.0f, z + 0.5f};
            }
    return {cx + 0.5f, float(g.size.y), cz + 0.5f};
}

// UTF-8 without control characters, bidi overrides or zero-width characters (which could hide or
// reorder a name's text); overlong forms and surrogates are invalid too.
static bool validName(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        auto c = uint8_t(s[i]);
        int n = c < 0x80 ? 0 : c >= 0xC2 && c <= 0xDF ? 1 : (c & 0xF0) == 0xE0 ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
        if (n < 0 || c < 0x20 || c == 0x7F || s.size() - i <= size_t(n)) return false;
        uint32_t cp = n ? c & (0x3F >> n) : c;
        for (int k = 1; k <= n; ++k) {
            auto t = uint8_t(s[i + size_t(k)]);
            if ((t & 0xC0) != 0x80) return false;
            cp = cp << 6 | (t & 0x3F);
        }
        if ((cp >= 0x80 && cp < 0xA0) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
            (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF || (n == 2 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
            (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)))
            return false;
        i += size_t(n) + 1;
    }
    return true;
}

const char* checkMarker(const Grid& g, const Marker& m) {
    if (m.kind >= MarkerKind::Count) return "unknown marker kind";
    if (m.entity >= EntityKind::Count) return "unknown entity kind";
    if (m.count < 1 || m.count > kMaxSpawnCount) return "count out of range (1..8)";
    if (m.name.size() > kMaxMarkerName) return "name longer than 48 bytes";
    if (!validName(m.name)) return "name must be UTF-8 text without control, bidi or zero-width characters";
    if (m.name.empty() && (m.kind == MarkerKind::Poi || m.kind == MarkerKind::Region)) return "a poi or region needs a name";
    if (!(m.pos.x >= 0 && m.pos.x < float(g.size.x) && m.pos.z >= 0 && m.pos.z < float(g.size.z) && m.pos.y >= 0 &&
          m.pos.y <= float(g.size.y)))
        return "position outside the world";  // NaN fails every comparison
    if (!(m.radius >= 0 && m.radius <= kMaxMarkerRadius)) return "radius out of range (0..512)";
    return nullptr;
}

const Marker* regionAt(const std::vector<Marker>& markers, glm::vec3 p) {
    const Marker* best = nullptr;
    for (const Marker& m : markers)
        if (m.kind == MarkerKind::Region && glm::distance(glm::vec2(p.x, p.z), glm::vec2(m.pos.x, m.pos.z)) <= m.radius &&
            (!best || m.radius < best->radius))
            best = &m;
    return best;
}

// World file layout (little-endian):
//   "FLYW" | u32 version | i32 size x, y, z | u32 seed
//   sections, each: char tag[4] | u64 bytes | payload. Unknown tags are skipped, which leaves room
//     for later sections without a version bump.
//   u32 FNV-1a of every byte before it: any corruption is rejected instead of loading a wrong world.
// "VOXL" section: per 32^3 chunk (in chunkOrigin order): u32 bytes, then runs of
//   (u8 block, LEB128 count - 1) over the chunk's voxels, x fastest, then z, then y. Terrain is
//   mostly horizontal layers, so whole layers of stone or air collapse into single runs.
// "INST" section (only when there are instances): u32 count, then per instance: u32 id | u8 name
//   length | name | i32 x, y, z | u8 rotation | u32 seed | u8 param count | f32 params |
//   u32 cell count | cells (u16 x, y, z | u8 before | u8 after).
constexpr uint32_t kWorldFileVersion = 1;
constexpr int kChunkVoxels = kChunk * kChunk * kChunk;

static uint32_t fnv1a(const uint8_t* p, size_t n) {
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

std::string saveWorld(const char* path, const Grid& g, uint32_t seed, const std::vector<Instance>& instances,
                      const std::vector<Marker>& markers) {
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
        out.insert(out.end(), {'I', 'N', 'S', 'T'});
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
            put(out, in.cells.size(), 4);
            for (const Instance::Cell& c : in.cells) {
                put(out, c.x, 2), put(out, c.y, 2), put(out, c.z, 2);
                out.push_back(c.before), out.push_back(c.after);
            }
        }
        endSection();
    }
    out.insert(out.end(), {'M', 'A', 'R', 'K'});  // always: an empty one means "no markers", a missing one "older file"
    sizeAt = out.size();
    put(out, 0, 8);
    put(out, markers.size(), 4);
    for (const Marker& m : markers) {
        put(out, m.id, 4);
        put(out, uint8_t(m.kind), 1);
        put(out, m.name.size(), 1);
        out.insert(out.end(), m.name.begin(), m.name.end());
        for (float f : {m.pos.x, m.pos.y, m.pos.z, m.radius}) put(out, std::bit_cast<uint32_t>(f), 4);
        put(out, uint8_t(m.entity), 1);
        put(out, uint32_t(m.count), 1);
    }
    endSection();
    put(out, fnv1a(out.data(), out.size()), 4);

    // Write a temp file and rename it over the target, so a failed save never destroys the last good one.
    std::filesystem::path target(path), tmp = target;
    tmp += ".tmp";
    std::error_code ec;
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        if (!f.flush()) {
            std::filesystem::remove(tmp, ec);
            return std::format("cannot write {}", tmp.string());
        }
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::string error = std::format("cannot replace {}: {}", target.string(), ec.message());
        std::filesystem::remove(tmp, ec);
        return error;
    }
    return {};
}

// Reads the INST payload [p, end) into `out`; returns why it's malformed, or "".
static std::string readInstances(const uint8_t* p, const uint8_t* end, glm::ivec3 size, std::vector<Instance>& out) {
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

// Reads the MARK payload [p, end) into `out`; returns why it's malformed, or "".
static std::string readMarkers(const uint8_t* p, const uint8_t* end, const Grid& g, std::vector<Marker>& out) {
    bool truncated = false;
    auto take = [&](int64_t bytes) -> uint64_t {  // a little-endian number, or 0 past the end
        if (end - p < bytes) return truncated = true, p = end, 0;
        p += bytes;
        return get(p - bytes, int(bytes));
    };
    uint64_t count = take(4);
    int npcs = 0;
    if (count > kMaxMarkers) return std::format("more than {} markers", kMaxMarkers);
    for (uint64_t i = 0; i < count && !truncated; ++i) {
        Marker m;
        m.id = uint32_t(take(4));
        m.kind = MarkerKind(take(1));
        uint64_t nameBytes = take(1);
        if (end - p < int64_t(nameBytes)) return "truncated markers";
        m.name.assign(p, p + nameBytes);
        p += nameBytes;
        float f[4];
        for (float& v : f) v = std::bit_cast<float>(uint32_t(take(4)));
        m.pos = {f[0], f[1], f[2]};
        m.radius = f[3];
        m.entity = EntityKind(take(1));
        m.count = int(take(1));
        if (truncated) break;
        bool secondPlayerSpawn = m.kind == MarkerKind::PlayerSpawn && std::ranges::find(out, MarkerKind::PlayerSpawn, &Marker::kind) != out.end();
        if (m.id == 0 || m.id == UINT32_MAX || (!out.empty() && m.id <= out.back().id) || checkMarker(g, m) || secondPlayerSpawn)
            return std::format("bad marker {}", m.id);
        if (m.kind == MarkerKind::Spawn && (npcs += m.count) > kMaxSpawnedNpcs)
            return std::format("spawn markers make more than {} NPCs", kMaxSpawnedNpcs);
        out.push_back(std::move(m));
    }
    if (truncated) return "truncated markers";
    return p == end ? std::string() : "marker section size mismatch";
}

std::string loadWorld(const char* path, Grid& g, uint32_t& seed, std::vector<Instance>& instances,
                      std::optional<std::vector<Marker>>& markers) {
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
    markers.reset();

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
        } else if (std::memcmp(tag, "INST", 4) == 0) {
            if (std::string error = readInstances(p, next, size, instances); !error.empty()) return error;
        } else if (std::memcmp(tag, "MARK", 4) == 0) {
            if (std::string error = readMarkers(p, next, g, markers.emplace()); !error.empty()) return error;
        }
        p = next;
    }
    return voxels ? std::string() : "no voxel section";
}
