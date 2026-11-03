#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "model.h"
#include "player.h"
#include "story.h"

// How an NPC treats the player. Harmless ones never fight back.
enum class Temper { Harmless, Passive, Aggressive };  // passive: fights back once hit; aggressive: attacks on sight
inline constexpr const char* kTemperNames[] = {"harmless", "passive", "aggressive"};  // their ids in data/creatures.json

// Fixed traits per kind, from data/creatures.json (ADR-018).
struct EntityType {
    const char* name;  // null: a random villager name
    int level;
    Stats stats;       // health and everything else derive from these
    bool attackable;   // can take damage
    bool wanders;      // walks around its home spot
    bool talks;        // E shows a speech balloon
    Model model;
    Temper temper;
    float aggroRange;     // aggressive: attacks a player this close (blocks)
    float wanderRadius;   // stays this close to home while not fighting
    float chaseSpeed;     // blocks/s while chasing
    ItemId weapon;        // held in the main hand
    ItemId loot;          // can drop when killed
    float lootChance;
    float respawnSeconds; // dead this long, then back at home fully healed
};
// From the loaded defs (defs.h); `kind` must be one of theirs (checkMarker refuses others).
const EntityType& typeOf(EntityKind kind);

struct Entity {
    EntityKind kind;
    std::string name;
    int level = 1;
    Player body;           // position, animation, health
    glm::vec3 home{};      // wanderers stay near this; respawns here
    float wanderRadius = 0;  // how far from home it wanders: its kind's, or its spawn marker's radius
    Marker spawn;          // the spawn marker it came from as it was then (id 0: none); see syncSpawns
    glm::vec2 wanderDir{}; // zero = standing still
    float wanderTimer = 0; // seconds until the next wander decision
    int phrase = -1;       // index into the phrase list, -1 = silent
    float speechTimer = 0; // seconds the balloon stays up
    float shoutTimer = 0;  // > 0: shouted a line on turning angry lately: no new shout, and the balloon doesn't hold it still
    bool angry = false;    // chasing and attacking the player
    ItemId loot = NoItem;  // what its corpse holds
    std::mt19937 rng;

    const EntityType& type() const { return typeOf(kind); }
    const char* speech() const;  // current balloon text, null when silent
    // Its name on the nameplate: the spawn marker's name for a lone NPC with lines, else `name`.
    const std::string& displayName() const;
};

constexpr float kTalkRange = 3.5f;   // blocks between feet; also how close you loot from
constexpr float kPunchReach = 2.0f;  // blocks between feet
constexpr float kLeashRadius = 20;   // fighters give up once the player is this far from their home
constexpr float kPlayerRespawnSeconds = 5;

// The training dummy, a villager, a boar and a bandit on grass beside `near` (usually the player
// spawn). The bandit's camp is far enough away that it can't see `near`.
std::vector<Entity> spawnEntities(const Grid& world, glm::vec3 near, uint32_t seed);
// The NPCs of every spawn marker. Entities whose marker is gone or changed (position, radius, kind
// or count) are replaced by fresh ones at the marker; the rest are left alone (a new name or new
// lines only update their copy of the marker), so it's cheap to call after anything that may have
// changed the markers. New ones face `face`.
void syncSpawns(std::vector<Entity>& entities, const Grid& world, const std::vector<Marker>& markers, uint32_t seed,
                glm::vec3 face);
// Where the player starts and gets back up: the player_spawn marker, else findSpawn, out of the terrain.
glm::vec3 playerSpawn(const Grid& world, const std::vector<Marker>& markers);
// Spawn markers for the NPCs that worlds had before markers existed: one per spawnEntities entity
// around the world spawn (findSpawn), with its kind's wander radius. For new worlds and older files.
std::vector<Marker> defaultMarkers(const Grid& world, uint32_t seed);
// AI and physics for every entity: wandering, talking villagers stop and face the player, angry
// fighters chase the player and swing at them.
void updateEntities(std::vector<Entity>& entities, const Grid& world, const Player& player, float dt);
// Keeps the player from walking through entities (never pushes into terrain).
void separate(Player& player, const Grid& world, const std::vector<Entity>& entities);
// Nearest living entity within talk range that can talk, or null.
Entity* talkTarget(std::vector<Entity>& entities, const Player& player);
// Says the next of its spawn marker's lines (cycling), or else a new random phrase (different from the last one).
void talk(Entity& e);
// Nearest corpse within talk range that still holds loot, or null (also when the player is
// knocked out or their bags are full).
Entity* lootTarget(std::vector<Entity>& entities, const Player& player);
// Moves the corpse's loot into the player's bags; false if they're full.
bool takeLoot(Entity& e, Inventory& inv);

// Takes damage if alive; health stops at 0. Puts the body in combat. Returns whether it applied.
bool hurt(Player& body, int amount);
// hurt() for attackable entities only. Angers fighters; a kill rolls the loot.
bool damage(Entity& e, int amount);

// Starts an attack (main-hand weapon, or a punch) toward `aim` (XZ direction) if the body is alive and its swing is ready.
bool startPunch(Player& attacker, glm::vec2 aim);
// Whether a swing from `attacker` reaches feet position `target`: in reach, within 60 degrees of facing, about level.
bool inReach(const Player& attacker, glm::vec3 target);
// What a punch from `attacker` hits: the nearest living, attackable entity in reach.
Entity* punchTarget(std::vector<Entity>& entities, const Player& attacker);
// Combat timers and out-of-combat regeneration for everyone; lands every swing at its impact frame
// (the player's on entities, entities' on the player), less the victim's armor (`armorK` is the
// K of armorReduction); brings the dead back home.
void updateCombat(Player& player, std::vector<Entity>& entities, float dt, std::mt19937& rng, float armorK = kArmorK);
