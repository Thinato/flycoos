#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "edit.h"  // Box

// Voxel light, two channels of 0..15 (Minecraft-style flood fill; docs/DECISIONS.md ADR-019).
// Sky light is 15 in every voxel open to the sky straight above, and spreads from there losing
// lightCost per voxel entered. Block light floods the same way from emitters (Ember, Lantern).
// Air, plants, leaves and water cost 1, everything else stops light. Only air and plants pass open sky down
// without loss, so the ground under a canopy or in a lake is shadier.
// Light depends only on the voxels, never on the time of day: the shader scales sky light by the sun.
constexpr int kMaxLight = 15;

inline int lightCost(Block b) { return b == Air || isPlant(b) || isLeaf(b) || b == Water ? 1 : 0; }
inline int emission(Block b) { return b == Ember ? 13 : b == Lantern ? 11 : 0; }

// One byte per voxel, sky << 4 | block, kept per 32^3 chunk (chunkOrigin order). A chunk holds a
// single value until one of its voxels differs, so open sky (15) and solid ground (0) cost nothing.
struct Light {
    struct Chunk {
        uint8_t fill = 0;                  // every voxel's value while `cells` is null
        std::unique_ptr<uint8_t[]> cells;  // kChunk^3 values, x fastest, then z, then y
        void set(int i, uint8_t v);
    };
    glm::ivec3 size{}, chunkCount{};
    std::vector<Chunk> chunks;

    explicit Light(glm::ivec3 size = {});
    uint8_t get(glm::ivec3 p) const;  // outside the volume: open sky
    void set(glm::ivec3 p, uint8_t v);
    size_t bytes() const;  // in allocated chunk arrays
    int allocated() const;  // chunks with their own array
};
inline int skyLight(uint8_t v) { return v >> 4; }
inline int blockLight(uint8_t v) { return v & 15; }

// Computes the light of every voxel of g (light is resized to g).
void fillLight(const Grid& g, Light& light);
// After the voxels in `edit` changed: recomputes the columns within kMaxLight of it (all heights,
// as sky light falls any distance) from the voxels and the unchanged light around them. Returns the
// box of voxels whose light changed; the faces in front of them need remeshing.
Box relight(const Grid& g, Light& light, Box edit);
