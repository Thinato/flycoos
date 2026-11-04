#include "plants.h"

#include <algorithm>
#include <cmath>

#include <FastNoiseLite.h>

#include "edit.h"
#include "mesher.h"

namespace {

// Small deterministic generator for the model shapes (same models every run).
struct Rng {
    uint32_t s;
    uint32_t next() { return s = s * 1664525u + 1013904223u, s >> 8; }
    int range(int lo, int hi) { return lo + int(next() % uint32_t(hi - lo + 1)); }
};

// A blade rooted at (x, z), `h` voxels tall: straight, its top quarter stepped one voxel away from
// the middle (straight columns merge into few quads; every step costs about four more).
void blade(Grid& g, int x, int z, int h, Block b, bool straight) {
    int sx = x < 5 ? -1 : 1, sz = z < 5 ? -1 : 1, kink = straight || h < 4 ? h : h - std::max(1, h / 4);
    bool alongX = (x + z) % 2 == 0;  // step along one axis only
    for (int y = 0; y < h; ++y) {
        int ox = y >= kink && alongX ? sx : 0, oz = y >= kink && !alongX ? sz : 0;
        g.set({std::clamp(x + ox, 0, kPlantVoxels - 1), y, std::clamp(z + oz, 0, kPlantVoxels - 1)}, b);
    }
}

// Far: every other blade, all straight (the far model, drawn beyond kPlantNear).
void tuft(Grid& g, Rng& r, int blades, int lo, int hi, Block b, bool far) {
    for (int i = 0; i < blades; ++i) {
        int x = r.range(1, 8), z = r.range(1, 8), h = r.range(lo, hi);
        if (!far || i % 2 == 0) blade(g, x, z, h, b, far);
    }
}

// A flower on a stem at (x, z) with its head on top: a ring of petals around a heart, or for
// bellflowers a bell hanging from the bent tip.
void flower(Grid& g, int x, int z, int h, Block petal, Block heart, bool bell) {
    for (int y = 0; y < h; ++y) g.set({x, y, z}, Stem);
    if (bell) {
        g.set({x + 1, h - 1, z}, Stem);
        for (int y = h - 3; y <= h - 2; ++y)
            for (int dz = 0; dz <= 1; ++dz) g.set({x + 1, y, z + dz}, petal), g.set({x + 2, y, z + dz}, petal);
        return;
    }
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) g.set({x + dx, h, z + dz}, dx || dz ? petal : heart);
}

}  // namespace

Grid buildPlant(PlantModel m, bool far) {
    Grid g{glm::ivec3(kPlantVoxels)};
    Rng r{uint32_t(m) * 2654435761u + 7};
    switch (m) {
    case ShortTuft0: case ShortTuft1: case ShortTuft2: tuft(g, r, 7, 2, 4, Blade, far); break;
    case TallTuft0: case TallTuft1: case TallTuft2:
        tuft(g, r, 4, 2, 4, Blade, far);
        tuft(g, r, 6, 5, 8, TallBlade, far);
        break;
    case Daisies: tuft(g, r, 4, 2, 3, Blade, far), flower(g, 3, 3, 4, PetalWhite, FlowerHeart, false), flower(g, 6, 6, 3, PetalWhite, FlowerHeart, false); break;
    case Buttercups: tuft(g, r, 4, 2, 3, Blade, far), flower(g, 2, 5, 5, PetalYellow, PetalYellow, false), flower(g, 6, 3, 4, PetalYellow, PetalYellow, false); break;
    case Poppies: tuft(g, r, 4, 2, 4, Blade, far), flower(g, 4, 4, 6, PetalRed, Eye, false); break;
    case Bellflowers: tuft(g, r, 4, 2, 4, Blade, far), flower(g, 3, 4, 6, PetalViolet, PetalViolet, true); break;
    case PlantModelCount: break;
    }
    return g;
}

std::vector<Quad> plantQuads(PlantModel m, bool far) {
    Grid g = buildPlant(m, far);
    std::vector<Quad> quads = meshRegion(g, {0, 0, 0}, g.size).opaque;
    std::erase_if(quads, [](Quad q) { QuadInfo i = unpack(q); return i.face == 3 && i.pos.y == 0; });  // on the ground
    return quads;
}

uint32_t plantHash(glm::ivec3 c) {
    uint32_t n = uint32_t(c.x) * 73856093u ^ uint32_t(c.y) * 19349663u ^ uint32_t(c.z) * 83492791u;
    n = n * 747796405u + 2891336453u;  // pcg
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    return (n >> 22u) ^ n;
}

PlantModel plantModel(Block plant, glm::ivec3 cell) {
    uint32_t pick = plantHash(cell) % 3;
    switch (plant) {
    case TallGrass: return PlantModel(TallTuft0 + pick);
    case FlowerWhite: return Daisies;
    case FlowerYellow: return Buttercups;
    case FlowerRed: return Poppies;
    case FlowerViolet: return Bellflowers;
    default: return PlantModel(ShortTuft0 + pick);
    }
}

namespace {

FastNoiseLite field(int seed, float frequency) {
    FastNoiseLite n{seed};
    n.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    n.SetFrequency(frequency);
    return n;
}
const FastNoiseLite kLush = field(71, 1.0f / 40), kTall = field(72, 1.0f / 18), kBloom = field(73, 1.0f / 11),
                    kFlowerKind = field(74, 1.0f / 60);

float smooth(float lo, float hi, float v) {
    float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

}  // namespace

Block meadowPlant(int x, int z, Block kind, float density) {
    float fx = float(x), fz = float(z);
    uint32_t h = plantHash({x, 0x5eed, z});
    float roll = float(h & 0xFFFF) / 65536.0f, pick = float(h >> 16) / 65536.0f;
    float lush = smooth(-0.45f, 0.5f, kLush.GetNoise(fx, fz));
    if (roll >= density * (0.06f + 0.6f * lush)) return Air;  // bare gaps ~6%, lush stretches ~66% of columns
    if (kind != ShortGrass) return kind;
    float bloom = smooth(0.35f, 0.7f, kBloom.GetNoise(fx, fz));
    if (pick < 0.012f + 0.5f * bloom) {  // a lone flower here and there, many in a cluster
        float k = kFlowerKind.GetNoise(fx, fz);  // wide areas of one kind (quartiles of the noise, roughly)
        return k < -0.3f ? FlowerWhite : k < 0 ? FlowerYellow : k < 0.3f ? FlowerRed : FlowerViolet;
    }
    return pick < 0.12f + 0.75f * smooth(0.1f, 0.45f, kTall.GetNoise(fx, fz)) ? TallGrass : ShortGrass;
}

void plantColumn(Grid& g, int x, int z, Block kind, float density) {
    int top = groundTop(g, x, z);  // leaves count: nothing grows under a tree
    if (top < 0) return;
    glm::ivec3 above(x, top + 1, z), under(x, groundUnderTrees(g, x, z) + 1, z);  // under a canopy: placed before its tree
    for (glm::ivec3 p : {above, under})
        if (isPlant(g.get(p))) g.set(p, Air);
    if (kind == Air || g.get({x, top, z}) != Grass || g.get(above) != Air) return;
    if (Block b = meadowPlant(x, z, kind, density); b != Air) g.set(above, b);
}
