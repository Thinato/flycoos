#pragma once
#include <functional>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "entity.h"

constexpr float kDayLength = 4320;  // seconds per full day/night cycle (72 minutes)
constexpr float kTick = 1 / 60.0f;  // seconds per step(); see docs/DECISIONS.md, ADR-017

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
};

// What the player wants this step; main.cpp maps keys and mouse to it.
struct SimInput {
    Input move;             // walking; ignored while the player is knocked out (outside the editor)
    glm::vec2 aim{};        // nonzero: XZ direction attacks go toward (a lock-on); else the way it moves, else faces
    bool attack = false;    // the attack button went down since the last step (a tap is a light, a hold a heavy: melee.h)
    bool attackHeld = false;  // the attack button is down
    bool heavy = false, heavyHeld = false;  // the heavy button: went down, is down
    bool guard = false;     // the guard button is down
    bool interact = false;  // loot the nearest corpse, else talk to the nearest NPC
    bool editing = false;   // the editor is open: NPCs and combat pause, no attacks
    float timeRate = 1;     // time-of-day speed-up this step (1 normal, 0 frozen)
};

struct StepResult {
    bool attacked = false;   // an attack move started, or `attack` came while the player is knocked out
    bool respawned = false;  // the knocked-out player got back up at the player spawn
};

// A fresh player with the starting gear at the player spawn, fresh NPCs from the markers, the clock
// and region at 0, every quest not started and no flags, and the sim RNG seeded from `seed`. Call after the world and markers are in place.
void startSession(Session& s);
// Back at the player spawn with full health, out of combat.
void respawnPlayer(Session& s);
// Advances the simulation by `dt` seconds (the game always passes kTick): the NPCs' quest lines,
// interaction, the player's swing and movement, NPCs and combat (paused while editing), the time of
// day, which region the player is in, item count changes, then the quests drain the tick's events.
// A player knocked out for kPlayerRespawnSeconds gets back up first.
StepResult step(Session& s, const SimInput& in, float dt);
