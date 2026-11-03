#pragma once
#include <optional>
#include <string>
#include <vector>

#include "world.h"

// Story data: NPC kinds and the markers that place them and the story's places, and their world
// file sections (MARK, LINE, OWNR).

// NPC kinds: the `save_id` of their record in data/creatures.json, saved in spawn markers: append-only,
// never renumbered. These are the ones code names, with their string ids; data may add more after Count.
enum class EntityKind : uint8_t { TrainingDummy, Villager, Boar, Bandit, Wolf, Guard, BanditChief, Ghoul, Count };
inline constexpr const char* kEntityKindNames[] = {"dummy", "villager", "boar", "bandit", "wolf", "guard", "bandit_chief", "ghoul"};

// Story markers: authored places in the world (see docs/ARCHITECTURE.md, "Markers"). Saved in world
// files: only append kinds.
enum class MarkerKind : uint8_t {
    PlayerSpawn,  // where the player starts and gets back up (at most one; none: findSpawn)
    Spawn,        // `count` NPCs of kind `entity`; they wander within `radius` and respawn here
    Poi,          // a named point; its label shows while the player is within `radius`
    Region,       // a named circle (x, z, radius); entering it shows its name
    Count
};
inline constexpr const char* kMarkerKindNames[] = {"player_spawn", "spawn", "poi", "region"};

struct Marker {
    uint32_t id = 0;
    MarkerKind kind = MarkerKind::Poi;
    std::string name;  // UTF-8, at most kMaxMarkerName bytes, no control, bidi or zero-width characters
    glm::vec3 pos{};   // feet position: y is the top of the ground voxel it stands on
    float radius = 0;
    EntityKind entity = EntityKind::Villager;  // spawn
    int count = 1;                             // spawn
    // Spawn (not the dummy): what its NPCs say, in order and round again: talkers one line per E
    // (none: random villager phrases), and any of them one when it turns on the player (at most every
    // 8 s). Each line is checked like a name.
    std::vector<std::string> lines;
    // The prefab instance whose placement made it (0: placed by hand). It moves with that instance
    // and goes when it's removed (prefab.h); putMarker keeps it as it is.
    uint32_t owner = 0;
    bool operator==(const Marker&) const = default;
};
// Whether a marker may carry lines: spawns of any kind but the dummy. Deliberately not tied to NPC
// traits, so saved worlds keep loading when a kind's traits change.
inline bool takesLines(const Marker& m) { return m.kind == MarkerKind::Spawn && m.entity != EntityKind::TrainingDummy; }

// Caps, enforced when adding, editing and loading.
constexpr size_t kMaxMarkers = 4096;
constexpr size_t kMaxMarkerName = 48;  // bytes
constexpr int kMaxSpawnCount = 8;
constexpr int kMaxSpawnedNpcs = 256;  // the counts of all spawn markers together: every NPC is simulated each frame
constexpr float kMaxMarkerRadius = 512;
constexpr size_t kMaxMarkerLines = 8;
constexpr size_t kMaxMarkerLine = 160;  // bytes

// Why the marker is invalid (ignoring its id and the other markers), or nullptr.
const char* checkMarker(const Grid& g, const Marker& m);
// The smallest region containing `p` (x, z), or nullptr: the innermost one when they nest.
const Marker* regionAt(const std::vector<Marker>& markers, glm::vec3 p);

// World files (.flyw): the whole grid, its seed, the prefab instances and the markers (both sorted
// by id), in one versioned file; see docs/ARCHITECTURE.md. Both return an empty string on success,
// else why it failed. A file without a marker section (older ones) loads `markers` as nullopt. A
// rejected load leaves `g`, `instances` and `markers` unspecified. `markerIds` is the highest marker
// id ever used (EditHistory::markerIds); a file without one loads it as 0 (the markers' own ids count).
std::string saveWorld(const char* path, const Grid& g, uint32_t seed, const std::vector<Instance>& instances,
                      const std::vector<Marker>& markers, uint32_t markerIds = 0);
std::string loadWorld(const char* path, Grid& g, uint32_t& seed, std::vector<Instance>& instances,
                      std::optional<std::vector<Marker>>& markers, uint32_t* markerIds = nullptr);
