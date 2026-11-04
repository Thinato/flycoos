#pragma once
// Trees (docs/ART.md "Trees", ADR-029): a species plans a shape (bark capsules for the trunk,
// limbs and roots, leaf clumps for the crown), then the shape is drawn into the grid. Planning is
// separate so prefab boxes and tests read the same shape the grower draws.
#include <functional>
#include <vector>

#include "edit.h"

enum class Species : uint8_t { Oak, Maple, Birch, Conifer, Araucaria, Count };
// Full-grown heights (the crown's top above the ground), and each species' prefab.
inline constexpr int kFullGrown[int(Species::Count)][2] = {{20, 28}, {20, 28}, {18, 24}, {26, 38}, {32, 44}};
inline constexpr const char* kSpeciesPrefab[int(Species::Count)] = {"tree", "maple", "birch", "pine", "araucaria"};

struct TreeShape {
    struct Branch { glm::vec3 a, b; float r0, r1; };  // a capsule from a (radius r0) to b (r1)
    struct Clump { glm::vec3 c, radii; };             // an ellipsoid of leaves, its surface pushed by smooth noise
    Block bark = Wood;
    Leaf leaf = Leaf::Oak;
    std::vector<Branch> wood;  // trunk first
    int limbs = 0;             // how many limbs leave the trunk (tests)
    std::vector<Clump> leaves;
    float crownLo = 0, crownHi = 1;  // leaf shade 0 at crownLo up to 3 at crownHi (local y): the clumps' span
};

// Local coordinates: (0, 0, 0) is the ground voxel the tree stands on; the trunk starts above it.
// `height` is the top of the crown, `crown` its radius (0: the species' default for the height),
// `colour` picks a maple's leaves (0 red, 1 orange, 2 gold). Deterministic in its arguments.
TreeShape planTree(Species s, int height, float crown, int colour, uint32_t seed);
// A bush: one low clump of oak leaves, plus side clumps from radius 2 up.
TreeShape planBush(float radius, uint32_t seed);
// Every voxel growShape may change, for a tree standing on `at`.
Box shapeBox(const TreeShape& t, glm::ivec3 at);
// Bark fills Air and Water, leaves fill Air only; leaf shades step with the height in the crown.
void growShape(Grid& g, glm::ivec3 at, const TreeShape& t, uint32_t seed);

// A tree the world generator plants: standing on ground voxel `at`, grown by planTree(species,
// height, 0, colour, seed).
struct TreeSite {
    glm::ivec3 at;
    Species species;
    int height, colour;
    uint32_t seed;
};
// The generator's trees for world `seed` whose trunks stand in columns [lo, hi] (x, z), in order;
// each is chosen on the grid as `plant` left it (world.cpp). generateTerrain grows them; replant
// (prefab.h) places them as instances.
void plantTrees(Grid& g, uint32_t seed, glm::ivec2 lo, glm::ivec2 hi, const std::function<void(const TreeSite&)>& plant);
