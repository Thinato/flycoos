#pragma once
#include <array>
#include <cstddef>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "player.h"

// Melee after Elden Ring's one-handed sword (ADR-035). Every attack is a Move with a windup, an impact
// frame and a recovery, timed by its weapon class's moveset (ADR-044; scaled by the weapon's speed). Taps of the attack button
// chain four different light swings; holding it (or the heavy button) makes a heavy that charges
// while held. What the body was doing picks the attack instead: running, just out of a roll, in the
// air (it strikes on landing), right after a block (a guard counter) or at a stance-broken enemy (a
// riposte). Attacks, rolls and blocked hits spend stamina; a hit on a guard with too little left
// breaks it. Hits wear down an NPC's hidden stance: at 0 it reels, open to a riposte; hits of at least
// its poise make it flinch, which cancels its attack. Unblocked hits make the player flinch too, except
// in a heavy's windup (hyperarmor), and a roll can't be hit for most of its length.
// A shield in the off hand (ADR-036) guards better than the sword, by its own negation and guard boost;
// a greatshield's guard makes weak attacks recoil. Small and medium shields parry: an attack that meets
// the parry's short active window deflects and leaves its attacker open to a riposte; caught before
// or after it, the parry is a weak guard. Without a shield a knife or bare fists parry too (ADR-041), and a
// guard raised just before a hit is a perfect guard: no damage and half the stamina. The roll button
// standing still is a backstep (ADR-038), with its own attack after it; the worn gear's weight makes
// rolls light, medium or heavy (ADR-039). Hits slash, strike or pierce (ADR-040).
// Criticals (ADR-045): an attack from behind an NPC that isn't attacking, reeling or just hurt is a
// backstab, the riposte's move: the NPC is caught, knocked down by the hit. Any other hit may be a
// random critical by the attacker's crit chance, for its crit damage more.
// The healing flask (ADR-047): R / D-pad down drinks, a move like the parry: it commits, walks slowly,
// and heals at its drink frame (a charge, its Flask healing share of max health); a hit that makes the
// player flinch before then cancels it, the charge kept. With none left it's only the gesture.
// Two-handing (ADR-048): F / Triangle+R1 takes the main-hand weapon in both hands (and back). Its hits
// count strength x1.5 (a modifier in collect), it guards better (twoHandNegation, twoHandBoost), and the
// off hand does nothing: no shield guard, no parry. The shield goes on the back.

// A move's timing and what it does. Every weapon class has its own (ADR-044): data/weapon_classes.json
// gives each class every attack move; None, Parry and Drink are code's (kParrySpec: the parry is the off
// hand's, or a knife's or the fists', and the same for every class; kDrinkSpec, the flask's).
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
// Ids in data files (none, parry and drink aren't: code has them) and names in the UI, by Move.
inline constexpr const char* kMoveIds[] = {"none", "light_1", "light_2", "light_3", "light_4", "heavy_1", "heavy_2", "running", "rolling",
                                           "jump_light", "jump_heavy", "guard_counter", "riposte", "parry", "backstep", "drink"};
inline constexpr const char* kMoveNames[] = {"none", "light 1", "light 2", "light 3", "light 4", "heavy 1", "heavy 2", "running",
                                             "rolling", "jump light", "jump heavy", "guard counter", "riposte", "parry", "backstep", "drink"};
static_assert(std::size(kMoveIds) == size_t(Move::Count) && std::size(kMoveNames) == size_t(Move::Count));
constexpr bool codeMove(Move m) { return m == Move::None || m == Move::Parry || m == Move::Drink; }  // not in data files
inline constexpr MoveSpec kParrySpec{0.75f, 0.14f, 0.44f, 1.00f, 1.00f, 0, 0, 10, 0};  // a swipe (no hit; scaled by its parry length)
// The flask to the lips by its hold point, the drink frame (impact: the heal), lowered after; a roll or
// the guard can cut its last quarter. Weapon speed doesn't change it.
inline constexpr MoveSpec kDrinkSpec{1.3f, 0.3f, 0.5f, 1.00f, 0.78f, 0, 0, 0, 0};

// A weapon class (ADR-044): its moveset, and how its hits feel.
struct WeaponClassDef {
    std::string name;
    float speed = 1;    // x every move's length on top of the weapon's speed (fists: 0.75, there's no item to say)
    float hitStop = 1;  // x a landed hit's hit-stop
    std::array<MoveSpec, size_t(Move::Count)> moves{};  // by Move
};
const WeaponClassDef& weaponClass(WeaponClass c);  // from the loaded defs
inline const MoveSpec& spec(WeaponClass c, Move m) { return weaponClass(c).moves[size_t(m)]; }
// The move `m` of the weapon `c` swings (its swing weapon's class), by default the one under way.
inline const MoveSpec& spec(const Combatant& c, Move m) { return spec(classOf(c.swingWeapon), m); }
inline const MoveSpec& spec(const Combatant& c) { return spec(c, c.move); }

// The knobs, live from the debug window's Player tab.
struct MeleeTuning {
    float staminaMax = 100;
    float staminaRegen = 45;     // per second, once staminaDelay has passed without acting
    float staminaDelay = 0.5f;
    float guardRegen = 0.35f;    // x the regen while guarding
    float runDrain = 8;          // per second running in combat (out of it running is free)
    float rollStamina = 14;
    float tapSeconds = 0.16f;    // the attack button held this long is a heavy, not a light
    float chargeSeconds = 0.55f; // a heavy held at its windup pose this long is charged
    float chargeDamage = 1.45f, chargeStance = 1.5f;  // a charged heavy's damage and stance damage, x
    float bufferSeconds = 0.5f;  // a press waits this long for the body to be able to act on it
    float chainGrace = 0.25f;    // after a light ends, the next one still goes on with the chain
    float guardNegation = 0.75f, fistNegation = 0.4f;  // the share of a blocked hit's damage the sword / the arms stop
    float guardBoost = 0.35f;    // the share of a blocked hit's stamina damage the sword saves (arms: none)
    float blockStamina = 10, blockStaminaPerDamage = 2.5f;  // a blocked hit's stamina damage: this + per point of its damage
    float guardAngle = 0.34f;    // cos of the widest angle off the facing the guard covers (about 70 degrees)
    float counterWindow = 0.8f;  // seconds after a block an attack is a guard counter
    float blockStun = 0.15f;     // seconds after a block before it can counter
    float flinchSeconds = 0.3f;
    float poisePerArmor = 0.4f;  // the player's poise: hits of less stance damage don't make it flinch
    float breakSeconds = 2.5f;   // a stance break
    float downSeconds = 1.2f;    // after a riposte, on the ground
    float guardBreakSeconds = 1.1f;
    float iframeFrom = 0.04f, iframeTo = 0.34f;  // seconds into a roll that hits pass through
    float trackSeconds = 0.12f;  // an NPC turns toward its target until this long before its strike
    float parryFrom = 0.1f, parryTo = 0.33f;  // seconds into a parry (at the shield's speed 1) it deflects
    float parrySeconds = 1.5f;   // a parried NPC reels this long, open to a riposte
    float parryGuard = 0.5f;     // a parry caught outside its window guards this much as well as the shield
    float recoilSeconds = 0.6f;  // an attack that bounces off a guard, or a parry short of opening it, rocks its attacker back
    float bounceBoost = 0.65f;   // guards of at least this boost (a greatshield's) bounce attacks...
    float bounceStance = 20;     // ...of less stance damage than this
    float backstepStamina = 8;   // a backstep's cost (a roll: rollStamina)
    float backstepIframeFrom = 0.02f, backstepIframeTo = 0.14f;  // a backstep's few i-frames
    float heavyIframeTo = 0.28f; // a heavy load's roll: fewer i-frames
    float heavyRegen = 0.7f;     // x the stamina regen with a heavy load
    float perfectGuardSeconds = 0.15f;  // a guard this fresh is perfect (ADR-041): no damage...
    float perfectStamina = 0.5f; // ...this much of the stamina damage...
    float perfectRearm = 0.3f;   // ...if the guard was down this long before (no tapping it)
    float fistParry = 1;         // the parry's length with bare fists (a knife's: its item's `parry`)
    float backstabAngle = 0.5f;  // cos of the widest angle off an NPC's back a backstab works from (60 degrees, ADR-045)
    float backstabWait = 2;      // seconds after it was last hurt before an NPC can be backstabbed (it turns to face its attacker)
    float hitShake = 0.5f;       // camera shake (trauma, 0..1) from the player's critical hits; heavier hits, a share by stance damage
    float drinkSpeed = 1.5f;     // blocks/s it walks while drinking from the flask (ADR-047)
    float twoHandStrength = 1.5f;  // x strength while two-handing (ADR-048)...
    float twoHandNegation = 0.85f, twoHandBoost = 0.45f;  // ...and the weapon's guard (one-handed: guardNegation, guardBoost)
};
inline MeleeTuning gMelee;

// The player's attack and guard buttons for one tick.
struct MeleeInput {
    bool attack = false, attackHeld = false;  // LMB / R1: pressed since the last tick; down now
    bool heavy = false, heavyHeld = false;    // R2
    bool guard = false;                       // RMB / L1, held
    bool parry = false;                       // Q / L2: pressed since the last tick
    bool drink = false;                       // R / D-pad down: pressed since the last tick (ADR-047)
    bool grip = false;                        // F / Triangle+R1: two-hand or let go, pressed since the last tick (ADR-048)
    glm::vec2 aim{};                          // nonzero: attack this way (a lock-on); else the way it moves, else faces
    bool riposte = false;                     // an enemy open to a critical is in reach in front (criticalTarget): a riposte or backstab
};

// The move's length, its impact frame and its hold point, in seconds.
inline float moveLength(const Combatant& c) { return spec(c).seconds * c.moveScale; }
inline float impactTime(const Combatant& c) { return spec(c).impact * moveLength(c); }
inline float holdTime(const Combatant& c) { return spec(c).holdAt * moveLength(c); }
inline float moveProgress(const Combatant& c) { return c.punching() ? c.moveTime / moveLength(c) : 0; }

// Starts `m` toward `aim` (XZ; zero keeps the facing) with the main-hand weapon (else fists), holding
// its windup pose `hold` seconds longer. Doesn't check whether it may.
void startMove(Player& b, Move m, glm::vec2 aim, float hold = 0);
// The move's clock: holds at the windup pose while `hold` or `holding` says so, stops while frozen,
// and ends the move.
void advanceMove(Combatant& c, float dt);
// Cancels whatever it was doing (an unlanded hit never lands) and makes it reel `seconds`.
void interrupt(Combatant& c, float seconds, bool broken);

// The player's buttons, every tick before updatePlayer: tap/hold, the buffer, the chain, charging,
// the guard and stamina. Rewrites `move` (the body's input) for what it's doing: none while reeling or
// mid-attack (but the attack's lunge), a buffered roll when it can roll. True if a move started.
bool updateMelee(Player& p, const MeleeInput& in, Input& move, float dt);

// Whether a hit now passes through `b` (a roll's i-frames, a riposte).
bool invulnerable(const Player& b);
// Whether a hit now doesn't make `b` flinch: a heavy before its hit, a riposte.
bool hyperarmor(const Combatant& b);
// The player's poise (from its armor): an NPC hit of less stance damage doesn't make it flinch.
float playerPoise(const Combatant& b);
// Whether `b`'s guard takes a hit from feet position `from` (in front of it).
bool guards(const Player& b, glm::vec3 from);

// Whether `b` holds its main-hand weapon in both hands (ADR-048): two-handed with a weapon to hold.
// (Mid-swing it's the swing's weapon: put away then, the swing keeps its grip to the end.)
inline bool twoHanding(const Combatant& b) { return b.twoHanded && b.weapon() != NoItem; }
// The shield in `b`'s off hand, or null; null too while two-handing (it's on the back).
const Shield* shieldOf(const Player& b);
// What `b` parries with: the parry's length (x its base timing) of its shield, else (the off hand empty)
// of its main-hand weapon or its fists; 0: it can't parry (a greatshield, a sword, two-handing).
float parryOf(const Player& b);
// What a guard stops: the share of a blocked hit's damage and of its stamina damage.
struct Guard { float negation = 0, boost = 0; };
// `b`'s guard against `type`: its shield's, else its weapon's (two-handed: better), else its bare arms'.
Guard guardOf(const Player& b, DamageType type = DamageType::Slash);
// The guard a hit of `type` from `from` meets now: the full one while guarding, a weak one (parryGuard)
// in a parry outside its window; none from behind or not guarding.
std::optional<Guard> guardAgainst(const Player& b, glm::vec3 from, DamageType type = DamageType::Slash);
// Whether `b`'s guard went up just now (perfectGuardSeconds, after a rest): a hit on it is a perfect guard.
bool perfectGuard(const Player& b);
// What the swing under way does: its weapon's damage type (a blade's thrusts pierce), else `bare` (its
// creature's, or fists': strike).
DamageType damageOf(const Combatant& c, DamageType bare = DamageType::Strike);
// Whether a hit from `from` meets `b`'s parry in its active window (in front of it).
bool parries(const Player& b, glm::vec3 from);
// Whether a guard of `g` makes an attack of `stance` damage bounce off (its attacker recoils).
bool bounces(const Guard& g, float stance);
