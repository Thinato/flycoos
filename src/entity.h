#pragma once
#include <functional>
#include <map>
#include <optional>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "model.h"
#include "player.h"
#include "quest.h"
#include "story.h"

// How an NPC treats the player. Harmless ones never fight back.
enum class Temper { Harmless, Passive, Aggressive };  // passive: fights back once hit; aggressive: attacks on sight
inline constexpr const char* kTemperNames[] = {"harmless", "passive", "aggressive"};  // their ids in data/creatures.json

// Fixed traits per kind, from data/creatures.json (ADR-018).
// What a hit on a creature throws out (ADR-058, fx.h): red drops, wood splinters, grave dust and bone chips.
enum class Blood : uint8_t { Red, Splinters, Dust };

struct EntityType {
    const char* name;  // null: a random villager name
    int level;
    Stats stats;       // health and everything else derive from these
    bool attackable;   // can be hit
    bool immortal;     // hits show their numbers but take no health (the training dummy, ADR-056)
    bool wanders;      // walks around its home spot
    bool talks;        // E shows a speech balloon
    Model model;
    Temper temper;
    float aggroRange;     // aggressive: attacks a player this close (blocks) that it can see (ADR-055)
    float wanderRadius;   // stays this close to home while not fighting
    float chaseSpeed;     // blocks/s while chasing, or running away (a villager, ADR-056)
    ItemId weapon;        // held in the main hand
    ItemId loot;          // can drop when killed
    float lootChance;
    ItemId rareLoot;      // dropped instead when `loot` isn't (ADR-049: a great weapon)
    float rareChance;
    // The AI's numbers (ADR-020); fighters only.
    float chaseRange;      // a chase ends once the player is this far from it, seen or not (> aggroRange; ADR-055)
    float reaggroCooldown; // seconds after getting home before it attacks on sight again
    float giveUpSeconds;   // can't step toward the player (out of reach) this long: goes home
    float windup;          // seconds from starting a swing to its hit, standing committed
    float recover;         // seconds it stands after the hit
    float cooldown;        // seconds of chasing before the next swing (the weapon's swing time gates it too); an archer's: from one shot's start to the next (and between its knife's swings)
    float fleeHealth;      // runs away below this fraction of its health; 0: never
    float stance;          // stance damage it takes to break its stance (ADR-035); 0: never breaks
    float poise;           // a hit with at least this stance damage makes it flinch (cancels its attack)
    float hitStance;       // the stance damage of its hits: they make the player flinch if at least its poise (playerPoise)
    bool parryable;        // a parry deflects its attacks (ADR-036)
    int parries;           // parries it takes to open it to a riposte (the ones before only make it recoil)
    int xp;                // character XP the player gets for killing one (ADR-027)
    // Its sounds (ADR-037): "<voice>_windup", "_attack", "_hurt", "_death" (empty: it has no voice),
    // played at voicePitch; hits on a wooden one sound as on wood, not flesh.
    std::string voice;
    float voicePitch;
    bool wooden;
    Blood blood;        // what a hit on it throws out (ADR-058): blood, splinters (the dummy) or grave dust (ghouls)
    DamageType damage;  // what its attacks do without a weapon (ADR-040): teeth pierce, claws slash, tusks and fists strike
    PerType resist;     // the share of each damage type it shrugs off (negative: it's weak to it)
    // Its tactics (ADR-042, ADR-043): the longest string of hits it swings (1: single hits), the chance a
    // hit after the first is delayed, and the chances it meets a player's attack with a guard, a parry,
    // a dodge, or backs off after its own. A shield in its off hand guards (and parries) as the player's.
    int combo;
    float delay;
    float guard, parry, dodge, backOff;
    ItemId shield;
    std::vector<Modifier> mods;  // its own modifiers on top of its stats (ADR-046): a bite's bleed, a status resist
    // An archer (ADR-051): a ranged weapon, the range band it keeps from the player (blocks between feet), how far
    // off its shots go (radians, either way), and what it fights with up close (a main-hand weapon; none: the bow).
    float keepMin = 0, keepMax = 0;
    float aimError = 0;
    ItemId sidearm;
};
// From the loaded defs (defs.h); `kind` must be one of theirs (checkMarker refuses others).
const EntityType& typeOf(EntityKind kind);
// A bystander (a villager): harmless and walking about. It can be hit (ADR-056), but only on purpose: no lock-on,
// and no backstab or riposte turns an attack onto it.
inline bool bystander(const EntityType& t) { return t.temper == Temper::Harmless && t.wanders; }

// The NPC AI's states (ADR-020). Idle: stands where it was put (kinds that don't wander). Talk:
// stands facing the player while its balloon is up. Chase, Windup, Recover: fighting. Return: walks
// home ignoring the player, then heals. Flee: runs from the player for a while, then returns.
// Stagger: reels from a hit (a flinch, or a stance break), then fights on (ADR-035). Search: lost sight
// of the player mid-chase, goes to where it last saw them and looks around, then gives up (ADR-055).
enum class AiState : uint8_t { Idle, Wander, Talk, Chase, Windup, Recover, Return, Flee, Dead, Stagger, Search, Count };
inline constexpr const char* kAiStateNames[] = {"idle", "wander", "talk", "chase", "windup", "recover", "return", "flee", "dead", "stagger", "search"};
static_assert(std::size(kAiStateNames) == size_t(AiState::Count));
// What happened to an entity since its last AI update (bits of Entity::events).
enum AiEvent : uint8_t { EventDamaged = 1 };
struct AiTransition {
    AiState from = AiState::Idle, to = AiState::Idle;
    const char* reason = "";  // a static string, e.g. "lost_sight"
    float after = 0;          // seconds it had spent in `from`
};

// What an NPC means to do about the player's attack under way (ADR-043), chosen as the attack starts.
enum class Answer : uint8_t { None, Guard, Parry, Dodge, Count };
inline constexpr const char* kAnswerNames[] = {"none", "guard", "parry", "dodge"};
static_assert(std::size(kAnswerNames) == size_t(Answer::Count));

// The fighting AI's knobs (ADR-042, ADR-043), live from the debug window's Entities tab.
struct AiTuning {
    int attackers = 2;          // fighters on the player that may swing at once (attack tokens); the rest wait
    float waitRing = 3.6f;      // blocks from the player those that wait keep, circling
    float strafeSpeed = 0.5f;   // x walking speed while they circle
    float delaySeconds = 0.5f;  // a delayed hit in a string holds its windup this much longer (x 0.8 .. 1.2)
    float reactFrom = 0.06f, reactTo = 0.18f;  // seconds before an NPC acts on an attack it sees coming
    float aimError = 0.07f;     // +- seconds off it times a parry or a dodge by
    float dodgeLead = 0.12f;    // a dodge starts this long before the hit is due
    float guardHold = 0.4f;     // seconds the guard stays up after the attack has passed
    float parriedSeconds = 1.0f;  // a player whose attack an NPC parries reels this long
    float counterWindup = 0.3f;   // an NPC's swing right after it blocked or parried winds up this fast...
    float counterWindow = 1.0f;   // ...if it starts within this long
    float backOffSeconds = 0.9f;  // an NPC backing off walks away facing the player this long
    float drawHold = 0.8f;      // an archer holds its draw this long before it looses (x 0.85 .. 1.15): the tell (ADR-051)
    float closeIn = 3;          // an archer with a sidearm takes it out once the player is this close (the bow back past keepMin)
    float sightGrace = 0.5f;    // a chase goes on this long out of sight (round a corner) before it turns into a search (ADR-055)
    float searchSeconds = 8;    // a search lasts this long (going to where it last saw the player, looking round) before it gives up
    float corpseGone = 64;      // a corpse with nothing on it goes once the player is this far (blocks) and can't see it (ADR-056)
};
inline AiTuning gAi;

struct Entity {
    uint32_t id = 0;       // unique in the run (makeEntity counts them): what a lock-on holds
    EntityKind kind;
    std::string name;
    Player body;           // position, animation, health, level
    glm::vec3 home{};      // wanderers stay near this, and go back to it
    float wanderRadius = 0;  // how far from home it wanders: its kind's, or its spawn marker's radius
    Marker spawn;          // the spawn marker it came from as it was then (id 0: none); see syncSpawns
    int slot = 0;          // which of that marker's NPCs it is (0..count-1): player saves key NPCs by spawn.id and slot
    glm::vec2 wanderDir{}; // zero = standing still
    float wanderTimer = 0; // seconds until the next wander decision
    int phrase = -1;       // index into the phrase list, -1 = silent
    int saying = -1;       // the quest line (defs().questLines) its balloon shows, -1: `phrase`'s
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
    float side = 0;        // fighting the player with others: the angle around the player it closes in from (atan2(x, z))
    int pack = 0;          // how many are fighting the player together (it included); 0: not fighting
    float xpWear = 0;      // the player's hits on it that earned XP lately: each one earns less (awardHitXp)
    float armorWear = 0;   // its hits on the player that earned armor XP lately
    int parried = 0;       // the player's parries on it toward its kind's `parries` (they don't wear off)
    bool token = false;    // may swing at the player now (ADR-042): else it waits on the ring
    float sinceSwing = 99; // seconds since it last started a string of hits: who waited longest swings next
    int string = 0;        // hits left in the string it's swinging (ADR-042)
    float strafe = 1;      // which way round it circles while it waits (+1, -1), and...
    float strafeTimer = 0; // ...seconds until it may turn round
    bool threatened = false;  // the player's attack under way can hit it (ADR-043)
    bool picked = false;   // it has picked its answer to that attack
    Answer answer = Answer::None;  // what it means to do about that attack...
    float react = 0;       // ...in this many seconds (its reaction time)...
    float aimOff = 0;      // ...this far off the right moment (parries and dodges)
    float guardTimer = 0;  // > 0: holds its guard up
    float counterTimer = 0;  // > 0: it just blocked or parried: its next swing comes fast
    float backOff = 0;     // > 0: backing off after its string
    bool sight = false;    // an archer: it saw the player's chest from its bow at its last look (ADR-051)
    glm::vec3 lastSeen{};  // where it last saw the player (or was hit from), fighting or searching (ADR-055)
    float unseen = 0;      // seconds it hasn't seen the player, fighting or searching
    std::mt19937 rng;

    const EntityType& type() const { return typeOf(kind); }
    bool angry() const {
        return state == AiState::Chase || state == AiState::Windup || state == AiState::Recover || state == AiState::Stagger;
    }
    bool hunting() const { return angry() || state == AiState::Search; }  // fighting, or searching for the player (ADR-055)
    const char* speech() const;  // current balloon text, null when silent
    // Its name on the nameplate: the spawn marker's name for a lone NPC with lines, else `name`.
    const std::string& displayName() const;
};

constexpr float kDefaultWindup = 0.5f;   // seconds from an NPC's swing to its hit (attack_windup)
constexpr float kDefaultRecover = 0.5f;  // seconds it stands after (attack_recover): the opening to hit back
constexpr float kTalkRange = 3.5f;   // blocks between feet; also how close you loot from
constexpr float kPunchReach = 2.0f;  // blocks between feet
constexpr float kFleeSeconds = 4;    // how long a fleeing NPC runs before it heads home
constexpr float kPlayerRespawnSeconds = 5;
// Who died for good (ADR-056): spawn marker id -> a bit per slot whose NPC is dead. Nothing comes back; the player
// save keeps it (DEAD), so a slot stays empty after its corpse has gone.
using DeadSlots = std::map<uint32_t, uint32_t>;
constexpr float kCorpseLinger = 3;  // seconds an empty corpse of an inactive spawn marker (ADR-023) lies before it goes

// The training dummy, a villager, a boar, a bandit and a bandit archer on grass beside `near` (usually the player
// spawn). The bandit's camp is far enough away that they can't see `near`.
std::vector<Entity> spawnEntities(const Grid& world, glm::vec3 near, uint32_t seed);
// A fresh `kind` `distance` blocks in front of `player` (the way its body faces), lifted out of the
// terrain and facing it; for trying out fights (tests; the debug window uses spawnGroup). Not from a marker (spawn.id 0):
// marker syncs and the player save leave it alone, and it doesn't come back with the game.
Entity& spawnAhead(std::vector<Entity>& entities, const Grid& world, const Player& player, EntityKind kind,
                   uint32_t seed, float distance = 5);
// `count` (at most kMaxSpawnedNpcs) fresh `kind`s around `at`: the first on its column, the others
// on the spiral spawn markers use, each in the standing place of its column nearest at.y (within 4
// blocks: on the ground under a roof or a crown, not on it), else on the column's highest opaque
// voxel, facing `face`. Columns with no ground (the endless world's unloaded ones) are skipped, and a
// non-finite or far-out `at` spawns none. Not from a marker, like spawnAhead's. Returns how many.
int spawnGroup(std::vector<Entity>& entities, const Grid& world, EntityKind kind, int count, glm::vec3 at,
               glm::vec3 face, uint32_t seed);
// The NPCs of every spawn marker. Entities whose marker is gone or changed (position, radius, kind
// or count) are replaced by fresh ones at the marker; the rest are left alone (a new name or new
// lines only update their copy of the marker), so it's cheap to call after anything that may have
// changed the markers. Entities not from a marker (spawnAhead's) are left alone. New ones face `face`. A marker with an active window (Marker::activeFrom)
// spawns only inside it at `timeOfDay` (then only the slots it misses: those still out keep their
// state); outside it its NPCs leave once out of a fight (Chase, Windup,
// Recover), its corpses once looted (kCorpseLinger after death at the earliest) or back up, and the
// rest of its NPCs stay away. Slots in `dead` are never spawned again, stale marker or not, and a changed marker's
// corpse that holds loot stays where it lies (ADR-056: a quest's item mustn't vanish with an edit).
void syncSpawns(std::vector<Entity>& entities, const Grid& world, const std::vector<Marker>& markers, uint32_t seed,
                glm::vec3 face, float timeOfDay, const DeadSlots& dead = {});
// Lasting deaths (ADR-056), once a tick: every dead marker NPC goes into `dead`, and a corpse with no loot left
// goes once `player` is gAi.corpseGone away and can't see it (or its part of the world isn't `loaded`). One with
// loot stays until it's taken, so a quest's item can't vanish with it.
void buryDead(std::vector<Entity>& entities, DeadSlots& dead, const Grid& world, const Player& player,
              const std::function<bool(glm::vec3)>& loaded = {});
// Where the player starts and gets back up: the player_spawn marker, else findSpawn, out of the terrain.
glm::vec3 playerSpawn(const Grid& world, const std::vector<Marker>& markers);
// Spawn markers for the NPCs that worlds had before markers existed: one per spawnEntities entity
// around the world spawn (findSpawn), with its kind's wander radius. For new worlds and older files.
std::vector<Marker> defaultMarkers(const Grid& world, uint32_t seed);
// Moves `e` to `to`, recording `reason`, with the exit and enter actions: leaving Windup early
// cancels its hit; entering Chase from a calm state shouts the spawn marker's next line (at most every 8 s).
void setState(Entity& e, AiState to, const char* reason);
// AI and physics for every entity: the state machine of ADR-020 (wandering, talking villagers stop
// and face the player, fighters chase the player, wind up and swing, give up and walk home). Fighters
// on the player together spread around it (ADR-034): each gets a side to come in from, evenly spaced,
// and circles to it before closing in. Living NPCs push apart so they never stand inside each other.
// NPCs where `loaded` says no (the endless world's part that isn't in) wait where they are.
void updateEntities(std::vector<Entity>& entities, const Grid& world, const Player& player, float dt,
                    const std::function<bool(glm::vec3)>& loaded = {});
// Whether nothing solid stands between `from`'s eyes (1.5 above its feet) and `target`'s chest (1.1): how an NPC
// sees the player (ADR-055). ponytail: one ray, no field of view; a sliver of cover hides the whole body.
bool canSee(const Grid& world, const Player& from, const Player& target);
// Keeps the player from walking through entities (never pushes into terrain).
void separate(Player& player, const Grid& world, const std::vector<Entity>& entities);
// Nearest living entity within talk range that can talk, or null.
Entity* talkTarget(std::vector<Entity>& entities, const Player& player);
// Says `questLine` (an index into defs().questLines, pickLine's) unless it said that one last time and
// its marker has lines of its own; else the next of its marker's lines (cycling), or else a new random
// phrase (different from the last one). So quest lines and the marker's lines take turns.
void talk(Entity& e, int questLine = -1);
// Nearest corpse within talk range that still holds loot the player can stow (canStow), or null
// (also when the player is knocked out).
Entity* lootTarget(std::vector<Entity>& entities, const Player& player);
// Moves the corpse's loot into the player's bags, else its empty equip slot (stowItem); false if neither.
bool takeLoot(Entity& e, Inventory& inv);

// Takes damage if alive; health stops at 0. Puts the body in combat and shows a damage number.
// Returns whether it applied. `crit`: a critical hit (ADR-045); `kind`: a status's damage (ADR-046),
// not a hit's (Count): it shows its own number and leaves lastDamage alone. Poison's ticks aren't hits:
// they leave hitTime alone (and damage() doesn't call a fighter back for them). A death ends its status
// build-up and timed effects.
bool hurt(Player& body, int amount, bool crit = false, Status kind = Status::Count);
// hurt() for attackable entities only, an immortal one keeping its health. Tells it (EventDamaged: a fighter fights
// back, a harmless wanderer flees); a kill rolls the loot.
bool damage(Entity& e, int amount, bool crit = false, Status kind = Status::Count);

// The status effects' knobs (ADR-046), live from the debug window's Player tab. Shares are of the
// afflicted body's max health; status damage skips armor and damage taken.
struct StatusTuning {
    float decay = 8;                                  // build-up a body loses per second
    float bleedShare = 0.1f, bleedFlat = 5;           // a bleed: a burst of this share, plus this
    float poisonSeconds = 10;                         // poison: this long...
    float poisonShare = 0.005f, poisonFlat = 0.5f;    // ...this much a second (rounded)
    float frostShare = 0.1f, frostFlat = 3;           // frostbite: a burst...
    float frostSeconds = 15, frostTaken = 0.2f;       // ...then this much more damage taken (increased) this long
};
inline StatusTuning gStatus;
// Status `s` takes effect on `b` now, if alive (its build-up starts over): a bleed is a burst, poison a timed
// effect that ticks every second, frostbite a burst and a timed effect with more damage taken. One
// already on is replaced, fresh. Returns the burst's damage, for the caller to deal with its number.
int afflict(Player& b, Status s, std::string_view source);
// A landed hit's status build-up on `b`: the attacker's Bleed, Poison and Frost (with its swing's
// weapon skill), or with `own` a throwable's own instead (ADR-053: not the weapon in hand's), times `share` (what a guard let through), toward b's resists. One that fills takes
// effect (afflict); poison and frost don't build while their effect is on. Returns each status's burst
// damage due now, for the caller to deal.
std::array<int, size_t(Status::Count)> buildUp(const Combatant& attacker, Player& b, float share, std::string_view source,
                                               const std::array<float, size_t(Status::Count)>* own = nullptr);
// A flask's drink frame (ADR-047): a charge heals `b` its Flask healing share of max health; with none
// left, nothing (the gesture's Noise::Empty). updateCombat calls it when a Drink move reaches its impact.
void drink(Player& b);
// Rests `b` (ADR-047): its flasks filled to the Flasks stat, its quiver to kQuiver (ADR-050), its pouch (ADR-053), full health and
// stamina, statuses gone.
void rest(Player& b);

// An NPC's attack (main-hand weapon, or a punch): the next light of its chain toward `aim` (XZ
// direction), if the body is alive, not reeling or rolling, and its swing is ready (the weapon's
// swing time since the last). It holds its windup pose so the hit lands `windup` seconds in (no
// sooner than the move's own impact frame).
bool startPunch(Player& attacker, glm::vec2 aim, float windup = 0);
// Whether a swing from `attacker` reaches feet position `target`: in reach, within 60 degrees of facing, about level.
bool inReach(const Player& attacker, glm::vec3 target);
// What a punch from `attacker` hits: the nearest living, attackable entity in reach.
Entity* punchTarget(std::vector<Entity>& entities, const Player& attacker);
// Whether `attacker` stands behind `e`: within backstabAngle of its back (ADR-045).
bool behind(const Entity& e, const Player& attacker);
// Whether an attack from `attacker` on `e` is a critical (ADR-045): its stance is broken (a riposte),
// or `attacker` is behind it while it isn't attacking, rolling or reeling and wasn't hurt for
// backstabWait seconds (a backstab). Never a bystander (ADR-056).
bool opensToCritical(const Entity& e, const Player& attacker);
// The nearest living, attackable entity in reach open to a critical: an attack on it is the riposte
// move. `caught`: the move is already on its way, and the backstab it caught (reeling now, held) still
// counts; one that's rolling, down or was just hurt still doesn't.
Entity* criticalTarget(std::vector<Entity>& entities, const Player& attacker, bool caught = false);
// While the attacker's riposte move is on its way to a backstab, holds that NPC (it reels a moment, so
// it can't turn or act); call it every tick after updateMelee, before the NPCs think.
void holdCritical(std::vector<Entity>& entities, const Player& attacker);
// A landed hit's `stance` damage on `e` (ADR-035): at 0 its stance breaks (it reels breakSeconds, open
// to a riposte, and its stance is back); else a hit of at least its poise makes it flinch. A critical
// (a riposte or backstab) knocks it down instead.
void staggerHit(Entity& e, float stance, bool critical);
// What a hit is resolved in: the damage roll's RNG and the armor balance (the K of armorReduction).
struct HitContext {
    std::mt19937& rng;
    float armorK = kArmorK;
    float power = 1;  // x the hit before armor: the move's damage (a heavy, a charge, a riposte)
};
// The one damage formula (ADR-022), in this order: the attack's roll, plus the attacker's strength
// bonus, times the attacker's damage and the context's power, less the defender's armor against the attacker's level, times
// the defender's damage taken; at least 1, unless the hit before armor is 0 (nothing rolled, or a
// damage multiplier of 0): then 0. Reads both sheets; the attacker's with the conditional modifiers
// of its swing's weapon skill (skillOf(attacker.swingWeapon)).
int resolveHit(const Combatant& attacker, const Combatant& defender, const Attack& attack, const HitContext& ctx);
// Skill XP halves for every kXpWearHalving hits an NPC took from the player lately (and, apart,
// for every one it dealt the player, for armor XP); the wear wears off at one hit per
// kXpWearSeconds. The training dummy (harmless) teaches at kTrainingRate.
constexpr float kXpWearHalving = 20, kXpWearSeconds = 10, kTrainingRate = 0.25f;
// The only source of skill and character XP (ADR-027), for a hit that landed between the player and
// `npc`: the player's (`playerHit`; `killed`: it killed) earns the swing's weapon skill XP and the
// kill's character XP, the NPC's earns armor XP while the player wears armor. Only attackable NPCs
// count; the XP scales with the attack's average base damage (never the final hit), the NPC's wear
// and, for harmless ones, kTrainingRate. A kill gives the creature's `xp` times its wear, once per kill.
void awardHitXp(Combatant& player, Entity& npc, bool playerHit, bool killed);
// A hit by the player that reached `target` (not parried, not rolled through): a swing's at its impact
// frame, or an arrow's (ADR-050). How it goes is the same for both: a random critical (the attacker's
// CritChance, for CritDamage more; a riposte is always one), the target's resistance, resolveHit with
// the attacker's swingWeapon, its guard from `from` (part of the damage and its stamina; empty, it
// breaks), status build-up, the noises, XP (awardHitXp), Killed into `events`, staggerHit. A swing's
// (`melee`) also freezes both bodies (hit-stop), bounces off a greatshield's guard and shakes the camera.
struct Strike {
    float power = 1;        // x the hit before armor: the move's damage (a heavy, a charge, a riposte, a draw)
    float stance = 0;       // its stance damage
    glm::vec3 from{};       // where it comes from: a guard facing this way takes it
    DamageType type = DamageType::Slash;
    bool riposte = false;   // a riposte or backstab: always critical, past any guard, knocks it down
    bool canCrit = true;    // may be a random critical
    bool melee = true;      // a swing (an arrow: false)
    std::optional<std::array<float, size_t(Status::Count)>> buildup{};  // a throwable's own build-up, instead of the attacker's (ADR-053)
};
void strike(Player& player, Entity& target, const Strike& s, std::mt19937& rng, float armorK = kArmorK,
            std::vector<GameEvent>* events = nullptr);
// The other way round (ADR-051): a hit by `npc` that reached the player (not rolled through): a swing's at its
// impact frame, or its arrow's. A random critical, resolveHit with npc's swingWeapon times `s.power`, then a
// swing's parry (an arrow can't be parried: a parry is a weak guard), the guard from `s.from` (a fresh one
// is perfect), else a flinch from `s.stance` unless hyperarmor; statuses, armor XP, the noises and camera
// shake. A swing (`melee`) also freezes its attacker on the guard and bounces off a greatshield.
void strikePlayer(Entity& npc, Player& player, const Strike& s, std::mt19937& rng, float armorK = kArmorK);
// Combat timers, the moves' clocks, stance recovery, status build-up and timed effects (poison's ticks,
// a kill by them the player's) and out-of-combat regeneration for everyone; lands
// every swing at its impact frame (the player's on entities, entities' on the player) through
// resolveHit (`armorK` is the K of armorReduction) and awardHitXp (the player's through strike()). The dead stay dead (ADR-056).
// A bow's shot reaching its release sets the shooter's `loosed` instead (step() looses the arrow; an NPC's too, ADR-051). Any hit
// but the riposte move's may be a random critical (the attacker's CritChance, for CritDamage more). A kill
// by the player raises Killed into `events`. The player's moves hit with their damage and stance
// (staggerHit) and freeze both bodies a moment (hit-stop). An NPC's hit passes through a rolling
// player's i-frames, is blocked by its guard from the front (part of the damage, stamina; a guard
// with too little stamina breaks), and else makes it flinch unless it has hyperarmor (ADR-035).
// Big hits either way add camera trauma. Hits that land build up statuses (buildUp).
void updateCombat(Player& player, std::vector<Entity>& entities, float dt, std::mt19937& rng, float armorK = kArmorK,
                  std::vector<GameEvent>* events = nullptr);
