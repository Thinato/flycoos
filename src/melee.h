#pragma once
#include <cstddef>

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

struct MoveSpec {
    const char* name;
    float seconds;              // its length at the weapon's speed 1
    float holdAt, impact;       // fractions of it: the windup pose (a charge or a telegraph holds there), the hit
    float chain, cancel;        // fractions: from here the next attack can start; a roll, the guard or walking can cut it
    float damage, stance;       // x the weapon's damage roll; stance damage dealt
    float stamina;              // stamina it costs
    float lunge;                // blocks/s it steps forward at its start, easing to 0 at the hit
    bool heavy;                 // charges while held; can't be made to flinch before its hit (hyperarmor)
};
inline constexpr MoveSpec kMoves[] = {
    //  name              s     hold  hit   chain cancel dmg   stance stam lunge heavy
    {"none",            1.0f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0, 0, 0.0f, false},
    {"light 1",         0.70f, 0.30f, 0.40f, 0.50f, 0.62f, 1.00f, 10, 12, 2.5f, false},  // diagonal, high right to low left
    {"light 2",         0.65f, 0.28f, 0.38f, 0.48f, 0.60f, 1.00f, 10, 12, 2.0f, false},  // backhand, left to right
    {"light 3",         0.70f, 0.30f, 0.40f, 0.50f, 0.62f, 1.05f, 11, 12, 2.5f, false},  // wide sweep, right to left
    {"light 4",         0.85f, 0.36f, 0.46f, 0.58f, 0.68f, 1.30f, 14, 14, 3.5f, false},  // thrust: the chain's finisher
    {"heavy 1",         1.05f, 0.40f, 0.52f, 0.62f, 0.72f, 1.50f, 22, 22, 4.0f, true},   // lunging thrust
    {"heavy 2",         1.10f, 0.42f, 0.54f, 0.64f, 0.74f, 1.55f, 24, 22, 3.0f, true},   // overhead chop
    {"running",         0.85f, 0.22f, 0.36f, 0.55f, 0.64f, 1.15f, 14, 14, 8.0f, false},  // leaping sweep
    {"rolling",         0.60f, 0.20f, 0.34f, 0.50f, 0.58f, 0.95f, 8, 10, 5.0f, false},   // rising thrust out of the roll
    {"jump light",      0.75f, 0.30f, 0.42f, 0.60f, 0.64f, 1.25f, 18, 14, 0.0f, false},  // chop on landing
    {"jump heavy",      0.95f, 0.34f, 0.46f, 0.64f, 0.70f, 1.65f, 28, 20, 0.0f, true},
    {"guard counter",   0.80f, 0.16f, 0.30f, 0.55f, 0.62f, 1.50f, 34, 16, 3.0f, true},   // rising slash off the guard
    {"riposte",         1.40f, 0.30f, 0.45f, 0.80f, 0.80f, 3.00f, 0, 0, 1.0f, false},    // a critical thrust: can't be hit
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
};
inline MeleeTuning gMelee;

// The player's attack and guard buttons for one tick.
struct MeleeInput {
    bool attack = false, attackHeld = false;  // LMB / R1: pressed since the last tick; down now
    bool heavy = false, heavyHeld = false;    // R2
    bool guard = false;                       // RMB / L1, held
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
