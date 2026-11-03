#pragma once
#include <array>
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
    // The AI's numbers (ADR-020); fighters only.
    float leashRadius;     // a chase ends once the player is this far from home
    float reaggroRadius;   // aggressive: attacks on sight only a player this close to home (< leashRadius)
    float reaggroCooldown; // seconds after getting home before it attacks on sight again
    float giveUpSeconds;   // can't step toward the player (out of reach) this long: goes home
    float windup;          // seconds from starting a swing to its hit, standing committed
    float recover;         // seconds it stands after the hit
    float cooldown;        // seconds of chasing before the next swing (the weapon's swing time gates it too)
    float fleeHealth;      // runs away below this fraction of its health; 0: never
};
// From the loaded defs (defs.h); `kind` must be one of theirs (checkMarker refuses others).
const EntityType& typeOf(EntityKind kind);

// The NPC AI's states (ADR-020). Idle: stands where it was put (kinds that don't wander). Talk:
// stands facing the player while its balloon is up. Chase, Windup, Recover: fighting. Return: walks
// home ignoring the player, then heals. Flee: runs from the player for a while, then returns.
enum class AiState : uint8_t { Idle, Wander, Talk, Chase, Windup, Recover, Return, Flee, Dead, Count };
inline constexpr const char* kAiStateNames[] = {"idle", "wander", "talk", "chase", "windup", "recover", "return", "flee", "dead"};
static_assert(std::size(kAiStateNames) == size_t(AiState::Count));
// What happened to an entity since its last AI update (bits of Entity::events).
enum AiEvent : uint8_t { EventDamaged = 1 };
struct AiTransition {
    AiState from = AiState::Idle, to = AiState::Idle;
    const char* reason = "";  // a static string, e.g. "target_left_leash"
    float after = 0;          // seconds it had spent in `from`
};

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
    AiState state = AiState::Idle;  // change it with setState
    float stateTime = 0;   // seconds in `state`
    float lostTime = 0;    // seconds it couldn't step toward where it's going (chasing or going home)
    float calmTimer = 0;   // > 0: back home lately; doesn't attack on sight yet
    uint8_t events = 0;    // AiEvent bits, read and cleared by its next AI update
    std::array<AiTransition, 8> history{};  // the latest transitions: history[(transitions - 1) % 8] is the newest
    uint32_t transitions = 0;               // how many it has made
    ItemId loot = NoItem;  // what its corpse holds
    std::mt19937 rng;

    const EntityType& type() const { return typeOf(kind); }
    bool angry() const { return state == AiState::Chase || state == AiState::Windup || state == AiState::Recover; }
    const char* speech() const;  // current balloon text, null when silent
    // Its name on the nameplate: the spawn marker's name for a lone NPC with lines, else `name`.
    const std::string& displayName() const;
};

constexpr float kTalkRange = 3.5f;   // blocks between feet; also how close you loot from
constexpr float kPunchReach = 2.0f;  // blocks between feet
constexpr float kFleeSeconds = 4;    // how long a fleeing NPC runs before it heads home
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
// Moves `e` to `to`, recording `reason`, with the exit and enter actions: leaving Windup early
// cancels its hit; entering Chase from a calm state shouts the spawn marker's next line (at most every 8 s).
void setState(Entity& e, AiState to, const char* reason);
// AI and physics for every entity: the state machine of ADR-020 (wandering, talking villagers stop
// and face the player, fighters chase the player, wind up and swing, give up and walk home).
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
// hurt() for attackable entities only. Tells fighters (EventDamaged); a kill rolls the loot.
bool damage(Entity& e, int amount);

// Starts an attack (main-hand weapon, or a punch) toward `aim` (XZ direction) if the body is alive and
// its swing is ready. It hits `windup` seconds later (sooner if the swing is quicker than its animation).
bool startPunch(Player& attacker, glm::vec2 aim, float windup = kSwingWindup);
// Whether a swing from `attacker` reaches feet position `target`: in reach, within 60 degrees of facing, about level.
bool inReach(const Player& attacker, glm::vec3 target);
// What a punch from `attacker` hits: the nearest living, attackable entity in reach.
Entity* punchTarget(std::vector<Entity>& entities, const Player& attacker);
// Combat timers and out-of-combat regeneration for everyone; lands every swing at its impact frame
// (the player's on entities, entities' on the player), less the victim's armor (`armorK` is the
// K of armorReduction); brings the dead back home.
void updateCombat(Player& player, std::vector<Entity>& entities, float dt, std::mt19937& rng, float armorK = kArmorK);
