#pragma once
#include <cstdint>
#include <vector>

#include "world.h"

// Grass and flowers (ADR-030): plant blocks (isPlant) stand in the cell above the ground and are drawn
// as small voxel models at 1/10 scale, like the characters. The block says what grows there; which
// model, its turn and a small offset come from a hash of the cell, so a field doesn't repeat.
enum PlantModel : uint8_t { ShortTuft0, ShortTuft1, ShortTuft2, TallTuft0, TallTuft1, TallTuft2,
                            Daisies, Buttercups, Poppies, Bellflowers, PlantModelCount };
constexpr int kPlantVoxels = 10;  // a plant model's grid spans one block
constexpr float kPlantNear = 24;  // blocks from the eye: nearer plants get the detailed model

struct Quad;
// The far model has half the blades, all straight: about half the quads where a blade is a few pixels.
Grid buildPlant(PlantModel m, bool far = false);
std::vector<Quad> plantQuads(PlantModel m, bool far);  // meshed, without the faces on the ground
uint32_t plantHash(glm::ivec3 cell);  // the shader's plantHash (shaders/world.metal) must match
PlantModel plantModel(Block plant, glm::ivec3 cell);

// What grows at column (x, z) of a mixed meadow (Air: nothing). Soft noise fields make lush
// stretches and bare gaps, knee-high patches of TallGrass and clusters of one flower kind; the rest
// is ShortGrass. `kind`: ShortGrass for that mix, or any other plant for only that kind where the
// mix would grow something. `density` 1 is the generator's meadow; it scales every column's chance.
Block meadowPlant(int x, int z, Block kind, float density);
// Replants column (x, z): clears the plant on its ground (and under its trees), then puts meadowPlant there if the ground
// is Grass open to the sky with air above (kind Air: only clears).
void plantColumn(Grid& g, int x, int z, Block kind, float density);
