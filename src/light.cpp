#include "light.h"

#include <algorithm>
#include <cassert>
#include <cstring>

constexpr int kChunkCells = kChunk * kChunk * kChunk;

static int localIndex(glm::ivec3 p) { return ((p.y & (kChunk - 1)) * kChunk + (p.z & (kChunk - 1))) * kChunk + (p.x & (kChunk - 1)); }

void Light::Chunk::set(int i, uint8_t v) {
    if (!cells) {
        if (v == fill) return;
        cells = std::make_unique<uint8_t[]>(kChunkCells);
        std::memset(cells.get(), fill, kChunkCells);
    }
    cells[size_t(i)] = v;
}

Light::Light(glm::ivec3 s) : size(s), chunkCount((s + kChunk - 1) / kChunk) {
    chunks.resize(size_t(chunkCount.x) * chunkCount.y * chunkCount.z);
}

static size_t chunkIndex(glm::ivec3 n, glm::ivec3 p) {
    p /= kChunk;
    return size_t(p.x + n.x * (p.y + n.y * p.z));
}

uint8_t Light::get(glm::ivec3 p) const {
    if (glm::any(glm::lessThan(p, glm::ivec3(0))) || glm::any(glm::greaterThanEqual(p, size))) return kMaxLight << 4;
    const Chunk& c = chunks[chunkIndex(chunkCount, p)];
    return c.cells ? c.cells[size_t(localIndex(p))] : c.fill;
}

void Light::set(glm::ivec3 p, uint8_t v) { chunks[chunkIndex(chunkCount, p)].set(localIndex(p), v); }

size_t Light::bytes() const { return size_t(allocated()) * kChunkCells; }

int Light::allocated() const {
    return int(std::ranges::count_if(chunks, [](const Chunk& c) { return c.cells != nullptr; }));
}

// Flood queue entries: x | z << 11 | y << 22.
static uint32_t packPos(glm::ivec3 p) { return uint32_t(p.x) | uint32_t(p.z) << 11 | uint32_t(p.y) << 22; }
static glm::ivec3 unpackPos(uint32_t k) { return {int(k & 2047), int(k >> 22), int(k >> 11 & 2047)}; }

// Recomputes the light of the columns [lo.x, hi.x] x [lo.z, hi.z] (every height) from the voxels,
// seeded at the border by the light of the columns around them. With `changed`, also returns the
// box of voxels whose light changed.
static void relightColumns(const Grid& g, Light& light, glm::ivec2 lo, glm::ivec2 hi, Box* changed) {
    assert(g.size.x <= 2048 && g.size.z <= 2048 && g.size.y <= 1024);  // packPos
    // Sky top of each column (one past its highest non-air voxel), for the columns and a ring around.
    glm::ivec2 tlo = glm::max(lo - 1, glm::ivec2(0)), thi = glm::min(hi + 1, glm::ivec2(g.size.x, g.size.z) - 1);
    int tw = thi.x - tlo.x + 1;
    std::vector<int16_t> tops(size_t(tw) * size_t(thi.y - tlo.y + 1));
    parallelFor(thi.y - tlo.y + 1, [&](int row) {
        int16_t* top = &tops[size_t(row) * size_t(tw)];
        std::fill_n(top, tw, int16_t(-1));
        for (int y = g.size.y - 1, left = tw; y >= 0 && left; --y) {
            const Block* cells = &g.cells[g.index({tlo.x, y, tlo.y + row})];
            for (int x = 0; x < tw; ++x)
                if (top[x] < 0 && cells[x] != Air) top[x] = int16_t(y + 1), --left;
        }
        std::replace(top, top + tw, int16_t(-1), int16_t(0));
    });
    auto top = [&](int x, int z) { return int(tops[size_t(z - tlo.y) * size_t(tw) + size_t(x - tlo.x)]); };

    // Reset the chunks' voxels in the columns to open sky (15) above the tops, else dark, plus
    // emission; emitters seed the flood.
    glm::ivec3 clo = glm::ivec3(lo.x, 0, lo.y) / kChunk, chi = glm::ivec3(hi.x, g.size.y - 1, hi.y) / kChunk;
    glm::ivec3 cn = chi - clo + 1;
    std::vector<size_t> ids;
    for (int z = clo.z; z <= chi.z; ++z)
        for (int y = clo.y; y <= chi.y; ++y)
            for (int x = clo.x; x <= chi.x; ++x) ids.push_back(chunkIndex(light.chunkCount, glm::ivec3(x, y, z) * kChunk));
    std::vector<Light::Chunk> before;  // for `changed`: the chunks as they were
    if (changed)
        for (size_t i : ids) {
            const Light::Chunk& c = light.chunks[i];
            Light::Chunk& copy = before.emplace_back(c.fill, nullptr);
            if (c.cells) copy.cells = std::make_unique<uint8_t[]>(kChunkCells), std::memcpy(copy.cells.get(), c.cells.get(), kChunkCells);
        }
    std::vector<std::vector<uint32_t>> seeds(ids.size());
    parallelFor(int(ids.size()), [&](int j) {
        Light::Chunk& c = light.chunks[ids[size_t(j)]];
        glm::ivec3 o = (clo + glm::ivec3(j % cn.x, j / cn.x % cn.y, j / (cn.x * cn.y))) * kChunk;
        glm::ivec3 a = glm::max(o, glm::ivec3(lo.x, 0, lo.y));
        glm::ivec3 b = glm::min(o + kChunk, glm::ivec3(hi.x + 1, g.size.y, hi.y + 1));
        uint8_t values[kChunkCells];
        for (int i = 0; i < kChunkCells; ++i) values[i] = c.cells ? c.cells[size_t(i)] : c.fill;
        for (int y = a.y; y < b.y; ++y)
            for (int z = a.z; z < b.z; ++z)
                for (int x = a.x; x < b.x; ++x) {
                    int e = emission(g.cells[g.index({x, y, z})]);
                    values[localIndex({x, y, z})] = uint8_t((y >= top(x, z) ? kMaxLight << 4 : 0) | e);
                    if (e) seeds[size_t(j)].push_back(packPos({x, y, z}));
                }
        if (std::all_of(values, values + kChunkCells, [&](uint8_t v) { return v == values[0]; })) {
            c.cells.reset();
            c.fill = values[0];
        } else {
            if (!c.cells) c.cells = std::make_unique<uint8_t[]>(kChunkCells);
            std::memcpy(c.cells.get(), values, kChunkCells);
        }
    });
    std::vector<uint32_t> next;
    for (const std::vector<uint32_t>& s : seeds) next.insert(next.end(), s.begin(), s.end());

    // Sky seeds: the open-sky voxels next to a column that is dark at their height, and the voxel
    // above a column's top when light passes into it (leaves, water).
    for (int z = lo.y; z <= hi.y; ++z)
        for (int x = lo.x; x <= hi.x; ++x) {
            int t = top(x, z), m = t;
            for (glm::ivec2 d : {glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1)})
                if (glm::ivec2 n(x + d.x, z + d.y); n.x >= 0 && n.y >= 0 && n.x < g.size.x && n.y < g.size.z) m = std::max(m, top(n.x, n.y));
            for (int y = t; y < m; ++y) next.push_back(packPos({x, y, z}));
            if (t > 0 && t < g.size.y && lightCost(g.get({x, t - 1, z})) && t >= m) next.push_back(packPos({x, t, z}));
        }
    // Border seeds: the lit voxels of the columns just outside, which the edit cannot have changed.
    auto border = [&](int x, int z) {
        if (x < 0 || z < 0 || x >= g.size.x || z >= g.size.z) return;
        for (int y = 0; y < g.size.y; ++y)
            if (light.get({x, y, z})) next.push_back(packPos({x, y, z}));
    };
    for (int x = lo.x; x <= hi.x; ++x) border(x, lo.y - 1), border(x, hi.y + 1);
    for (int z = lo.y; z <= hi.y; ++z) border(lo.x - 1, z), border(hi.x + 1, z);

    // Flood both channels at once, one wave of raised voxels at a time. Raising only ever moves a
    // value toward the exact one (the brightest path), so the order doesn't matter.
    constexpr glm::ivec3 kDirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (std::vector<uint32_t> wave; !next.empty();) {
        wave.swap(next);
        next.clear();
        for (uint32_t k : wave) {
            glm::ivec3 p = unpackPos(k);
            uint8_t v = light.get(p);
            if (skyLight(v) <= 1 && blockLight(v) <= 1) continue;
            for (glm::ivec3 d : kDirs) {
                glm::ivec3 q = p + d;
                if (!g.inside(q)) continue;
                int cost = lightCost(g.cells[g.index(q)]);
                if (!cost) continue;
                uint8_t w = light.get(q);
                int sky = std::max(skyLight(v) - cost, skyLight(w)), block = std::max(blockLight(v) - cost, blockLight(w));
                if (uint8_t raised = uint8_t(sky << 4 | block); raised != w) {
                    light.set(q, raised);
                    next.push_back(packPos(q));
                }
            }
        }
    }

    if (!changed) return;
    *changed = {};
    for (size_t j = 0; j < ids.size(); ++j) {
        const Light::Chunk &was = before[j], &now = light.chunks[ids[j]];
        if (!was.cells && !now.cells && was.fill == now.fill) continue;
        glm::ivec3 o = (clo + glm::ivec3(int(j) % cn.x, int(j) / cn.x % cn.y, int(j) / (cn.x * cn.y))) * kChunk;
        for (int i = 0; i < kChunkCells; ++i)
            if ((was.cells ? was.cells[size_t(i)] : was.fill) != (now.cells ? now.cells[size_t(i)] : now.fill)) {
                glm::ivec3 p = o + glm::ivec3(i % kChunk, i / (kChunk * kChunk), i / kChunk % kChunk);
                if (changed->empty()) *changed = {p, p};
                changed->lo = glm::min(changed->lo, p);
                changed->hi = glm::max(changed->hi, p);
            }
    }
}

void fillLight(const Grid& g, Light& light) {
    light = Light(g.size);
    relightColumns(g, light, {0, 0}, {g.size.x - 1, g.size.z - 1}, nullptr);
}

Box relight(const Grid& g, Light& light, Box edit) {
    if (edit.empty()) return {};
    glm::ivec2 lo = glm::max(glm::ivec2(edit.lo.x, edit.lo.z) - kMaxLight, glm::ivec2(0));
    glm::ivec2 hi = glm::min(glm::ivec2(edit.hi.x, edit.hi.z) + kMaxLight, glm::ivec2(g.size.x, g.size.z) - 1);
    Box changed;
    relightColumns(g, light, lo, hi, &changed);
    return changed;
}
