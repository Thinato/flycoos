#pragma once
#include <cstddef>
#include <optional>

#include <glm/glm.hpp>

#include "player.h"

// Melee after Elden Ring's one-handed sword (ADR-035). Every attack is a Move with a windup, an impact
// frame and a recovery, timed by kMoves (scaled by the weapon's speed). Taps of the attack button
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

struct MoveSpec {
    const char* name;
    float seconds;              // its length at the weapon's speed 1
    float holdAt, impact;       // fractions of it: the windup pose (a charge or a telegraph holds there), the hit
    float chain, cancel;        // fractions: from here the next attack can start; a roll, the guard or walking can cut it
    float damage, stance;       // x the weapon's damage roll; stance damage dealt
    float stamina;              // stamina it costs
    float lunge;                // blocks/s it steps forward at its start, easing to 0 at the hit
    bool heavy;                 // charges while held; can't be made to flinch before its hit (hyperarmor)
    bool thrust = false;        // a blade's thrust pierces (its swings slash; ADR-040)
};
inline constexpr MoveSpec kMoves[] = {
    //  name              s     hold  hit   chain cancel dmg   stance stam lunge heavy thrust
    {"none",            1.0f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0, 0, 0.0f, false},
    {"light 1",         0.70f, 0.30f, 0.40f, 0.50f, 0.62f, 1.00f, 10, 12, 2.5f, false},  // diagonal, high right to low left
    {"light 2",         0.65f, 0.28f, 0.38f, 0.48f, 0.60f, 1.00f, 10, 12, 2.0f, false},  // backhand, left to right
    {"light 3",         0.70f, 0.30f, 0.40f, 0.50f, 0.62f, 1.05f, 11, 12, 2.5f, false},  // wide sweep, right to left
    {"light 4",         0.85f, 0.36f, 0.46f, 0.58f, 0.68f, 1.30f, 14, 14, 3.5f, false, true},  // thrust: the chain's finisher
    {"heavy 1",         1.05f, 0.40f, 0.52f, 0.62f, 0.72f, 1.50f, 22, 22, 4.0f, true, true},   // lunging thrust
    {"heavy 2",         1.10f, 0.42f, 0.54f, 0.64f, 0.74f, 1.55f, 24, 22, 3.0f, true},   // overhead chop
    {"running",         0.85f, 0.22f, 0.36f, 0.55f, 0.64f, 1.15f, 14, 14, 8.0f, false},  // leaping sweep
    {"rolling",         0.60f, 0.20f, 0.34f, 0.50f, 0.58f, 0.95f, 8, 10, 5.0f, false, true},   // rising thrust out of the roll
    {"jump light",      0.75f, 0.30f, 0.42f, 0.60f, 0.64f, 1.25f, 18, 14, 0.0f, false},  // chop on landing
    {"jump heavy",      0.95f, 0.34f, 0.46f, 0.64f, 0.70f, 1.65f, 28, 20, 0.0f, true},
    {"guard counter",   0.80f, 0.16f, 0.30f, 0.55f, 0.62f, 1.50f, 34, 16, 3.0f, true},   // rising slash off the guard
    {"riposte",         1.40f, 0.30f, 0.45f, 0.80f, 0.80f, 3.00f, 0, 0, 1.0f, false, true},    // a critical thrust: can't be hit
    {"parry",           0.75f, 0.14f, 0.44f, 1.00f, 1.00f, 0.00f, 0, 10, 0.0f, false},   // a shield swipe (no hit; scaled by the shield)
    {"backstep",        0.70f, 0.22f, 0.38f, 0.55f, 0.62f, 1.05f, 12, 12, 7.0f, false},  // out of a backstep: a lunging sweep back in
};
static_assert(std::size(kMoves) == size_t(Move::Count));
inline const MoveSpec& spec(Move m) { return kMoves[size_t(m)]; }

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
    float fistScale = 0.75f;     // unarmed moves are this much shorter
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
};
inline MeleeTuning gMelee;

// The player's attack and guard buttons for one tick.
struct MeleeInput {
    bool attack = false, attackHeld = false;  // LMB / R1: pressed since the last tick; down now
    bool heavy = false, heavyHeld = false;    // R2
    bool guard = false;                       // RMB / L1, held
    bool parry = false;                       // Q / L2: pressed since the last tick
    glm::vec2 aim{};                          // nonzero: attack this way (a lock-on); else the way it moves, else faces
    bool riposte = false;                     // a stance-broken enemy is in reach in front (riposteTarget)
};

// The move's length, its impact frame and its hold point, in seconds.
inline float moveLength(const Combatant& c) { return spec(c.move).seconds * c.moveScale; }
inline float impactTime(const Combatant& c) { return spec(c.move).impact * moveLength(c); }
inline float holdTime(const Combatant& c) { return spec(c.move).holdAt * moveLength(c); }
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

// The shield in `b`'s off hand, or null.
const Shield* shieldOf(const Player& b);
// What `b` parries with: the parry's length (x its base timing) of its shield, else (the off hand empty)
// of its main-hand weapon or its fists; 0: it can't parry (a greatshield, a sword).
float parryOf(const Player& b);
// What a guard stops: the share of a blocked hit's damage and of its stamina damage.
struct Guard { float negation = 0, boost = 0; };
// `b`'s guard against `type`: its shield's, else its sword's, else its bare arms'.
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
