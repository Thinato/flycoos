#include "player.h"

#include <algorithm>
#include <cmath>
#include <numbers>

constexpr float kStride = 1.7f;  // blocks travelled per full gait cycle
constexpr float kEps = 1e-3f;
constexpr float kRollRecovery = 0.15f;  // seconds after a roll before the next one
constexpr float kAirRollWindow = 0.4f;  // seconds after a jump's take-off that a roll may still start (in the air)
constexpr float kRollCut = 0.12f;       // seconds a roll jumped out of takes to finish its turn
constexpr float kCoyoteTime = 0.1f;     // seconds after stepping off an edge that a jump still takes off

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
    p.rollTime = std::min(p.rollTime + dt, 99.0f);
    p.jumpTime = std::min(p.jumpTime + dt, 99.0f);
    p.offGround = p.onGround ? 0 : std::min(p.offGround + dt, 99.0f);
    p.inWater = g.get(glm::ivec3(glm::floor(p.pos + glm::vec3(0, 0.9f, 0)))) == Water;
    if (flying) {
        float speed = t.flySpeed * (in.run ? 2.5f : 1.0f);
        p.vel = {approach(p.vel.x, in.move.x * speed, 10, dt),
                 approach(p.vel.y, float(in.jump - in.descend) * speed, 10, dt),
                 approach(p.vel.z, in.move.y * speed, 10, dt)};
    } else {
        bool jumps = in.jump && p.offGround <= kCoyoteTime && !p.inWater;  // just off an edge still counts
        if (jumps) {
            p.jumpTime = 0, p.offGround = 99;  // one jump per time on the ground
            // Jumping out of a roll: the rest of it plays out in kRollCut seconds, in the air (time
            // scaled, so the pose carries on from where it was), and the jump keeps its speed.
            if (float left = p.rollLength - p.rollTime; p.rolling() && left > kRollCut) {
                p.rollTime *= kRollCut / left;
                p.rollLength *= kRollCut / left;
            }
        }
        // On the ground, or just after a jump: then the roll goes through the air with the jump.
        if (in.roll && (p.onGround || p.jumpTime < kAirRollWindow) && !p.inWater && !p.punching() && p.rollTime >= p.rollLength + kRollRecovery) {
            glm::vec2 facing{std::sin(p.yaw), std::cos(p.yaw)};
            p.rollDir = glm::length(in.move) > 0.1f ? glm::normalize(in.move) : facing;
            p.rollTime = 0, p.rollLength = t.rollSeconds;
            p.yaw = std::atan2(p.rollDir.x, p.rollDir.y);  // tucks facing the roll
        }
        glm::vec2 target = in.move * (in.speed > 0 ? in.speed : in.run ? t.runSpeed : t.walkSpeed) * (p.inWater ? 0.6f : 1.0f);
        float control = p.onGround || p.inWater ? 12.0f : 3.0f;
        p.vel.x = approach(p.vel.x, target.x, control, dt);
        p.vel.z = approach(p.vel.z, target.y, control, dt);
        if (p.rolling()) {  // the roll carries it, slowing from full speed to half
            glm::vec2 v = p.rollDir * t.rollSpeed * (1 - 0.5f * p.rollTime / p.rollLength);
            p.vel.x = v.x, p.vel.z = v.y;
        }
        if (p.inWater) {
            p.vel.y = in.jump ? 4.0f : std::max(p.vel.y - 6 * dt, -3.0f);
        } else {
            p.vel.y = std::max(p.vel.y - t.gravity * dt, -50.0f);
            if (jumps) p.vel.y = t.jumpSpeed;
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
    p.pos.x = std::clamp(p.pos.x, float(g.lo.x) + kHalfWidth, float(g.hi().x + 1) - kHalfWidth);
    p.pos.z = std::clamp(p.pos.z, float(g.lo.z) + kHalfWidth, float(g.hi().z + 1) - kHalfWidth);
    if (p.pos.y < 0) { p.pos.y = 0; p.vel.y = 0; p.onGround = true; }

    // Animation state: gait follows the distance actually covered, so feet neither slide
    // nor keep walking in place against a wall.
    glm::vec2 step(p.pos.x - before.x, p.pos.z - before.z);
    float moved = glm::length(step);
    float speed = moved / dt;
    // Backing away (facing a lock-on target) walks the gait backwards.
    bool backwards = glm::dot(step, glm::vec2(std::sin(p.yaw), std::cos(p.yaw))) < -0.3f * moved;
    p.gaitPhase = std::fmod(p.gaitPhase + (backwards ? -moved : moved) * kTau / kStride + kTau, kTau);
    bool walking = speed > 0.3f && !flying;
    p.gaitAmount = approach(p.gaitAmount, walking ? std::min(speed / t.walkSpeed, 1.4f) : 0, 10, dt);
    p.airAmount = approach(p.airAmount, (p.onGround || p.inWater) && !flying ? 0.0f : 1.0f, 12, dt);
    p.swimAmount = approach(p.swimAmount, p.inWater && !flying ? 1.0f : 0.0f, 6, dt);
    // Face where we're trying to go, or what's locked on while walking; a punch keeps facing where
    // it was aimed.
    glm::vec2 want = glm::length(in.face) > 1e-3f && !in.run && !p.rolling() && !flying ? in.face
                   : glm::length(glm::vec2(p.vel.x, p.vel.z)) > 0.3f                  ? glm::vec2(p.vel.x, p.vel.z)
                                                                                      : glm::vec2(0);
    if (!p.punching() && want != glm::vec2(0)) {
        float diff = std::remainder(std::atan2(want.x, want.y) - p.yaw, kTau);
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
