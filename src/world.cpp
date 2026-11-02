#include "world.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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

// Tall tree: 8-14 block trunk (2x2 when 12+), flattened ragged canopy of radius ~4-5.
static void placeTree(Grid& g, int x, int z, uint32_t rnd, uint32_t seed) {
    int y = topY(g, x, z);
    int trunk = 8 + int((rnd >> 3) % 7), thick = trunk >= 12 ? 2 : 1;
    float radius = 2.5f + trunk * 0.2f;
    int r = int(std::ceil(radius));
    if (y < 0 || g.get({x, y, z}) != Grass || nearEdge(g, x, z, r + 2) || y + trunk + r + 1 >= g.size.y) return;

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
                if (glm::dot(d, d) + jitter(p, seed) * 0.4f <= 1 && g.get(p) == Air) g.set(p, Leaves);
            }
}

// Rock spire (10-23 tall, tapering) or a half-buried boulder.
static void placeRock(Grid& g, int x, int z, uint32_t rnd, bool spire, uint32_t seed) {
    int y = topY(g, x, z);
    float radius = spire ? 2.5f + float((rnd >> 8) % 3) * 0.75f : 2.0f + float((rnd >> 8) % 3);
    int height = spire ? 10 + int((rnd >> 12) % 14) : int(radius * 1.5f);
    int r = int(std::ceil(radius)) + 1;
    if (y < 0 || g.get({x, y, z}) == Water || nearEdge(g, x, z, r + 1) || y + height + 1 >= g.size.y) return;

    for (int dy = -3; dy <= height; ++dy)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                glm::ivec3 p{x + dx, y + dy, z + dz};
                float t = std::max(0.0f, float(dy)) / float(height);
                float d = spire ? std::sqrt(float(dx * dx + dz * dz)) / (radius * (1 - 0.65f * std::pow(t, 1.3f)))
                                : glm::length(glm::vec3(dx, (dy - radius * 0.3f) / 0.75f, dz)) / radius;
                if (d + jitter(p, seed) * 0.25f <= 1) g.set(p, Rock);
            }
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
