#include "trees.h"

#include <algorithm>
#include <cmath>
#include <random>

#include <FastNoiseLite.h>

namespace {

constexpr float kBump = 0.22f;   // how far smooth noise pushes a clump's surface (fraction of its radius)
constexpr float kTau = 6.2832f;  // a turn in radians, rounded (the shapes were tuned with it)
constexpr int kTrunkSink = 3;    // trunks and roots reach this far down, to meet the ground on slopes

struct Rng {
    std::mt19937 m;
    float operator()(float lo, float hi) { return lo + (hi - lo) * float(m() >> 8) / 16777216.0f; }
};

// The trunk's axis: centred on the ground voxel for odd widths, on its +x+z corner for even ones,
// so a width-w trunk is exactly w voxels across.
glm::vec3 trunkAxis(int width) { return {width % 2 ? 0.5f : 1.0f, 0, width % 2 ? 0.5f : 1.0f}; }

// Shades run from the crown's lowest leaves (0) to its top (3), whatever the species.
void spanCrown(TreeShape& t) {
    t.crownLo = 1e9f, t.crownHi = -1e9f;
    for (const TreeShape::Clump& c : t.leaves)
        t.crownLo = std::min(t.crownLo, c.c.y - c.radii.y * 0.85f), t.crownHi = std::max(t.crownHi, c.c.y + c.radii.y * 0.85f);
}

glm::vec3 dir(float azimuth, float elevation) {
    return {std::cos(azimuth) * std::cos(elevation), std::sin(elevation), std::sin(azimuth) * std::cos(elevation)};
}

// Broadleaf: a flared trunk that forks low into thick limbs, each ending in a leaf clump, around
// a clump on top. Limb tips sit at the clumps' undersides, so the limbs show below the crown.
TreeShape broadleaf(int height, float crown, Rng& rnd, bool maple) {
    TreeShape t;
    float h = float(height);
    int width = std::clamp(int(std::lround(h * 0.18f)), 1, 6);
    float r = width * 0.5f;
    if (crown <= 0) crown = maple ? 5.5f + h * 0.24f : 6 + h * 0.25f;  // 20-26 across at 20-28 tall
    glm::vec3 axis = trunkAxis(width);
    float fork = h * (maple ? 0.38f : 0.33f);
    float clumpR = crown * 0.38f, clumpH = std::max(2.0f, h * (maple ? 0.18f : 0.15f));
    t.wood.push_back({axis + glm::vec3(0, -kTrunkSink, 0), axis + glm::vec3(0, h * 0.62f, 0), r, r * 0.7f});
    if (width >= 3) {  // root flare and buttress roots
        t.wood.push_back({axis + glm::vec3(0, -kTrunkSink, 0), axis + glm::vec3(0, 2.5f, 0), r + 1.3f, r});
        for (int i = 0, n = 4 + int(rnd(0, 2)); i < n; ++i) {
            float a = rnd(-0.3f, 0.3f) + float(i) * kTau / float(n);
            t.wood.push_back({axis + glm::vec3(0, 2, 0), axis + dir(a, 0) * (r + 3.5f) + glm::vec3(0, -1, 0), r * 0.6f, 0.7f});
        }
    }
    int limbs = std::clamp(int(h / 6), 3, 5);
    float spin = rnd(0, kTau);
    for (int i = 0; i < limbs; ++i) {
        float a = spin + float(i) * kTau / float(limbs) + rnd(-0.3f, 0.3f);
        float base = fork + rnd(0, h * 0.12f), reach = crown - clumpR * rnd(0.9f, 1.1f);
        glm::vec3 out(std::cos(a), 0, std::sin(a));
        glm::vec3 from = axis + glm::vec3(0, base, 0);
        // Out at a low angle, then bending up into the clump: the elbow shows under the crown.
        glm::vec3 elbow = from + out * (reach * 0.6f) + glm::vec3(0, reach * (maple ? 0.45f : 0.3f), 0);
        glm::vec3 tip = from + out * reach;
        tip.y = std::min(h - clumpH * 1.5f, elbow.y + reach * (maple ? 0.5f : 0.35f));
        float r0 = std::max(0.8f, r * 0.5f), r1 = std::max(0.6f, r * 0.28f);
        t.wood.push_back({from, elbow, r0, (r0 + r1) * 0.5f});
        t.wood.push_back({elbow, tip, (r0 + r1) * 0.5f, r1});
        t.leaves.push_back({tip + glm::vec3(0, clumpH * 0.55f, 0), glm::vec3(clumpR, clumpH, clumpR) * rnd(0.9f, 1.1f)});
    }
    t.limbs = limbs;
    t.leaves.push_back({axis + glm::vec3(0, h - clumpH * 1.15f, 0), glm::vec3(crown * 0.5f, clumpH * 1.15f, crown * 0.5f)});
    return t;
}

// Birch: a slim, slightly leaning trunk and an oval crown of clumps along its upper half. No limbs.
TreeShape birch(int height, float crown, Rng& rnd) {
    TreeShape t;
    float h = float(height);
    int width = std::clamp(int(std::lround(h * 0.11f)), 1, 3);
    float r = width * 0.5f;
    if (crown <= 0) crown = h * 0.33f;  // 12-16 across at 18-24 tall
    glm::vec3 axis = trunkAxis(width), lean = dir(rnd(0, kTau), 0) * (h * 0.06f);
    glm::vec3 top = axis + lean + glm::vec3(0, h * 0.85f, 0);
    glm::vec3 knee = axis + glm::vec3(0, h * 0.3f, 0);  // straight up to here, then leaning
    t.wood.push_back({axis + glm::vec3(0, -kTrunkSink, 0), knee, r + 0.2f, r + 0.1f});
    t.wood.push_back({knee, top, r + 0.1f, std::max(0.5f, r * 0.7f)});
    float lo = h * 0.42f, clumpH = std::max(2.0f, h * 0.13f);
    int n = std::clamp(int(h / 6), 2, 4);
    for (int i = 0; i < n; ++i) {
        float k = (float(i) + 0.5f) / float(n), y = lo + clumpH + (h - lo - 2 * clumpH) * k;
        glm::vec3 c = axis + lean * std::max(0.0f, (y - h * 0.3f) / (h * 0.55f)) + glm::vec3(0, y, 0) + dir(rnd(0, kTau), 0) * (crown * 0.2f);
        float w = crown * (0.85f - 0.3f * std::abs(k - 0.4f));  // widest a little below the middle
        t.leaves.push_back({c, glm::vec3(w, clumpH * 1.3f, w) * rnd(0.9f, 1.05f)});
    }
    t.bark = BirchBark;
    t.leaf = Leaf::Birch;
    return t;
}

// Conifer: a straight trunk under stacked tiers of needles, each a flat disc narrowing to the tip.
TreeShape conifer(int height, float crown, Rng& rnd) {
    TreeShape t;
    float h = float(height);
    int width = std::clamp(int(std::lround(h * 0.1f)), 1, 3);
    float r = width * 0.5f;
    if (crown <= 0) crown = 2 + h * 0.16f;  // 12-16 across at 26-38 tall
    glm::vec3 axis = trunkAxis(width);
    t.wood.push_back({axis + glm::vec3(0, -kTrunkSink, 0), axis + glm::vec3(0, h - 2, 0), r + 0.1f, 0.5f});
    float base = h * 0.18f;
    int tiers = std::clamp(int(h / 4.5f), 3, 9);
    float step = (h - 1.5f - base) / float(tiers);
    for (int i = 0; i < tiers; ++i) {
        float k = float(i) / float(tiers), y = base + step * (float(i) + 0.4f);
        float w = std::max(1.2f, crown * (1 - k) * rnd(0.92f, 1.0f));
        t.leaves.push_back({axis + glm::vec3(0, y, 0), glm::vec3(w, step * 0.55f + 0.4f, w)});
    }
    t.leaves.push_back({axis + glm::vec3(0, h - 1.5f, 0), glm::vec3(1.1f, 1.6f, 1.1f)});  // the tip
    t.bark = PineBark;
    t.leaf = Leaf::Pine;
    return t;
}

// Araucaria: a tall bare trunk; at its top 2-3 whorls of limbs reach out level, then turn up,
// each holding a flat tuft, so together they make the flat, slightly cupped candelabra crown.
TreeShape araucaria(int height, float crown, Rng& rnd) {
    TreeShape t;
    float h = float(height);
    int width = std::clamp(int(std::lround(h * 0.09f)), 1, 5);
    float r = width * 0.5f;
    if (crown <= 0) crown = 3 + h * 0.25f;  // 22-28 across at 32-44 tall
    glm::vec3 axis = trunkAxis(width);
    float tuftH = std::max(1.5f, h * 0.045f), tuftR = std::max(2.0f, crown * 0.34f), rim = h - tuftH * 1.6f;
    t.wood.push_back({axis + glm::vec3(0, -kTrunkSink, 0), axis + glm::vec3(0, rim - 1, 0), r + 0.1f, std::max(0.6f, r * 0.75f)});
    if (width >= 3) t.wood.push_back({axis + glm::vec3(0, -kTrunkSink, 0), axis + glm::vec3(0, 2, 0), r + 1, r});
    int whorls = h >= 24 ? 3 : 2;
    for (int w = 0; w < whorls; ++w) {  // w = 0 the lowest and widest
        float y = rim - 2.5f - float(whorls - 1 - w) * std::max(2.5f, h * 0.07f);
        float reach = (crown - tuftR) * (1 - 0.3f * float(w)) * rnd(0.92f, 1.0f);
        int n = 6 + int(rnd(0, 3));
        float spin = rnd(0, kTau);
        for (int i = 0; i < n; ++i) {
            glm::vec3 out = dir(spin + float(i) * kTau / float(n) + rnd(-0.2f, 0.2f), 0);
            glm::vec3 from = axis + glm::vec3(0, y, 0), elbow = from + out * (reach * 0.75f) + glm::vec3(0, reach * 0.12f, 0);
            glm::vec3 tip = from + out * reach;
            tip.y = rim - 0.8f * float(w);  // the inner tufts sit a little lower: a shallow cup
            float r0 = std::max(0.6f, r * 0.4f);
            t.wood.push_back({from, elbow, r0, r0 * 0.8f});
            t.wood.push_back({elbow, tip, r0 * 0.8f, 0.5f});
            t.leaves.push_back({tip + glm::vec3(0, tuftH * 0.5f, 0), glm::vec3(tuftR, tuftH, tuftR) * rnd(0.9f, 1.1f)});
            ++t.limbs;
        }
    }
    t.leaves.push_back({axis + glm::vec3(0, rim, 0), glm::vec3(tuftR * 0.8f, tuftH, tuftR * 0.8f)});  // the top
    t.bark = AraucariaBark;
    t.leaf = Leaf::Araucaria;
    return t;
}

}  // namespace

TreeShape planTree(Species s, int height, float crown, int colour, uint32_t seed) {
    Rng rnd{std::mt19937(seed)};
    TreeShape t;
    switch (s) {
    case Species::Oak: t = broadleaf(height, crown, rnd, false); break;
    case Species::Maple:
        t = broadleaf(height, crown, rnd, true);
        t.leaf = colour == 1 ? Leaf::MapleOrange : colour == 2 ? Leaf::MapleGold : Leaf::MapleRed;
        break;
    case Species::Birch: t = birch(height, crown, rnd); break;
    case Species::Conifer: t = conifer(height, crown, rnd); break;
    case Species::Araucaria: t = araucaria(height, crown, rnd); break;
    case Species::Count: break;
    }
    spanCrown(t);
    return t;
}

TreeShape planBush(float radius, uint32_t seed) {
    Rng rnd{std::mt19937(seed)};
    TreeShape t;
    t.leaves.push_back({{0.5f, 1 + radius * 0.4f, 0.5f}, glm::vec3(radius, radius * 0.75f, radius)});
    for (int i = 0, n = radius >= 2 ? 1 + int(rnd(0, 2)) : 0; i < n; ++i)
        t.leaves.push_back({glm::vec3(0.5f, 1 + radius * 0.25f, 0.5f) + dir(rnd(0, kTau), 0) * (radius * 0.7f),
                            glm::vec3(radius * 0.65f, radius * 0.5f, radius * 0.65f)});
    spanCrown(t);
    return t;
}

Box shapeBox(const TreeShape& t, glm::ivec3 at) {
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const TreeShape::Branch& b : t.wood) {
        float r = std::max(b.r0, b.r1);
        lo = glm::min(lo, glm::min(b.a, b.b) - r), hi = glm::max(hi, glm::max(b.a, b.b) + r);
    }
    for (const TreeShape::Clump& c : t.leaves) {
        glm::vec3 e = c.radii * (1 + kBump);
        lo = glm::min(lo, c.c - e), hi = glm::max(hi, c.c + e);
    }
    return {at + glm::ivec3(glm::floor(lo)), at + glm::ivec3(glm::ceil(hi))};
}

void growShape(Grid& g, glm::ivec3 at, const TreeShape& t, uint32_t seed) {
    auto each = [&](glm::vec3 lo, glm::vec3 hi, auto&& fn) {  // voxels whose centre is in [lo, hi] (local)
        for (int y = int(std::floor(lo.y)); y <= int(std::ceil(hi.y)); ++y)
            for (int z = int(std::floor(lo.z)); z <= int(std::ceil(hi.z)); ++z)
                for (int x = int(std::floor(lo.x)); x <= int(std::ceil(hi.x)); ++x) fn(glm::ivec3(x, y, z), glm::vec3(x, y, z) + 0.5f);
    };
    for (const TreeShape::Branch& b : t.wood) {
        float r = std::max(b.r0, b.r1);
        glm::vec3 d = b.b - b.a;
        float len2 = std::max(glm::dot(d, d), 1e-6f);
        each(glm::min(b.a, b.b) - r, glm::max(b.a, b.b) + r, [&](glm::ivec3 p, glm::vec3 c) {
            float k = std::clamp(glm::dot(c - b.a, d) / len2, 0.0f, 1.0f);
            float rk = b.r0 + (b.r1 - b.r0) * k;
            glm::vec3 q = c - (b.a + d * k);
            if (glm::dot(q, q) > rk * rk || isOpaque(g.get(at + p))) return;
            for (int y = p.y + 1; y <= 0; ++y)  // below the ground voxel's level: only down to the first solid voxel
                if (isOpaque(g.get(at + glm::ivec3(p.x, y, p.z)))) return;
            g.set(at + p, t.bark);
        });
    }

    FastNoiseLite noise{int(seed)};
    noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
    noise.SetFrequency(0.16f);
    std::vector<glm::ivec3> placed;
    float span = std::max(t.crownHi - t.crownLo, 1.0f);
    for (const TreeShape::Clump& cl : t.leaves) {
        glm::vec3 e = cl.radii * (1 + kBump);
        each(cl.c - e, cl.c + e, [&](glm::ivec3 p, glm::vec3 c) {
            glm::vec3 q = (c - cl.c) / cl.radii;
            if (glm::length(q) + kBump * noise.GetNoise(c.x, c.y, c.z) > 1 || !isClear(g.get(at + p))) return;
            int shade = std::clamp(int((c.y - t.crownLo) / span * kLeafShades), 0, kLeafShades - 1);
            g.set(at + p, leafBlock(t.leaf, shade));
            placed.push_back(at + p);
        });
    }
    // No stray leaves: a leaf touching nothing solid across a face goes (removing one can't strand another).
    constexpr glm::ivec3 kFaces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (glm::ivec3 p : placed)
        if (std::ranges::none_of(kFaces, [&](glm::ivec3 f) { return isOpaque(g.get(p + f)); })) g.set(p, Air);
}
