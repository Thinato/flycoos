#pragma once
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "item.h"
#include "progress.h"
#include "stats.h"
#include "world.h"

// Player AABB: feet-centered, kHalfWidth on x/z, kHeight tall.
constexpr float kHalfWidth = 0.35f;
constexpr float kHeight = 1.8f;

struct Input {
    glm::vec2 move{};  // world-space XZ direction, length <= 1
    bool jump = false, run = false;
    bool descend = false;  // flight only
    bool roll = false;     // a tap: roll the way it moves (else the way it faces), on the ground or just after a jump
    glm::vec2 face{};      // nonzero: keep facing this XZ direction while walking (a lock-on); running and rolls don't
    float speed = 0;       // > 0: moves at this many blocks/s instead of walking or running (an attack's lunge)
};

// Movement knobs, tweakable live from the debug UI.
struct PlayerTuning {
    float walkSpeed = 5, runSpeed = 9, jumpSpeed = 9.5f, gravity = 28;
    bool fly = false;     // no gravity: jump rises, descend sinks; run = 2.5x
    bool noclip = false;  // fly through terrain (implies fly)
    float flySpeed = 15;
    float rollSpeed = 12, rollSeconds = 0.45f;  // a roll's speed at its start (it slows to half) and length (a medium load's)
    // The equip load (ADR-039): what the worn gear may weigh, from vitality; its share picks the roll.
    float loadBase = 30, loadPerVitality = 2.5f;
    float lightLoad = 0.3f, heavyLoad = 0.7f;   // under the first: light, over the second: heavy, over 1: overloaded
    float lightRollSpeed = 14;                  // light: farther, as quick
    float heavyRollSpeed = 7, heavyRollSeconds = 0.6f, heavyRollRecovery = 0.3f;  // heavy: shorter, slower, slower to get up
    float overloadedSpeed = 0.6f;               // overloaded: walks only, this much slower, can't roll or backstep
    // A backstep (ADR-038): the roll button standing still hops back, still facing ahead.
    float backstepSpeed = 7, backstepSeconds = 0.35f;
};

// How heavy the worn gear is against what the body can carry (ADR-039).
enum class Load : uint8_t { Light, Medium, Heavy, Over };
inline constexpr const char* kLoadNames[] = {"Light", "Medium", "Heavy", "Overloaded"};

// The moving body: position and physics (updatePlayer), its animation state, and where to draw it.
struct Body {
    glm::vec3 pos{}, vel{};  // pos = feet center
    float yaw = 0;           // radians; 0 faces +Z
    glm::vec3 prevPos{};     // pos and yaw at the start of the latest sim tick: drawn between them
    float prevYaw = 0;
    float prevGait = 0;      // gaitPhase then: a foot comes down in between (step() hears it, ADR-037)
    float landSpeed = 0;     // how fast it was falling when it last landed (blocks/s)
    bool onGround = false, inWater = false;
    float gaitPhase = 0;     // advances with distance walked
    float gaitAmount = 0;    // 0 idle .. ~1.4 running
    float airAmount = 0;     // 0 grounded .. 1 airborne
    float swimAmount = 0;    // 0 out of the water .. 1 chest-deep in it (swim pose)
    float breathPhase = 0;  // idle breathing cycle, radians
    float rollTime = 99, rollLength = 0;  // seconds since the latest roll began, and its length
    glm::vec2 rollDir{};                  // unit XZ direction of the latest roll
    bool backstep = false;                // the latest roll is a backstep (ADR-038): a hop back, facing ahead
    Load rollLoad = Load::Medium;         // the load it rolled with: a heavy one rolls short and slow (ADR-039)
    float jumpTime = 99;                  // seconds since the latest jump took off
    float offGround = 99;                 // seconds since it last stood on the ground (99 once it jumped)
    PlayerTuning tuning;

    bool rolling() const { return rollTime < rollLength; }

    // Moved without passing in between (spawned, respawned, flown to): drawn there at once, not slid.
    void teleport(glm::vec3 to) { pos = prevPos = to; prevYaw = yaw; }
};

// A melee move (ADR-035; melee.h has their timings): the four-swing light chain, the two heavies,
// and the attacks that depend on what the body was doing; the parry, and a drink from the healing
// flask (ADR-047), which commit like attacks but strike nothing. A bow's shot (ADR-050) draws to its
// hold point, holds while the button is down and looses an arrow at its impact frame. A crossbow's (ADR-052)
// shot doesn't hold: it fires at its impact frame, and its reload seats a bolt at its own. A throw (ADR-053)
// lets the readied throwable go from the left hand at its impact frame.
enum class Move : uint8_t { None, Light1, Light2, Light3, Light4, Heavy1, Heavy2, Running, Rolling, JumpLight, JumpHeavy,
                            GuardCounter, Riposte, Parry, Backstep, Drink, Shot, Reload, Throw,
                            Charge, Leap, Area, Grab,  // a creature's own attacks (ADR-057): timed by its data
                            Count };
// What a body did this tick that can be heard (ADR-037): bits of Combatant::noises, set where it
// happens and gathered by step() into Session::sounds. Steps and swim strokes come from the gait.
enum class Noise : uint8_t { Step, Swim, Jump, Land, Roll, Splash, Swing, Windup, Charged, GuardUp, Hurt, Crit, Death,
                             StanceBreak, Parry, Block, GuardBreak, Bounce, Loot, Talk, Backstep, PerfectGuard,
                             Bleed, Poison, Frost,  // a status took effect on it (ADR-046)
                             Drink, Empty, Rest,    // a flask healed it, an empty one didn't; it rested by a fire (ADR-047)
                             Grip,                  // it took its weapon in both hands or let go with one (ADR-048)
                             Loose, Thunk,          // a bow loosed an arrow; an arrow struck terrain (ADR-050)
                             Count };
static_assert(size_t(Noise::Count) <= 32, "Combatant::noises is 32 bits");
// What the player's attack buttons asked for and is still waiting to happen (melee.cpp).
enum class Want : uint8_t { None, Light, Heavy, Roll, Counter, Parry, Drink, Grip, Throw };

// A timed effect (ADR-046; TALENTS.md foundation 4): poison, frostbite, and later buffs, enchants and
// the like. Its modifiers count in the stat pipeline (collect) while it lasts; the clock runs in updateCombat.
enum class Effect : uint8_t { Poisoned, Frostbitten, Count };
inline constexpr const char* kEffectNames[] = {"Poisoned", "Frostbitten"};
static_assert(std::size(kEffectNames) == size_t(Effect::Count));
struct TimedEffect {
    Effect kind;
    std::string source;          // what put it on (a creature's or weapon's name), for the UI
    float seconds = 0, length = 0;  // left, and of how many
    int stacks = 1;
    std::vector<Modifier> mods;  // their source is the effect's name
    float tick = 0;              // seconds to its next tick of damage (poison)
};
// A damage number over a body (the HUD's): a hit's, or a status's (`kind`; Count: a hit).
struct DamageNumber {
    int amount = 0;
    float age = 99;  // seconds since; shown for one
    bool crit = false;
    Status kind = Status::Count;
};

constexpr int kQuiver = 30;  // arrows a quiver holds (ADR-050): a rest fills it, picking arrows up tops it up

// A move's timing and what it does. Every weapon class has its own (ADR-044): data/weapon_classes.json
// gives each class every attack move; None, Parry, Drink and Throw are code's (melee.h: kParrySpec: the parry is the off
// hand's, or a knife's or the fists', and the same for every class; kDrinkSpec, the flask's; kThrowSpec, ADR-053),
// and a creature attack's (ADR-057) is built from its kind's data into Combatant::special.
struct MoveSpec {
    float seconds = 0;          // its length at the weapon's speed 1
    float holdAt = 0, impact = 0;  // fractions of it: the windup pose (a charge or a telegraph holds there), the hit
    float chain = 0, cancel = 0;   // fractions: from here the next attack can start; a roll, the guard or walking can cut it
    float damage = 0, stance = 0;  // x the weapon's damage roll; stance damage dealt
    float stamina = 0;          // stamina it costs
    float lunge = 0;            // blocks/s it steps forward at its start, easing to 0 at the hit
    bool heavy = false;         // can't be made to flinch before its hit (hyperarmor; heavy 1 and 2 also charge)
    bool thrust = false;        // a blade's thrust pierces (its swings slash; ADR-040)
};

// What fights: attributes, health, the swing and damage timers, and the gear it wears and carries.
struct Combatant {
    Stats stats;                           // base attributes; sheet() adds gear on top
    int level = 1;                         // an NPC's is its kind's; the player's grows with XP (ADR-027)
    float health = float(maxHealth(Stats{}));
    float combatTimer = 0;                 // > 0: in combat, no regeneration
    float swingTimer = 0;                  // NPCs (startPunch): > 0, the next swing isn't ready
    Move move = Move::None;                // the attack under way
    float moveTime = 0;                    // seconds into it (stops while it holds its windup pose or freezes)
    float moveScale = 1;                   // its seconds per second of the move's own timing (weapon speed, haste)
    float hold = 0;                        // seconds more it holds the windup pose: an NPC's telegraph
    MoveSpec special{};                    // a creature attack's timing (ADR-057), from its kind's data: spec() of Charge, Leap, Area and Grab
    bool holding = false;                  // the player holds it there: charging a heavy, or a jump attack in the air
    float charge = 0;                      // seconds a heavy has charged
    bool charged = false;                  // fully: it hits harder
    bool punchRight = false;               // which hand the current punch uses (unarmed)
    bool hitPending = false;               // the current move hasn't reached its impact frame
    float freeze = 0;                      // seconds of hit-stop left: the move's clock stops
    Move fromMove = Move::None;            // the move this one cut short (a chain), and how far in: its windup blends from there
    float fromU = 0;
    WeaponClass fromClass = WeaponClass::Fists;  // the class that move swung with (its pose; ADR-044)
    Move lastMove = Move::None;            // the latest move to end, and seconds since: the chain goes on from it
    float sinceMove = 99;
    float stagger = 0, staggerLength = 0;  // seconds left reeling from a hit (no acting), of how many
    bool broken = false;                   // the reeling is a stance break (NPCs) or guard break (the player)
    float roar = 0;                        // NPCs: how far through a boss's roar into its next phase (ADR-059), 0..1; 0: not roaring
    float stance = 0;                      // NPCs: stance damage left before it breaks (EntityType::stance)
    float stanceWait = 0;                  // seconds until it starts coming back
    // The player's (melee.cpp): stamina, the guard, and button presses waiting for their moment.
    float stamina = 100;
    float staminaWait = 0;                 // seconds until it refills
    bool guarding = false;
    bool rollPaid = false;                 // the roll under way has spent its stamina
    float guardBlend = 0;                  // 0 .. 1: how far the guard is up (the pose)
    float blockTime = 99;                  // seconds since the guard took a hit
    float guardTime = 99;                  // seconds since the guard went up (or down)
    bool perfectReady = false;             // this guard went up after a rest: its first moments are a perfect guard (ADR-041)
    Load load = Load::Light;               // the worn gear's (updatePlayer)
    Want want = Want::None;                // a press waiting until the body can act on it
    float wantAge = 0;
    bool reloadWant = false;               // `want` is a crossbow's automatic reload (ADR-052), not a press: dropped if it no longer loads
    bool pressing = false;                 // the attack button is down and not yet a tap or a hold
    float pressTime = 0;
    bool chargeByHeavy = false;            // the charging heavy follows the heavy button (else the attack button)
    ItemId swingWeapon = NoItem;           // main hand when the swing started; it lands and animates with that
    float hitTime = 99;                    // seconds since this body last took damage
    ItemId hitWith = NoItem;               // what that hit was struck with (NoItem: bare hands, teeth, tusks)
    float hitStance = 0;                   // and its stance damage: how heavy it was
    glm::vec3 hitFrom{};                   // and where it came from (the hit's Strike::from), for its effects (ADR-058); the sim never reads it
    uint32_t noises = 0;                   // bits of Noise made this tick
    Move swung = Move::None;               // the move the latest Noise::Swing came from (it may be cut short the same tick)
    int lastDamage = 0;
    bool lastCrit = false;                 // that hit was a critical one (ADR-045): a random crit, a riposte or a backstab
    std::array<DamageNumber, 4> numbers{};  // the latest damage numbers (hurt() replaces the oldest)
    std::array<float, size_t(Status::Count)> buildup{};  // status build-up toward its resists (ADR-046); wears off
    std::vector<TimedEffect> effects;      // what's on it now (ADR-046)
    std::vector<Modifier> innate;          // its kind's own modifiers (creature "mods": a wolf's bleeding bite)
    float trauma = 0;                      // camera shake, 0..1 (the player's): big hits add it, it wears off (ADR-045)
    int flasks = kBaseFlasks;              // healing flask charges left (ADR-047); a rest fills them to the Flasks stat
    bool twoHanded = false;                // the player's main-hand weapon in both hands (ADR-048; twoHanding)
    float gripBlend = 0;                   // 0 .. 1: how far the left hand has gone to the hilt (the pose)
    int arrows = kQuiver;                  // in the quiver (ADR-050); a rest fills it, arrows stuck in terrain can be picked up
    bool aiming = false;                   // a bow's aim mode (ADR-050): the guard button with a bow
    float aimBlend = 0;                    // 0 .. 1: how far the bow is raised to aim (the pose, the camera)
    bool loosed = false;                   // the shot under way reached its release this tick: step() looses the arrow
    ItemId loaded = NoItem;                // the crossbow with a bolt in it (ADR-052; a reload seats one, the shot spends it); not saved
    std::array<uint8_t, 256> pouch{};      // throwables carried, by ItemId (ADR-053); a rest fills each to its kind's pouch
    ItemId readied = NoItem;               // the throwable the throw button throws (ADR-053)
    ItemId throwing = NoItem;              // the one in the hand during a throw (taken out at its release)
    bool hurled = false;                   // the throw under way reached its release this tick: step() throws it
    float deadTime = 0;                    // seconds spent at 0 health
    uint32_t heldBy = 0;                   // the player held by a grab (ADR-057): the grabbing NPC's id; 0: free
    Inventory inv;                         // equipment and bags
    Progress progress;                     // skills, XP and talents: the player's (NPCs' stay at the start)

    bool alive() const { return health > 0; }
    void noise(Noise n) { noises |= 1u << int(n); }
    bool made(Noise n) const { return noises >> int(n) & 1; }
    bool punching() const { return move != Move::None; }  // any attack move, armed or not
    // What's in hand: the main hand, or what the current swing started with (swapping mid-swing waits).
    ItemId weapon() const { return punching() ? swingWeapon : inv.equipped[SlotMainHand]; }
    const TimedEffect* effect(Effect k) const {
        for (const TimedEffect& e : effects)
            if (e.kind == k) return &e;
        return nullptr;
    }
};

// The modifiers on `c` (ADR-022): its innate ones, its equipped items' armor and mods, its skills' and
// talents' (ADR-027), its timed effects' (ADR-046) and two-handing's strength (ADR-048), rebuilt on every
// call, so unequipping, a respec or an effect wearing off needs no undo.
std::vector<Modifier> collect(const Combatant& c);
// Its final stats: base attributes plus every collected modifier. Uncached. `with`: the weapon skill
// of a swing, whose conditional ("with") modifiers count too.
inline StatSheet sheet(const Combatant& c, Skill with = Skill::Count) { return compute(c.stats, collect(c), with); }
// The weapon skill a swing with `weapon` uses: a punch is unarmed, every weapon so far one-handed.
constexpr Skill skillOf(ItemId weapon) { return weapon ? Skill::OneHanded : Skill::Unarmed; }

// The equip load (ADR-039): what `c` can wear (from its vitality), and how heavy what it wears is.
float maxEquipLoad(const Combatant& c, const PlayerTuning& t);
Load loadOf(const Combatant& c, const PlayerTuning& t);

// A walking, fighting character: the player, and the body of every NPC (driven by AI input instead
// of keys).
struct Player : Body, Combatant {};


// Where to draw a body `alpha` of the way through the current tick (0: as it was, 1: as it is).
inline glm::vec3 drawPos(const Body& b, float alpha) { return glm::mix(b.prevPos, b.pos, alpha); }
inline float drawYaw(const Body& b, float alpha) {  // the short way around
    return b.prevYaw + std::remainder(b.yaw - b.prevYaw, 2 * std::numbers::pi_v<float>) * alpha;
}

bool overlapsSolid(const Grid& g, glm::vec3 feet);
// `feet` raised a block at a time until the body is out of the terrain (up to the world's top), so a
// spawn buried by later edits doesn't trap whoever appears there.
glm::vec3 clearOfTerrain(const Grid& g, glm::vec3 feet);
void updatePlayer(Player& p, const Grid& g, const Input& in, float dt);

// How far a camera (a cube of half-size `radius`, bigger than the near plane) can move from `from`
// along unit `dir` before touching an opaque voxel, up to maxDist.
float cameraClearance(const Grid& g, glm::vec3 from, glm::vec3 dir, float maxDist, float radius);
