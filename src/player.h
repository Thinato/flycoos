#pragma once
#include <cmath>
#include <numbers>

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
};

// Movement knobs, tweakable live from the debug UI.
struct PlayerTuning {
    float walkSpeed = 5, runSpeed = 9, jumpSpeed = 9.5f, gravity = 28;
    bool fly = false;     // no gravity: jump rises, descend sinks; run = 2.5x
    bool noclip = false;  // fly through terrain (implies fly)
    float flySpeed = 15;
    float rollSpeed = 12, rollSeconds = 0.45f;  // a roll's speed at its start (it slows to half) and length
};

// The moving body: position and physics (updatePlayer), its animation state, and where to draw it.
struct Body {
    glm::vec3 pos{}, vel{};  // pos = feet center
    float yaw = 0;           // radians; 0 faces +Z
    glm::vec3 prevPos{};     // pos and yaw at the start of the latest sim tick: drawn between them
    float prevYaw = 0;
    bool onGround = false, inWater = false;
    float gaitPhase = 0;     // advances with distance walked
    float gaitAmount = 0;    // 0 idle .. ~1.4 running
    float airAmount = 0;     // 0 grounded .. 1 airborne
    float swimAmount = 0;    // 0 out of the water .. 1 chest-deep in it (swim pose)
    float breathPhase = 0;  // idle breathing cycle, radians
    float rollTime = 99, rollLength = 0;  // seconds since the latest roll began, and its length
    glm::vec2 rollDir{};                  // unit XZ direction of the latest roll
    float jumpTime = 99;                  // seconds since the latest jump took off
    float offGround = 99;                 // seconds since it last stood on the ground (99 once it jumped)
    PlayerTuning tuning;

    bool rolling() const { return rollTime < rollLength; }

    // Moved without passing in between (spawned, respawned, flown to): drawn there at once, not slid.
    void teleport(glm::vec3 to) { pos = prevPos = to; prevYaw = yaw; }
};

// What fights: attributes, health, the swing and damage timers, and the gear it wears and carries.
struct Combatant {
    Stats stats;                           // base attributes; sheet() adds gear on top
    int level = 1;                         // an NPC's is its kind's; the player's grows with XP (ADR-027)
    float health = float(maxHealth(Stats{}));
    float combatTimer = 0;                 // > 0: in combat, no regeneration
    float swingTimer = 0;                  // > 0: the next swing isn't ready
    float punchTime = 0, punchLength = 0;  // seconds into the punch animation, and its length
    bool punchRight = false;               // which hand the current punch uses
    bool hitPending = false;               // the current punch hasn't reached its impact frame
    ItemId swingWeapon = NoItem;           // main hand when the swing started; it lands and animates with that
    float hitTime = 99;                    // seconds since this body last took damage
    int lastDamage = 0;
    float deadTime = 0;                    // seconds spent at 0 health
    Inventory inv;                         // equipment and bags
    Progress progress;                     // skills, XP and talents: the player's (NPCs' stay at the start)

    bool alive() const { return health > 0; }
    bool punching() const { return punchTime < punchLength; }  // any attack swing, armed or not
    // What's in hand: the main hand, or what the current swing started with (swapping mid-swing waits).
    ItemId weapon() const { return punching() ? swingWeapon : inv.equipped[SlotMainHand]; }
};

// The modifiers on `c` (ADR-022): its equipped items' armor and mods, then its skills' and talents'
// (ADR-027), rebuilt on every call, so unequipping or a respec needs no undo.
std::vector<Modifier> collect(const Combatant& c);
// Its final stats: base attributes plus every collected modifier. Uncached. `with`: the weapon skill
// of a swing, whose conditional ("with") modifiers count too.
inline StatSheet sheet(const Combatant& c, Skill with = Skill::Count) { return compute(c.stats, collect(c), with); }
// The weapon skill a swing with `weapon` uses: a punch is unarmed, every weapon so far one-handed.
constexpr Skill skillOf(ItemId weapon) { return weapon ? Skill::OneHanded : Skill::Unarmed; }

// A walking, fighting character: the player, and the body of every NPC (driven by AI input instead
// of keys).
struct Player : Body, Combatant {};

constexpr float kPunchSeconds = 0.35f;  // punch animation length (shorter if swings are faster)
constexpr float kPunchImpact = 0.4f;    // fraction of the animation where the fist lands
constexpr float kSwingWindup = kPunchImpact * kPunchSeconds;  // seconds from a swing's start to its hit

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
