#pragma once
// Hit feedback (ADR-058): sparks, blood, splinters and dust thrown out by hits, and a white smear behind a
// swinging blade. The big moments (ADR-060): a glow on a charging heavy's weapon, a flash at a parry, a riposte or
// backstab and a critical hit, and how a damage number shows its type. Presentation only: main feeds it the sim's
// sounds and the drawn poses; the sim never reads it, and it draws its own random numbers.
#include <cstddef>
#include <cstdint>
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
    // ADR-060: a charging heavy's glow (its weapon tinted gold, motes rising off the blade, a ring at full charge).
    bool glow = true, motes = true;
    float glowLow = 0.6f, glowHigh = 0.95f;   // the tint's strength as the charge starts and just before it's full
    float glowFull = 1;                        // fully charged, until the hit (then it fades out over glowFade)
    float glowFade = 0.1f;                    // seconds
    float moteRate = 25;                      // motes a second at a full charge
    // The flashes: the body (a riposte, a crit) or the guard (a parry) turns white, shards fly, a soft white disc.
    bool flashes = true, disc = true;
    float flashSeconds = 0.15f;  // the body's or guard's flash fades over this
    float flashStrength = 0.75f; // a riposte's on the body (a random crit's: half; a parry's guard: 1)
    float discSeconds = 0.1f, discSize = 0.9f;  // the disc's life and radius (blocks; it grows to it)
};

// Damage numbers (ADR-060), the HUD's: each hit's type as a glyph left of the number, a guarded hit steel grey.
struct NumberSettings {
    bool glyphs = true;        // the type's glyph
    bool guarded = true;       // damage a guard let through: steel grey, smaller, a shield glyph
    bool outline = true;       // a dark outline around number and glyph (readable on snow and sky)
    float glyphSize = 0.7f;    // x the number's height
    float guardedScale = 0.85f;
};
inline NumberSettings gNumbers;
enum class Glyph : uint8_t { None, Slash, Strike, Pierce, Shield };
struct NumberStyle {
    glm::vec3 rgb{1};
    float scale = 1;  // x the number's size
    Glyph glyph = Glyph::None;
    bool bang = false;  // ends in "!"
};
constexpr glm::vec3 kCritRgb{1, 0.5f, 0.1f}, kGuardedRgb{0.72f, 0.78f, 0.86f};
// How `n` shows: a hit's in `hit` (who was hit: yellow on NPCs, red on the player), a critical's orange, 1.5x and
// "!", a guarded one's steel grey; its type's glyph (a guarded one's a shield). A status's (`status`, its colour)
// as ADR-046: no glyph, a bleed's "!", poison's small.
NumberStyle numberStyle(const DamageNumber& n, glm::vec3 hit, glm::vec3 status, const NumberSettings& set = gNumbers);

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

// What a flash lights: the whole body (a riposte or crit), or its guard (a parry: the shield, else the weapon).
enum class FlashOn : uint8_t { Body, Guard };
// A flash on one body (the sound's), fading over `length`.
struct Flash {
    const void* body = nullptr;
    FlashOn on = FlashOn::Body;
    glm::vec3 colour{1};  // sRGB
    float strength = 1, age = 0, length = 0.15f;
};
struct Disc { glm::vec3 pos{}; float age = 0; };  // a soft white disc facing the eye, growing and fading
// The colours: a parry cold white, a riposte warm gold-white; the charge gold, white-gold at full. A flash's and a full
// charge's tint is kBright x its colour: brighter than anything lit, so it turns white-hot.
constexpr glm::vec3 kParryRgb{0.85f, 0.93f, 1}, kRiposteRgb{1, 0.85f, 0.55f}, kChargeRgb{1, 0.55f, 0.08f}, kChargedRgb{1, 0.9f, 0.6f};
constexpr float kBright = 1.4f;

struct Fx {
    FxSettings set;
    std::vector<Particle> particles;
    size_t oldest = 0;                 // the slot a new particle takes once at the cap
    std::vector<Trail> trails;
    std::vector<RibbonVertex> ribbon;  // the smears and discs as triangles (rebuilt by updateFx), for the renderer
    std::vector<Flash> flashes;
    std::vector<Disc> discs;
    glm::vec3 eye{};                   // where the discs face (main sets it each frame)
    std::mt19937 rng{58};              // its own: the sim's isn't touched
    float ms = 0;                      // main's time on effects last frame (the debug window)
};

// The burst one sound makes, if within reach of the eye: hurt, death and crit (blood, splinters or dust by the
// creature, sparks off plate), block, parry, perfect guard, guard break and bounce (sparks, or a wooden shield's
// splinters, a bare arm's puff), an arrow into terrain (chips of the block, sparks off stone). Others: nothing.
// ADR-060: a parry also flashes the parrier's guard, throws shards and a disc; a riposte or backstab (a crit on an NPC
// with kRiposteStance) flashes the body, throws shards and a disc; a random crit flashes the body at half; a heavy's
// full charge throws a ring of motes.
void spawnFx(Fx& fx, const Sound& s, glm::vec3 eye);
// A body's tint this frame (rgb sRGB x kBright, a strength 0..1) on `on`: its flashes', fading as their age squared;
// the strongest wins.
glm::vec4 flashTint(const Fx& fx, const void* body, FlashOn on);
// The glow on `b`'s weapon (rgb sRGB, a strength): while a heavy charges at its hold point, gold rising from glowLow
// to glowHigh with the charge; charged, white-gold (x kBright) at glowFull until the hit, fading over glowFade after; let go
// early, fading over glowFade from where it was. Otherwise none.
glm::vec4 chargeTint(const Player& b, const FxSettings& set);
// Motes rising off a charging heavy's blade (`held` its item's pose, `model` the item; no blade: from `hand`), about
// moteRate x the tint's share of glowFull a second over `dt`.
void chargeFx(Fx& fx, const Player& b, Model model, const glm::mat4& held, glm::vec3 hand, float dt);
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
// Moves, lands and ages the particles, smears, flashes and discs by `dt` (0: paused), drops trails no longer seen
// and empty, and rebuilds the ribbon (the smears, then the discs facing fx.eye).
void updateFx(Fx& fx, const Grid& world, float dt);
