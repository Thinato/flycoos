#pragma once
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "player.h"

enum class EntityKind { TrainingDummy, Villager };

// Fixed traits per kind.
struct EntityType {
    const char* name;  // null: a random villager name
    int level, maxHealth;
    bool attackable;   // can take damage
    bool wanders;      // walks around its home spot
    bool talks;        // E shows a speech balloon
    Model model;
};
const EntityType& typeOf(EntityKind kind);

struct Entity {
    EntityKind kind;
    std::string name;
    int level = 1;
    Player body;           // position, animation, health
    glm::vec3 home{};      // wanderers stay near this
    glm::vec2 wanderDir{}; // zero = standing still
    float wanderTimer = 0; // seconds until the next wander decision
    int phrase = -1;       // index into the phrase list, -1 = silent
    float speechTimer = 0; // seconds the balloon stays up
    std::mt19937 rng;

    const EntityType& type() const { return typeOf(kind); }
    const char* speech() const;  // current balloon text, null when silent
};

constexpr float kTalkRange = 3.5f;  // blocks between feet
constexpr float kWanderRadius = 12;

// The training dummy and a villager on grass beside `near` (usually the player spawn).
std::vector<Entity> spawnEntities(const Grid& world, glm::vec3 near, uint32_t seed);
// AI and physics for every entity; talking villagers stop and face the player.
void updateEntities(std::vector<Entity>& entities, const Grid& world, const Player& player, float dt);
// Keeps the player from walking through entities (never pushes into terrain).
void separate(Player& player, const Grid& world, const std::vector<Entity>& entities);
// Nearest entity within talk range that can talk, or null.
Entity* talkTarget(std::vector<Entity>& entities, const Player& player);
// Starts a new random phrase (different from the last one).
void talk(Entity& e);
// Applies damage if the entity can be attacked and has health left; health stops at 0.
// Returns whether it applied. ponytail: no death state yet; decide when combat lands.
bool damage(Entity& e, int amount);
