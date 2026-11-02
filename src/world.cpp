#include "world.h"

#include <algorithm>
#include <atomic>
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

static int topY(const Grid& g, int x, int z) {
    for (int y = g.size.y - 1; y >= 0; --y)
        if (g.get({x, y, z}) != Air) return y;
    return -1;
}

void generateTerrain(Grid& g, uint32_t seed) {
    FastNoiseLite hills{int(seed)};
    hills.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    hills.SetFractalType(FastNoiseLite::FractalType_FBm);
    hills.SetFractalOctaves(5);
    hills.SetFrequency(0.0045f);
    FastNoiseLite mountains{int(seed + 1u)};
    mountains.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    mountains.SetFractalType(FastNoiseLite::FractalType_FBm);
    mountains.SetFractalOctaves(3);
    mountains.SetFrequency(0.0018f);
    FastNoiseLite forest{int(seed + 2u)};
    forest.SetFrequency(0.012f);

    // Columns are independent: fill terrain and water in parallel, one z-row per task.
    parallelFor(g.size.z, [&](int z) {
        for (int x = 0; x < g.size.x; ++x) {
            float n = hills.GetNoise(float(x), float(z));                    // [-1,1] rolling hills
            float m = mountains.GetNoise(float(x), float(z)) * 0.5f + 0.5f;  // [0,1] mountain mask
            int h = int(kSeaLevel + 3 + n * 16 + m * m * m * 70 * (0.6f + 0.4f * n));
            h = std::clamp(h, 2, std::max(2, g.size.y - 12));
            Block top = h <= kSeaLevel + 1 ? Sand : h > 92 ? Snow : h > 78 ? Stone : Grass;
            Block sub = top == Sand ? Sand : top == Grass ? Dirt : Stone;
            for (int y = 0; y <= h; ++y) g.set({x, y, z}, y == h ? top : y > h - 4 ? sub : Stone);
            for (int y = h + 1; y <= kSeaLevel; ++y) g.set({x, y, z}, Water);
        }
    });

    // Trees: at most one per 6x6 cell, denser where the forest noise is high.
    // Sequential because leaf blobs cross column boundaries.
    for (int cz = 0; cz < g.size.z / 6; ++cz)
        for (int cx = 0; cx < g.size.x / 6; ++cx) {
            uint32_t r = hash(cx, cz, seed);
            int x = cx * 6 + 1 + int(r % 4), z = cz * 6 + 1 + int((r >> 8) % 4);
            float chance = float((r >> 16) & 0xFFFF) / 65536.0f;
            float density = std::clamp((forest.GetNoise(float(x), float(z)) * 0.5f + 0.5f) * 1.2f - 0.3f, 0.03f, 0.85f);
            if (chance >= density || x < 3 || z < 3 || x > g.size.x - 4 || z > g.size.z - 4) continue;
            int y = topY(g, x, z), trunk = 4 + int((r >> 4) % 3);
            if (y < 0 || g.get({x, y, z}) != Grass || y + trunk + 3 >= g.size.y) continue;
            for (int i = 1; i <= trunk; ++i) g.set({x, y + i, z}, Wood);
            for (int dy = -2; dy <= 2; ++dy)
                for (int dz = -2; dz <= 2; ++dz)
                    for (int dx = -2; dx <= 2; ++dx) {
                        glm::ivec3 p{x + dx, y + trunk + dy, z + dz};
                        if (dx * dx + dz * dz + dy * dy * 1.3f <= 5.5f && g.get(p) == Air) g.set(p, Leaves);
                    }
            g.set({x, y + trunk + 3, z}, Leaves);
        }
}

glm::vec3 findSpawn(const Grid& g) {
    int cx = g.size.x / 2, cz = g.size.z / 2;
    for (int r = 0; r < std::max(g.size.x, g.size.z) / 2; ++r)
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;  // ring only
                int x = cx + dx, z = cz + dz, y = topY(g, x, z);
                if (y >= 0 && g.get({x, y, z}) == Grass) return {x + 0.5f, y + 1.0f, z + 0.5f};
            }
    return {cx + 0.5f, float(g.size.y), cz + 0.5f};
}
