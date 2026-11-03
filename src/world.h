#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

// Block ids double as palette indices (see kPalette in renderer.cpp).
// Water's id is also hard-coded as kWater in shaders/world.metal.
enum Block : uint8_t { Air, Grass, Dirt, Stone, Sand, Snow, Wood, Leaves, Water,
                       Skin, Tunic, Belt, Boots, Hair, Eye, Rock,
                       Robe, DarkHair, Straw, Burlap,  // NPC models
                       Steel, Iron,                    // items
                       Hide, Tusk, Snout, Hood, Leather,  // boar, bandit
                       Plate, BlockCount };               // armor

inline bool isOpaque(Block b) { return b != Air && b != Water; }

constexpr int kChunk = 32;
constexpr glm::ivec3 kWorldSize{1024, 192, 1024};  // ponytail: dense grid (192 MiB); chunk streaming in M1
constexpr int kSeaLevel = 34;
static_assert(kChunk <= 32, "quad positions are 6-bit; see mesher.h");
static_assert(kWorldSize.x % kChunk == 0 && kWorldSize.y % kChunk == 0 && kWorldSize.z % kChunk == 0);

// Dense voxel volume, x fastest. Reads outside the volume return Air.
struct Grid {
    glm::ivec3 size{};
    std::vector<Block> cells;

    explicit Grid(glm::ivec3 s) : size(s), cells(size_t(s.x) * s.y * s.z, Air) {}
    bool inside(glm::ivec3 p) const {
        return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < size.x && p.y < size.y && p.z < size.z;
    }
    size_t index(glm::ivec3 p) const { return (size_t(p.y) * size.z + p.z) * size.x + p.x; }
    Block get(glm::ivec3 p) const { return inside(p) ? cells[index(p)] : Air; }
    void set(glm::ivec3 p, Block b) { if (inside(p)) cells[index(p)] = b; }
};

// Runs fn(0..count-1) across all hardware threads.
void parallelFor(int count, const std::function<void(int)>& fn);

void generateTerrain(Grid& world, uint32_t seed);

// Feet position on the grass column nearest `near` (default: the world center).
glm::vec3 findSpawn(const Grid& world);
glm::vec3 findSpawn(const Grid& world, glm::ivec2 near);
