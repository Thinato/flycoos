#pragma once
#include <functional>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "entity.h"
#include "projectile.h"

constexpr float kDayLength = 4320;  // seconds per full day/night cycle (72 minutes)
constexpr float kTick = 1 / 60.0f;  // seconds per step(); see docs/DECISIONS.md, ADR-017

// A noise made in the world (ADR-037), for main.cpp to play: what, where, by whom and how.
struct Sound {
    Noise what = Noise::Step;
    glm::vec3 pos{};
    int kind = -1;           // the EntityKind that made it; -1: the player
    Block ground = Air;      // what it stands on (steps, landings, rolls)
    Move move = Move::None;  // the move it was in (swings: the one swung)
    ItemId with = NoItem;    // swings: the weapon; hurt: what hit it; blocks: what took it (a shield, the main hand)
    float amount = 0;        // steps: the gait (0..1.4); landings: the fall speed; hurt: the hit's stance damage
};
constexpr size_t kMaxSounds = 256;  // Session::sounds stops growing here when nobody plays them

// The simulation state: the world and everything that lives in it. main.cpp keeps the window,
// camera, renderer, UI and editor around it and drives it with step().
struct Session {
    Grid world;
    uint32_t seed = 0;                // what the world was generated from; saved with it; seeds spawns and `rng`
    std::vector<Instance> instances;  // placed prefabs, saved with the world
    std::vector<Marker> markers;      // story markers (spawns, places), saved with the world
    Player player;
    std::vector<Entity> entities;
    std::mt19937 rng{};                     // damage rolls; seeded by startSession
    double elapsed = 0;                     // simulated seconds
    float timeOfDay = 0.3f, timeScale = 1;  // see FrameParams::timeOfDay; timeScale 0 = time paused
    float armorK = kArmorK;                 // armor balance: see armorReduction
    uint32_t regionId = 0;                  // the region marker the player is in (0: none), since `regionSince` of `elapsed`
    // Where the world is in (the endless world streams, stream.h); null: everywhere. Nothing moves elsewhere.
    std::function<bool(glm::vec3)> loaded;
    double regionSince = 0;
    // Quests (ADR-026): this tick's events, drained once at the end of step(); each quest's state
    // (by handle, defs().quests); the world flags; the player's item counts as of the last tick
    // (a change raises ItemCountChanged; empty: everything held counts as new).
    std::vector<GameEvent> events;
    std::vector<QuestState> quests;
    std::map<std::string, int32_t, std::less<>> flags;
    std::vector<int> itemCounts;
    std::deque<std::string> eventLog;  // the latest kEventLogSize drained events, newest last (debug window)
    std::vector<std::pair<int, std::string>> toasts;  // quest news since main last took them: quest handle (-1: none), text
    std::vector<Sound> sounds;  // noises since main last took them, oldest first
    std::optional<glm::vec3> restPoint;  // where the player last rested (ADR-047): it gets back up there
    std::vector<Projectile> projectiles;  // arrows in flight or stuck in terrain (ADR-050), oldest first; not saved
};

// What the player wants this step; main.cpp maps keys and mouse to it.
struct SimInput {
    Input move;             // walking; ignored while the player is knocked out (outside the editor)
    glm::vec2 aim{};        // nonzero: XZ direction attacks go toward (a lock-on); else the way it moves, else faces
    bool attack = false;    // the attack button went down since the last step (a tap is a light, a hold a heavy: melee.h)
    bool attackHeld = false;  // the attack button is down
    bool heavy = false, heavyHeld = false;  // the heavy button: went down, is down
    bool guard = false;     // the guard button is down
    bool parry = false;     // the parry button went down (ADR-036)
    bool drink = false;     // the flask button went down (ADR-047)
    bool grip = false;      // the two-hand button went down (ADR-048)
    // What a bow's shot goes at (ADR-050): in aim mode the point under the crosshair (straight at it),
    // locked on the target's middle (lobbed onto it); none: along the facing.
    std::optional<glm::vec3> aimAt;
    bool interact = false;  // loot the nearest corpse, else talk to the nearest NPC, else rest by a fire (canRest)
    bool editing = false;   // the editor is open: NPCs and combat pause, no attacks
    float timeRate = 1;     // time-of-day speed-up this step (1 normal, 0 frozen)
};

struct StepResult {
    bool attacked = false;   // a move started (an attack, a parry, a drink), or `attack` came while the player is knocked out
    bool respawned = false;  // the knocked-out player got back up at the player spawn
    bool rested = false;     // the player rested by a fire (ADR-047)
};
constexpr float kRestReach = 3;  // blocks from the player's feet to an Ember it can rest by (ADR-047)

// A fresh player with the starting gear at the player spawn, fresh NPCs from the markers, the clock
// and region at 0, every quest not started and no flags, and the sim RNG seeded from `seed`. Call after the world and markers are in place.
void startSession(Session& s);
// Back where it last rested (else at the player spawn) with full health, flasks and quiver, out of combat.
void respawnPlayer(Session& s);
// Whether the player can rest now (ADR-047): alive, not hit for kCombatSeconds (poison's ticks don't
// count) with nothing angry near, not mid-move,
// and by a fire (an Ember block within kRestReach: a campfire, a hearth, a shrine's brazier).
bool canRest(const Session& s);
// Advances the simulation by `dt` seconds (the game always passes kTick): the NPCs' quest lines,
// interaction, the player's swing and movement, NPCs and combat (paused while editing), the time of
// day, which region the player is in, a bow's arrow loosed and every projectile's tick (ADR-050), item count changes, then the quests drain the tick's events,
// and what everyone did that can be heard joins `sounds`.
// A player knocked out for kPlayerRespawnSeconds gets back up first.
StepResult step(Session& s, const SimInput& in, float dt);
