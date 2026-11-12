#include "lockon.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {

bool lockable(const Entity& e) { return e.type().attackable && !bystander(e.type()) && e.body.alive(); }

// Its place on screen (x scaled by the aspect, so directions and distances are true), if it's in
// front of the camera.
std::optional<glm::vec2> onScreen(const LockView& v, glm::vec3 p) {
    glm::vec4 c = v.viewProj * glm::vec4(p, 1);
    if (c.w < 0.1f) return std::nullopt;
    return glm::vec2(c.x / c.w * v.aspect, c.y / c.w);
}

// Nothing opaque between the camera and `p` (the HUD's nameplate test).
bool inSight(const Grid& world, glm::vec3 eye, glm::vec3 p) {
    glm::vec3 d = p - eye;
    float dist = glm::length(d);
    return dist < 0.5f || cameraClearance(world, eye, d / dist, dist, 0.05f) >= dist - 0.5f;
}

}  // namespace

glm::vec3 lockPoint(const Entity& e) {
    Model m = e.type().model;
    float chest = m == ModelBoar || m == ModelWolf ? 0.7f : m == ModelChief ? 1.4f : m == ModelBarrowLord ? 1.7f : 1.1f;
    return e.body.pos + glm::vec3(0, chest, 0);
}

const Entity* lockedEntity(const LockOn& lock, const std::vector<Entity>& entities) {
    if (!lock.target) return nullptr;
    auto it = std::ranges::find(entities, lock.target, &Entity::id);
    return it != entities.end() ? &*it : nullptr;
}

uint32_t pickTarget(const std::vector<Entity>& entities, const Grid& world, const LockView& view, glm::vec3 player,
                    const LockSettings& s) {
    uint32_t best = 0;
    float bestScore = 1e9f;
    for (const Entity& e : entities) {
        glm::vec3 p = lockPoint(e);
        float dist = glm::distance(e.body.pos, player);
        if (!lockable(e) || dist > s.acquireRange) continue;
        auto at = onScreen(view, p);
        if (!at || std::abs(at->x) > view.aspect || std::abs(at->y) > 1 || !inSight(world, view.eye, p)) continue;
        float score = glm::length(*at) + 0.3f * dist / s.acquireRange;  // the middle of the view first
        if (score < bestScore) best = e.id, bestScore = score;
    }
    return best;
}

uint32_t switchTarget(const LockOn& lock, const std::vector<Entity>& entities, const Grid& world, const LockView& view,
                      glm::vec3 player, const LockSettings& s, glm::vec2 dir) {
    const Entity* current = lockedEntity(lock, entities);
    if (!current || glm::length(dir) < 1e-3f) return lock.target;
    auto from = onScreen(view, lockPoint(*current));
    if (!from) return lock.target;
    dir = glm::normalize(dir);
    uint32_t best = lock.target;
    float bestScore = 1e9f;
    for (const Entity& e : entities) {
        if (&e == current || !lockable(e) || glm::distance(e.body.pos, player) > s.breakRange) continue;
        glm::vec3 p = lockPoint(e);
        auto at = onScreen(view, p);
        if (!at) continue;
        glm::vec2 d = *at - *from;
        float len = glm::length(d);
        float along = len > 1e-4f ? glm::dot(d / len, dir) : 0;
        if (along < 0.5f || !inSight(world, view.eye, p)) continue;  // within 60 degrees of the flick
        float score = len * (1 + 2 * (1 - along));                  // nearest, straightest first
        if (score < bestScore) best = e.id, bestScore = score;
    }
    return best;
}

void updateLock(LockOn& lock, const std::vector<Entity>& entities, const Grid& world, const LockView& view,
                glm::vec3 player, const LockSettings& s, float dt) {
    if (!lock.target) return;
    const Entity* e = lockedEntity(lock, entities);
    if (e && !e->body.alive()) {  // a kill: on to the next one in reach, if any
        lock = {s.autoSwitch ? pickTarget(entities, world, view, player, s) : 0, 0};
        return;
    }
    if (!e || glm::distance(e->body.pos, player) > s.breakRange) {
        lock = {};
        return;
    }
    lock.unseen = inSight(world, view.eye, lockPoint(*e)) ? 0 : lock.unseen + dt;
    if (lock.unseen > s.unseenSeconds) lock = {};
}
