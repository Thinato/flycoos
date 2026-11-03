#pragma once
#include <cstdint>
#include <random>
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
    double regionSince = 0;
};

// What the player wants this step; main.cpp maps keys and mouse to it.
struct SimInput {
    Input move;             // walking; ignored while the player is knocked out (outside the editor)
    glm::vec2 aim{};        // XZ direction attacks go toward
    bool attack = false;    // swing toward `aim` as soon as the swing is ready
    bool interact = false;  // loot the nearest corpse, else talk to the nearest NPC
    bool editing = false;   // the editor is open: NPCs and combat pause, no attacks
    float timeRate = 1;     // time-of-day speed-up this step (1 normal, 0 frozen)
};

struct StepResult {
    bool attacked = false;   // `attack` was used: a swing started, or the player is knocked out
    bool respawned = false;  // the knocked-out player got back up at the player spawn
};

// A fresh player with the starting gear at the player spawn, fresh NPCs from the markers, the clock
// and region at 0, and the sim RNG seeded from `seed`. Call after the world and markers are in place.
void startSession(Session& s);
// Back at the player spawn with full health, out of combat.
void respawnPlayer(Session& s);
// Advances the simulation by `dt` seconds (the game always passes kTick): interaction, the player's swing and movement, NPCs and
// combat (paused while editing), the time of day, and which region the player is in. A player
// knocked out for kPlayerRespawnSeconds gets back up first.
StepResult step(Session& s, const SimInput& in, float dt);
