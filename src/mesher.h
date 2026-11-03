#pragma once
#include <array>
#include <vector>

#include "world.h"

// One greedy-merged face, packed into 8 bytes and decoded in shaders/world.metal (voxel_vs).
//   a: x:6 | y:6 | z:6 | (w-1):6 | (h-1):6          (position relative to the meshed region)
//   b: face:3 | ao:8 (4 corners x 2 bits) | block:8
// Faces: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z. A face on axis a spans u=(a+1)%3 (w) and v=(a+2)%3 (h).
// AO corners in (u,v): 0=(0,0) 1=(1,0) 2=(1,1) 3=(0,1); 3 = fully lit, 0 = fully occluded.
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
    bool operator==(const QuadInfo&) const = default;
};

Quad pack(const QuadInfo& q);
QuadInfo unpack(Quad q);

struct Mesh { std::vector<Quad> opaque, water; };

// Greedy-meshes the region [origin, origin+extent) of g (extent <= 32 per axis).
// Neighbours outside the region are read from g, so chunk borders and AO are seamless.
Mesh meshRegion(const Grid& g, glm::ivec3 origin, glm::ivec3 extent);

// Every kChunk^3 chunk of g meshed (in parallel), indexed in chunkOrigin order.
std::vector<Mesh> meshChunks(const Grid& g);
// After an edit of the voxels in [lo, hi] (inclusive, lo <= hi): remeshes, in parallel, the chunks of `meshes`
// (from meshChunks) that overlap it grown by one voxel (faces and AO look at neighbours), and returns
// their indices.
std::vector<size_t> remeshChunks(const Grid& g, glm::ivec3 lo, glm::ivec3 hi, std::vector<Mesh>& meshes);
