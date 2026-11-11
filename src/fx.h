#pragma once
// Hit feedback (ADR-058): sparks, blood, splinters and dust thrown out by hits, and a white smear behind a
// swinging blade. Presentation only: main feeds it the sim's sounds and the drawn poses; the sim never reads it,
// and it draws its own random numbers.
#include <cstddef>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "session.h"

struct FxSettings {
    bool sparks = true;   // metal on metal: blocks, parries, plate, arrows into stone
    bool blood = true;    // what a body hit throws out: blood, the dummy's splinters, a ghoul's dust
    bool chips = true;    // a wooden shield's splinters, a bare-armed block's puff, arrows into terrain
    bool trails = true;   // the white smear behind a swinging blade
    float amount = 1;           // x every burst's count
    float bloodSeconds = 6;     // a drop on the ground fades by then
    float chipSeconds = 1.5f;   // a splinter or chip on the ground
    float sparkSeconds = 0.3f;  // a spark's longest life
    float trailSeconds = 0.12f; // a smear fades out over this, from the blade back
    float trailSpeed = 6;       // blocks/s the blade's tip must move to leave a smear
    float reach = 48;           // nothing spawns farther than this from the eye
    int cap = 4096;             // particles at most: past it new ones overwrite the slots in turn
};

struct Particle {
    glm::vec3 pos{}, vel{};
    float size = 0.06f;     // blocks across
    float age = 0, life = 1;  // seconds; gone at its life
    float rest = 0;         // its life once it comes down on the ground (0: gone when it lands)
    float gravity = 22;     // blocks/s²
    glm::vec3 colour{1};    // sRGB, one colour all its life (docs/ART.md)
    bool glow = false;      // drawn unshaded (sparks)
    bool resting = false;   // lying on the ground, flattened, until it fades
};
// A particle's drawn size: sparks shrink over their life, anything shrinks away in its last quarter.
float drawnSize(const Particle& p);

struct TrailSample { glm::vec3 base{}, tip{}; float age = 0; bool start = false; };  // start: a new smear (no band from the one before)
// One body's smear: the edge as it was at each tick its blade moved fast, oldest first.
struct Trail {
    const void* body = nullptr;  // whose (its address: the same body frame to frame)
    std::vector<TrailSample> samples;
    glm::vec3 base{}, tip{};  // the edge last frame
    float since = 0;          // seconds since the edge last moved
    bool seen = false, frozen = false;  // trailFx ran for it this frame; it's in hit-stop (the smear holds)
};
struct RibbonVertex { glm::vec3 pos; float alpha; };

struct Fx {
    FxSettings set;
    std::vector<Particle> particles;
    size_t oldest = 0;                 // the slot a new particle takes once at the cap
    std::vector<Trail> trails;
    std::vector<RibbonVertex> ribbon;  // the smears as triangles (rebuilt by updateFx), for the renderer
    std::mt19937 rng{58};              // its own: the sim's isn't touched
    float ms = 0;                      // main's time on effects last frame (the debug window)
};

// The burst one sound makes, if within reach of the eye: hurt, death and crit (blood, splinters or dust by the
// creature, sparks off plate), block, parry, perfect guard, guard break and bounce (sparks, or a wooden shield's
// splinters, a bare arm's puff), an arrow into terrain (chips of the block, sparks off stone). Others: nothing.
void spawnFx(Fx& fx, const Sound& s, glm::vec3 eye);
// A body's blade this frame (call for every body holding one): `held` its item's pose (poseModel), `model` the
// item's model. While `swinging` (swingsBlade) and its tip moves at least trailSpeed, each new edge joins its
// smear; `frozen` (hit-stop in its own move: a reeling body's freeze stays set) holds the smear where it is.
// Bodies are told apart by address: one erased can hand its smear to the body moved into its place (a jump of
// over 1.5 blocks starts none).
void trailFx(Fx& fx, const void* body, Model model, const glm::mat4& held, bool swinging, bool frozen);
// Whether `b` is striking in an attack move (not a parry, a drink, a shot, a reload or a throw): from leaving its
// windup pose to a little past its impact frame (kSmearAfter of the move's length). Windups and recoveries leave no smear.
constexpr float kSmearAfter = 0.08f;
bool swingsBlade(const Player& b);
// Moves, lands and ages the particles and smears by `dt` (0: paused), drops trails no longer seen and empty,
// and rebuilds the ribbon.
void updateFx(Fx& fx, const Grid& world, float dt);
