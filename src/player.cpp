#include "player.h"

#include <algorithm>
#include <cmath>
#include <numbers>

constexpr float kStride = 1.7f;  // blocks travelled per full gait cycle
constexpr float kEps = 1e-3f;

std::vector<Modifier> collect(const Combatant& c) {
    std::vector<Modifier> out;
    for (ItemId id : c.inv.equipped) {
        const ItemType& t = itemType(id);
        if (t.armor) out.push_back({Stat::Armor, ModKind::Flat, float(t.armor), t.name});
        for (Modifier m : t.mods) {
            m.source = t.name;
            out.push_back(m);
        }
    }
    progressModifiers(c.progress, out);
    return out;
}

bool overlapsSolid(const Grid& g, glm::vec3 p) {
    glm::ivec3 lo(glm::floor(p - glm::vec3(kHalfWidth, 0, kHalfWidth)));
    glm::ivec3 hi(glm::floor(p + glm::vec3(kHalfWidth, kHeight, kHalfWidth)));
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                if (isOpaque(g.get({x, y, z}))) return true;
    return false;
}

glm::vec3 clearOfTerrain(const Grid& g, glm::vec3 feet) {
    while (overlapsSolid(g, feet) && feet.y < float(g.size.y)) feet.y = std::floor(feet.y) + 1;
    return feet;
}

// Moves along one axis; on contact snaps flush to the block face. Horizontal moves
// on the ground may step up one block. |delta| must stay below one block.
static void moveAxis(Player& p, const Grid& g, int axis, float delta, bool canStep) {
    if (delta == 0) return;
    glm::vec3 next = p.pos;
    next[axis] += delta;
    if (!overlapsSolid(g, next)) { p.pos = next; return; }
    if (axis != 1 && canStep) {
        glm::vec3 up = next;
        up.y = std::floor(p.pos.y) + 1 + kEps;
        if (!overlapsSolid(g, up)) { p.pos = up; return; }
    }
    float below = axis == 1 ? 0 : kHalfWidth, above = axis == 1 ? kHeight : kHalfWidth;
    next[axis] = delta > 0 ? std::floor(next[axis] + above) - above - kEps
                           : std::floor(next[axis] - below) + 1 + below + kEps;
    if (!overlapsSolid(g, next)) p.pos = next;
    p.vel[axis] = 0;
    if (axis == 1 && delta < 0) p.onGround = true;
}

static float approach(float from, float to, float rate, float dt) {
    return from + (to - from) * (1 - std::exp(-rate * dt));
}

void updatePlayer(Player& p, const Grid& g, const Input& in, float dt) {
    dt = std::min(dt, 1 / 30.0f);
    constexpr float kTau = 2 * std::numbers::pi_v<float>;
    p.breathPhase = std::fmod(p.breathPhase + dt * 2.2f, kTau);

    const PlayerTuning& t = p.tuning;
    bool flying = t.fly || t.noclip;
    p.inWater = g.get(glm::ivec3(glm::floor(p.pos + glm::vec3(0, 0.9f, 0)))) == Water;
    if (flying) {
        float speed = t.flySpeed * (in.run ? 2.5f : 1.0f);
        p.vel = {approach(p.vel.x, in.move.x * speed, 10, dt),
                 approach(p.vel.y, float(in.jump - in.descend) * speed, 10, dt),
                 approach(p.vel.z, in.move.y * speed, 10, dt)};
    } else {
        glm::vec2 target = in.move * (in.run ? t.runSpeed : t.walkSpeed) * (p.inWater ? 0.6f : 1.0f);
        float control = p.onGround || p.inWater ? 12.0f : 3.0f;
        p.vel.x = approach(p.vel.x, target.x, control, dt);
        p.vel.z = approach(p.vel.z, target.y, control, dt);
        if (p.inWater) {
            p.vel.y = in.jump ? 4.0f : std::max(p.vel.y - 6 * dt, -3.0f);
        } else {
            p.vel.y = std::max(p.vel.y - t.gravity * dt, -50.0f);
            if (in.jump && p.onGround) p.vel.y = t.jumpSpeed;
        }
    }

    glm::vec3 delta = p.vel * dt;
    int steps = 1 + int(glm::max(glm::abs(delta.x), glm::max(glm::abs(delta.y), glm::abs(delta.z))) / 0.4f);
    // Swimming counts as grounded for step-up, so the player can climb out onto a shore.
    bool canStep = p.onGround || p.inWater;
    glm::vec3 before = p.pos;
    p.onGround = false;
    if (t.noclip) {  // pass straight through terrain
        p.pos += delta;
        steps = 0;
    }
    for (int i = 0; i < steps; ++i) {
        moveAxis(p, g, 0, delta.x / steps, canStep || p.onGround);
        moveAxis(p, g, 2, delta.z / steps, canStep || p.onGround);
        moveAxis(p, g, 1, delta.y / steps, false);
    }
    // Probe for ground instead of relying on this frame's fall reaching it: at high frame
    // rates one frame of gravity is smaller than the kEps rest gap.
    if (!t.noclip && p.vel.y <= 0 && overlapsSolid(g, p.pos - glm::vec3(0, 2 * kEps, 0))) {
        p.onGround = true;
        p.vel.y = 0;
    }
    p.pos.x = std::clamp(p.pos.x, kHalfWidth, g.size.x - kHalfWidth);
    p.pos.z = std::clamp(p.pos.z, kHalfWidth, g.size.z - kHalfWidth);
    if (p.pos.y < 0) { p.pos.y = 0; p.vel.y = 0; p.onGround = true; }

    // Animation state: gait follows the distance actually covered, so feet neither slide
    // nor keep walking in place against a wall.
    float moved = glm::length(glm::vec2(p.pos.x - before.x, p.pos.z - before.z));
    float speed = moved / dt;
    p.gaitPhase = std::fmod(p.gaitPhase + moved * kTau / kStride, kTau);
    bool walking = speed > 0.3f && !flying;
    p.gaitAmount = approach(p.gaitAmount, walking ? std::min(speed / t.walkSpeed, 1.4f) : 0, 10, dt);
    p.airAmount = approach(p.airAmount, (p.onGround || p.inWater) && !flying ? 0.0f : 1.0f, 12, dt);
    p.swimAmount = approach(p.swimAmount, p.inWater && !flying ? 1.0f : 0.0f, 6, dt);
    // Face where we're trying to go; a punch keeps facing where it was aimed.
    if (!p.punching() && glm::length(glm::vec2(p.vel.x, p.vel.z)) > 0.3f) {
        float diff = std::remainder(std::atan2(p.vel.x, p.vel.z) - p.yaw, kTau);
        p.yaw = std::remainder(p.yaw + diff * (1 - std::exp(-12 * dt)), kTau);
    }
}

float cameraClearance(const Grid& g, glm::vec3 from, glm::vec3 dir, float maxDist, float radius) {
    auto blocked = [&](glm::vec3 c) {
        glm::ivec3 lo(glm::floor(c - radius)), hi(glm::floor(c + radius));
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x)
                    if (isOpaque(g.get({x, y, z}))) return true;
        return false;
    };
    constexpr float kStep = 0.1f;
    for (float t = kStep; t < maxDist; t += kStep)
        if (blocked(from + dir * t)) return std::max(0.0f, t - kStep);
    return blocked(from + dir * maxDist) ? std::max(0.0f, maxDist - kStep) : maxDist;
}
