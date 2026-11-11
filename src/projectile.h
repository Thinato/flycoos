#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

#include "item.h"

struct Combatant;
struct Entity;
struct Player;
struct Session;

// Projectiles (ADR-050): arrows, in the sim at its fixed tick; the player's hit NPCs, an archer's (ADR-051) only
// the player. Each flies under gravity, swept from where it was to where it gets each tick (a voxel ray and a
// box per body, so nothing fast tunnels through a thin wall), and the first thing on its way stops it: a body
// takes the hit through strike() or strikePlayer() (guards block it, a dodge's i-frames let it through),
// terrain keeps it stuck where it went in for a while, to be picked back up. Deterministic: no randomness in flight.
struct Projectile {
    glm::vec3 pos{}, prevPos{}, vel{};  // its tip; drawn between prevPos and pos; blocks/s (stuck: the way it went in)
    ItemId weapon = NoItem;             // the bow that loosed it: its damage roll, type and skill
    float power = 1;                    // x the hit before armor: the shot's damage by its draw
    float stance = 0;                   // its stance damage
    float age = 0;                      // seconds in flight, or since it stuck
    bool stuck = false;                 // in terrain: it lies there stuckSeconds, to be picked up
    uint32_t owner = 0;                 // who loosed it: 0 the player, else the Entity::id of an archer (ADR-051)
};

// The knobs, live from the debug window's Projectiles node.
struct RangedTuning {
    float gravity = 10;                  // blocks/s^2 (bodies fall at 28: an arrow's flight is flatter)
    float minSpeed = 20, maxSpeed = 55;  // blocks/s leaving the bow, by the draw
    float boltSpeed = 75;                // blocks/s a crossbow's bolt leaves at (ADR-052; no draw): a flatter flight
    float drawSeconds = 0.6f;            // at the hold pose this long: full draw
    float minPower = 0.4f;               // x damage and stance at no draw (full draw: 1)
    float hipPitch = 0.05f;              // radians up a shot goes with nothing to aim at (along the facing)
    float flightSeconds = 5;             // an arrow still flying this long is gone
    float stuckSeconds = 30;             // stuck in terrain this long, then gone
    float pickupReach = 1.3f;            // blocks from the player's middle a stuck arrow is picked up within
    float aimWalk = 2.5f;                // blocks/s walking while aiming or drawing
    float hitRadius = 0.4f;              // half the width of the box a body is hit in
    float lobPitch = 0.4f;               // radians up a lobbed throwable goes with nothing to aim at (ADR-053)
    float burstEdge = 0.5f;              // x a burst's damage, stance and build-up at the edge of its radius (1 at its middle)
    float burstSeconds = 0.35f;          // how long a burst's puff shows
};
inline RangedTuning gRanged;
constexpr size_t kMaxProjectiles = 64;  // past it the oldest stuck arrow goes (else the oldest)

// A throwable bursting (ADR-053): where, how far it reaches, which one; shown as a puff for burstSeconds.
struct Burst {
    glm::vec3 pos{};
    float radius = 0;
    ItemId item = NoItem;
    float age = 0;
};

// How far the shot under way is drawn, 0 .. 1: its seconds at the hold pose over drawSeconds.
float drawOf(const Player& p);
// Where an arrow leaves the bow: in front of the chest, the way the body faces.
glm::vec3 shotOrigin(const Player& p);
// A shot of `speed` from `from`: with no `at`, along `yaw` (`pitch` up); else straight at `at` (aim
// mode: the crosshair's point), or with `lob` the low arc that comes down on it (a lock-on), the
// farthest one (45 degrees) when it's out of reach.
glm::vec3 shotVelocity(glm::vec3 from, float yaw, std::optional<glm::vec3> at, bool lob, float speed, float pitch = gRanged.hipPitch);
// Where the segment a..b first meets the box lo..hi, as a fraction of the way (0 when it starts inside).
std::optional<float> segmentBox(glm::vec3 a, glm::vec3 b, glm::vec3 lo, glm::vec3 hi);
// The box a body is hit in (feet-centred): hitRadius wide, as tall as its model.
struct Box3 { glm::vec3 lo, hi; };
Box3 hitBox(const Entity& e);

// Looses the player's drawn arrow (step() calls it at the shot's release): one from the quiver, at
// `aimAt` (SimInput's) straight in aim mode, else lobbed onto it, by the draw's speed and power. A
// crossbow (ADR-052) fires the bolt in it instead, at full power and boltSpeed.
void loose(Session& s, std::optional<glm::vec3> aimAt);
// Looses archer `e`'s drawn arrow (ADR-051; step() calls it at the release): lobbed at the player's chest as far
// away as the player is, along e's facing (it stops turning just before, so a sidestep can beat it), off by up
// to its kind's aimError either way and up or down. Its quiver never runs out.
void loose(Session& s, Entity& e);
// Fills each throwable's count in `c`'s pouch to its kind's pouch (ADR-053; a rest, a respawn, a new game), and
// readies the first kind if none is.
void fillPouch(Combatant& c);
// Readies the next throwable kind after the readied one (by item id, round), skipping those with none left
// unless none has any.
void cycleThrowable(Combatant& c);
// Throws the player's throwable at its throw's release (step() calls it): one from the pouch, from the left
// hand, at `aimAt` (SimInput's: lobbed onto a lock-on's target; no aim mode, a bow's), else along the facing
// (lobPitch up if it lobs, hipPitch if not), at its speed.
void hurl(Session& s, std::optional<glm::vec3> aimAt);
// A burst at `at` (ADR-053) from the player's throwable `item`: every living, attackable NPC within its radius
// and in sight of `at` struck through strike() (its damage, stance and its own build-up less toward the edge,
// burstEdge), guards facing `at` blocking it (right over one: facing `from`, where it flew in from); the player is
// never hurt by its own. Adds the puff to s.bursts and a Thunk.
void burst(Session& s, glm::vec3 at, ItemId item, glm::vec3 from);
// A tick of every projectile: flight, the first body or voxel on its way (an NPC's arrow: the player's, never
// another NPC's; its shooter gone, it flies through), sticking, expiry, and stuck arrows the player walks over
// back into its quiver (an archer's too). A throwable (ADR-053) that bursts does on the first body or voxel it
// meets; a thrown knife sticks like an arrow and goes back into the pouch. Ages the bursts' puffs.
void updateProjectiles(Session& s, float dt);
