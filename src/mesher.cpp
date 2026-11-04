#include "mesher.h"

#include <cassert>

#include "plants.h"

Quad pack(const QuadInfo& q) {
    uint32_t ao = 0;
    for (int c = 0; c < 4; ++c) ao |= uint32_t(q.ao[c]) << (2 * c);
    return {uint32_t(q.pos.x) | uint32_t(q.pos.y) << 6 | uint32_t(q.pos.z) << 12 |
                uint32_t(q.w - 1) << 18 | uint32_t(q.h - 1) << 24,
            uint32_t(q.face) | ao << 3 | uint32_t(q.block) << 11 | uint32_t(q.sky << 4 | q.glow) << 19};
}

QuadInfo unpack(Quad q) {
    QuadInfo r{};
    r.pos = {int(q.a & 63), int(q.a >> 6 & 63), int(q.a >> 12 & 63)};
    r.w = int(q.a >> 18 & 63) + 1;
    r.h = int(q.a >> 24 & 63) + 1;
    r.face = int(q.b & 7);
    for (int c = 0; c < 4; ++c) r.ao[c] = int(q.b >> (3 + 2 * c) & 3);
    r.block = Block(q.b >> 11 & 255);
    r.sky = int(q.b >> 23 & 15);
    r.glow = int(q.b >> 19 & 15);
    return r;
}

// Ambient occlusion of the 4 corners of a face whose outside cell is q.
static uint32_t faceAO(const Grid& g, glm::ivec3 q, glm::ivec3 U, glm::ivec3 V) {
    auto solid = [&](glm::ivec3 p) { return int(isOpaque(g.get(p))); };
    constexpr int cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
    uint32_t packed = 0;
    for (int c = 0; c < 4; ++c) {
        glm::ivec3 du = U * (cu[c] ? 1 : -1), dv = V * (cv[c] ? 1 : -1);
        int s1 = solid(q + du), s2 = solid(q + dv), corner = solid(q + du + dv);
        int ao = s1 && s2 ? 0 : 3 - (s1 + s2 + corner);
        packed |= uint32_t(ao) << (2 * c);
    }
    return packed;
}

Mesh meshRegion(const Grid& g, glm::ivec3 o, glm::ivec3 e, const Light* light) {
    assert(e.x <= 32 && e.y <= 32 && e.z <= 32);  // 6-bit packed fields
    Mesh mesh;
    bool empty = true;
    for (int y = 0; y < e.y && empty; ++y)
        for (int z = 0; z < e.z && empty; ++z)
            for (int x = 0; x < e.x && empty; ++x) empty = g.get(o + glm::ivec3(x, y, z)) == Air;
    if (empty) return mesh;

    for (int y = 0; y < e.y; ++y)
        for (int z = 0; z < e.z; ++z)
            for (int x = 0; x < e.x; ++x) {
                glm::ivec3 p = o + glm::ivec3(x, y, z);
                Block b = g.get(p);
                if (!isPlant(b) || p.x >= 1024 || p.y >= 256 || p.z >= 1024) continue;  // PlantInstance's 28 bits (bigger worlds load)
                uint32_t lit = light ? light->get(p) : kMaxLight << 4;
                mesh.plants.push_back({uint32_t(p.x) | uint32_t(p.y) << 10 | uint32_t(p.z) << 18,
                                       lit | uint32_t(plantModel(b, p)) << 8});
            }

    std::vector<uint32_t> mask;  // per (u,v): 0 = no face, else light<<17 | 1<<16 | ao<<8 | block
    for (int face = 0; face < 6; ++face) {
        int a = face / 2, u = (a + 1) % 3, v = (a + 2) % 3;
        glm::ivec3 n(0), U(0), V(0);
        n[a] = face % 2 == 0 ? 1 : -1;
        U[u] = 1;
        V[v] = 1;
        for (int d = 0; d < e[a]; ++d) {
            mask.assign(size_t(e[u]) * e[v], 0);
            for (int j = 0; j < e[v]; ++j)
                for (int i = 0; i < e[u]; ++i) {
                    glm::ivec3 p = o;
                    p[a] += d; p[u] += i; p[v] += j;
                    Block b = g.get(p);
                    if (b == Air || isPlant(b)) continue;
                    Block nb = g.get(p + n);
                    bool visible = b == Water ? isClear(nb) : !isOpaque(nb);
                    if (!visible) continue;
                    uint32_t ao = b == Water ? 0xFF : faceAO(g, p + n, U, V);
                    uint32_t lit = light ? light->get(p + n) : kMaxLight << 4;
                    mask[size_t(j) * e[u] + i] = lit << 17 | 1u << 16 | ao << 8 | b;
                }
            // Greedy merge: grow along u, then along v, while the key (block + AO + light) matches.
            for (int j = 0; j < e[v]; ++j)
                for (int i = 0; i < e[u];) {
                    uint32_t key = mask[size_t(j) * e[u] + i];
                    if (!key) { ++i; continue; }
                    int w = 1, h = 1;
                    while (i + w < e[u] && mask[size_t(j) * e[u] + i + w] == key) ++w;
                    for (bool grow = true; j + h < e[v] && grow; h += grow)
                        for (int k = 0; k < w; ++k)
                            if (mask[size_t(j + h) * e[u] + i + k] != key) { grow = false; break; }
                    for (int jj = 0; jj < h; ++jj)
                        for (int k = 0; k < w; ++k) mask[size_t(j + jj) * e[u] + i + k] = 0;

                    QuadInfo q{};
                    q.pos[a] = d; q.pos[u] = i; q.pos[v] = j;
                    q.w = w; q.h = h; q.face = face;
                    for (int c = 0; c < 4; ++c) q.ao[c] = int(key >> (8 + 2 * c) & 3);
                    q.block = Block(key & 255);
                    q.sky = skyLight(uint8_t(key >> 17));
                    q.glow = blockLight(uint8_t(key >> 17));
                    (q.block == Water ? mesh.water : mesh.opaque).push_back(pack(q));
                    i += w;
                }
        }
    }
    return mesh;
}

std::vector<Mesh> meshChunks(const Grid& g, const Light* light) {
    glm::ivec3 counts = g.size / kChunk;
    int n = counts.x * counts.y * counts.z;
    std::vector<Mesh> meshes(static_cast<size_t>(n));
    parallelFor(n, [&](int i) { meshes[size_t(i)] = meshRegion(g, chunkOrigin(g.size, i), glm::ivec3(kChunk), light); });
    return meshes;
}

std::vector<size_t> remeshChunks(const Grid& g, glm::ivec3 dirtyLo, glm::ivec3 dirtyHi, std::vector<Mesh>& meshes,
                                 const Light* light) {
    glm::ivec3 lo = glm::max(dirtyLo - 1, glm::ivec3(0)) / kChunk, hi = glm::min(dirtyHi + 1, g.size - 1) / kChunk;
    glm::ivec3 counts = g.size / kChunk;
    std::vector<size_t> ids;
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x) ids.push_back(size_t(x + counts.x * (y + counts.y * z)));
    parallelFor(int(ids.size()), [&](int j) {
        size_t i = ids[size_t(j)];
        meshes[i] = meshRegion(g, chunkOrigin(g.size, int(i)), glm::ivec3(kChunk), light);
    });
    return ids;
}
