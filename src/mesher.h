#pragma once
#include <array>
#include <vector>

#include "light.h"

// One greedy-merged face, packed into 8 bytes and decoded in shaders/world.metal (voxel_vs).
//   a: x:6 | y:6 | z:6 | (w-1):6 | (h-1):6          (position relative to the meshed region)
//   b: face:3 | ao:8 (4 corners x 2 bits) | block:8 | block light:4 | sky light:4
// Faces: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z. A face on axis a spans u=(a+1)%3 (w) and v=(a+2)%3 (h).
// AO corners in (u,v): 0=(0,0) 1=(1,0) 2=(1,1) 3=(0,1); 3 = fully lit, 0 = fully occluded.
// Light (light.h) is the voxel's in front of the face, one value per quad.
struct Quad {
    uint32_t a, b;
    bool operator==(const Quad&) const = default;
};
static_assert(sizeof(Quad) == 8);

struct QuadInfo {
    glm::ivec3 pos;
    int w, h, face;
    std::array<int, 4> ao;
    Block block;
    int sky = kMaxLight, glow = 0;  // sky and block light
    bool operator==(const QuadInfo&) const = default;
};

Quad pack(const QuadInfo& q);
QuadInfo unpack(Quad q);

// A plant (isPlant) to draw as a model (plants.h): its cell x:10 | y:8 | z:10, then the cell's
// light (sky << 4 | block) | PlantModel << 8. Decoded in shaders/world.metal (plant_vs).
struct PlantInstance {
    uint32_t cell, info;
    bool operator==(const PlantInstance&) const = default;
};
static_assert(kWorldSize.x <= 1024 && kWorldSize.y <= 256 && kWorldSize.z <= 1024, "PlantInstance packs cells in 28 bits");

struct Mesh { std::vector<Quad> opaque, water; std::vector<PlantInstance> plants; };

// Greedy-meshes the region [origin, origin+extent) of g (extent <= 32 per axis).
// Neighbours outside the region are read from g, so chunk borders and AO are seamless. Plants
// make no faces; they are listed in `plants` instead.
// Without `light` every face is under open sky (character models).
Mesh meshRegion(const Grid& g, glm::ivec3 origin, glm::ivec3 extent, const Light* light = nullptr);

// Every kChunk^3 chunk of g meshed (in parallel), indexed in chunkOrigin order.
std::vector<Mesh> meshChunks(const Grid& g, const Light* light = nullptr);
// After an edit of the voxels in [lo, hi] (inclusive, lo <= hi): remeshes, in parallel, the chunks of `meshes`
// (from meshChunks) that overlap it grown by one voxel (faces and AO look at neighbours), and returns
// their indices. Pass the voxels whose light changed too (relight's box).
std::vector<size_t> remeshChunks(const Grid& g, glm::ivec3 lo, glm::ivec3 hi, std::vector<Mesh>& meshes,
                                 const Light* light = nullptr);
