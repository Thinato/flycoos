#pragma once
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "entity.h"

// Lock-on (ADR-033): R3 or Tab locks onto the enemy nearest the middle of the view; the camera keeps
// it in sight, the player faces it while walking and attacks toward it. A flick (D-pad, arrow keys,
// right stick or mouse) moves the lock to the next enemy that way on screen. It lets go when the
// target gets too far or stays out of sight, and moves on to another when the target dies.

struct LockSettings {
    float acquireRange = 20;    // blocks: how far a new lock reaches
    float breakRange = 28;      // blocks: a lock lets go past this
    float unseenSeconds = 2;    // out of sight this long: lets go
    bool autoSwitch = true;     // the target died: lock onto the next one (if one is in reach)
};

struct LockOn {
    uint32_t target = 0;  // Entity::id, 0: none
    float unseen = 0;     // seconds the target has been out of sight
};

// What the camera saw: its view-projection (with the screen's aspect) and position.
struct LockView {
    glm::mat4 viewProj{1};
    glm::vec3 eye{};
    float aspect = 16.0f / 9;
};

// Where a lock aims on `e`: its chest.
glm::vec3 lockPoint(const Entity& e);
// The locked entity, or null (none, or it's gone).
const Entity* lockedEntity(const LockOn& lock, const std::vector<Entity>& entities);
// The id of the lockable entity (alive, attackable, not a bystander, within acquireRange of `player`, on screen and in
// line of sight) nearest the middle of the view, nearer ones winning close calls; 0 if none.
uint32_t pickTarget(const std::vector<Entity>& entities, const Grid& world, const LockView& view, glm::vec3 player,
                    const LockSettings& s);
// The id of the lockable entity (within breakRange, in front of the camera, in line of sight) next
// to the current target on screen in direction `dir` (x right, y up), within 60 degrees of it; the
// current target if there is none.
uint32_t switchTarget(const LockOn& lock, const std::vector<Entity>& entities, const Grid& world, const LockView& view,
                      glm::vec3 player, const LockSettings& s, glm::vec2 dir);
// Lets go of a target that's gone, past breakRange or unseenSeconds out of sight; a dead one passes the
// lock to pickTarget's choice when autoSwitch is on.
void updateLock(LockOn& lock, const std::vector<Entity>& entities, const Grid& world, const LockView& view,
                glm::vec3 player, const LockSettings& s, float dt);
