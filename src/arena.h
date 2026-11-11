#pragma once
#include <string>
#include <vector>

#include "prefab.h"
#include "story.h"

// The arena (worlds/arena.flyw, ADR-054): a walled sand ring for trying out fights, built by code so
// it can be rebuilt (`flycoos --build-arena PATH`). Its centre is the world's middle (worldMiddle),
// so the debug window's "x, z from the middle" (0, 0) is the arena's centre. Around the open middle:
// the Hill (north, +z: a ramp, stairs and a cliff), the Pillars (east, +x: columns, a wall, a kerb,
// crates), the Pool (west: shallow and deep water under a plank bridge), the Rough (north-east:
// uneven ground), the Narrows (north-west: an L-shaped corridor between walls), archery targets (south-east), and
// the player's spawn by a brazier and three training dummies (south). Braziers on the diagonals are
// rest points. Each zone has a named point (poi marker); the stands and the outer wall close it in.
constexpr int kArenaFloor = 40;  // the floor's top voxel (sand inside, grass outside)
constexpr int kArenaRadius = 56; // the floor is the disc inside, the wall's inner face around it

// Builds the arena into `g` (a fixed grid at lo 0, at least 160 wide and 64 high around its
// middle; past that, a grass plain): its voxels, its prefab instances (from `lib`: arena_pillar, brazier, banner,
// arena_gate, archery_target, crate_stack) and its markers (ids from 1), replacing what they held.
// Returns "" or why it failed (a prefab missing or rejected).
std::string buildArena(Grid& g, const PrefabLibrary& lib, std::vector<Instance>& instances, std::vector<Marker>& markers);
