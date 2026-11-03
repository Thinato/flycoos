#pragma once
#include <array>

#include <glm/glm.hpp>

#include "item.h"
#include "stats.h"
#include "world.h"

// Player AABB: feet-centered, kHalfWidth on x/z, kHeight tall.
constexpr float kHalfWidth = 0.35f;
constexpr float kHeight = 1.8f;

struct Input {
    glm::vec2 move{};  // world-space XZ direction, length <= 1
    bool jump = false, run = false;
    bool descend = false;  // flight only
};

// Movement knobs, tweakable live from the debug UI.
struct PlayerTuning {
    float walkSpeed = 5, runSpeed = 9, jumpSpeed = 9.5f, gravity = 28;
    bool fly = false;     // no gravity: jump rises, descend sinks; run = 2.5x
    bool noclip = false;  // fly through terrain (implies fly)
    float flySpeed = 15;
};

// A walking body: the player, and the body of every NPC (driven by AI input instead of keys).
struct Player {
    glm::vec3 pos{}, vel{};  // pos = feet center
    float yaw = 0;           // radians; 0 faces +Z
    bool onGround = false, inWater = false;
    float gaitPhase = 0;     // advances with distance walked
    float gaitAmount = 0;    // 0 idle .. ~1.4 running
    float airAmount = 0;     // 0 grounded .. 1 airborne
    float breathPhase = 0;  // idle breathing cycle, radians
    PlayerTuning tuning;

    // Combat
    Stats stats;
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

    bool alive() const { return health > 0; }
    bool punching() const { return punchTime < punchLength; }  // any attack swing, armed or not
    // What's in hand: the main hand, or what the current swing started with (swapping mid-swing waits).
    ItemId weapon() const { return punching() ? swingWeapon : inv.equipped[SlotMainHand]; }
};

constexpr float kPunchSeconds = 0.35f;  // punch animation length (shorter if swings are faster)
constexpr float kPunchImpact = 0.4f;    // fraction of the animation where the fist lands

bool overlapsSolid(const Grid& g, glm::vec3 feet);
// `feet` raised a block at a time until the body is out of the terrain (up to the world's top), so a
// spawn buried by later edits doesn't trap whoever appears there.
glm::vec3 clearOfTerrain(const Grid& g, glm::vec3 feet);
void updatePlayer(Player& p, const Grid& g, const Input& in, float dt);

// How far a camera (a cube of half-size `radius`, bigger than the near plane) can move from `from`
// along unit `dir` before touching an opaque voxel, up to maxDist.
float cameraClearance(const Grid& g, glm::vec3 from, glm::vec3 dir, float maxDist, float radius);

// Character models: 6 floating voxel parts, each its own tiny grid meshed by meshRegion.
// The training dummy reuses the slots: FootL is its post, FootR its base, hands its arms.
enum Part { Head, Torso, HandL, HandR, FootL, FootR, PartCount };
// The boar reuses them as a quadruped: hands are its front legs, feet its hind legs.
// Weapons are held items: only their Head slot has voxels, posed by poseModel's `held` output.
// Armor is a humanoid whose parts are plated: a worn piece swaps in the part it covers.
enum Model { ModelPlayer, ModelVillager, ModelDummy, ModelBoar, ModelBandit, ModelSword, ModelKnife, ModelIronArmor,
             ModelCount };
Grid buildPart(Model model, Part part);
Model heldModel(ItemId weapon);
// Which model each part of `model` is drawn from: its own, or the armor worn over it. Greaves have
// no part (the rig has no legs), so they don't show.
std::array<Model, PartCount> partModels(Model model, const Inventory& inv);
// Part-grid space -> world space for the body's current animation pose (the dummy only wobbles
// when hit and topples when out of health; everyone else falls over). With a main-hand weapon
// equipped, a humanoid's `held` gets its pose (gripped in the right hand) and attacks are slashes
// instead of punches; the boar gores with its head.
std::array<glm::mat4, PartCount> poseModel(const Player& body, Model model, glm::mat4* held = nullptr);
