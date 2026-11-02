#include "player.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

constexpr float kStride = 1.7f;  // blocks travelled per full gait cycle
constexpr float kEps = 1e-3f;

bool overlapsSolid(const Grid& g, glm::vec3 p) {
    glm::ivec3 lo(glm::floor(p - glm::vec3(kHalfWidth, 0, kHalfWidth)));
    glm::ivec3 hi(glm::floor(p + glm::vec3(kHalfWidth, kHeight, kHalfWidth)));
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                if (isOpaque(g.get({x, y, z}))) return true;
    return false;
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

// ---- Character model --------------------------------------------------------

constexpr float kVoxel = 0.1f;  // character voxels are 1/10 of a world block

struct PartSpec { glm::ivec3 size; glm::vec3 center; };  // center in voxels, origin = feet
static const PartSpec kHumanoid[PartCount] = {
    {{10, 9, 9}, {0, 13.5f, 0.3f}},   // Head
    {{8, 7, 6}, {0, 5.5f, 0}},        // Torso
    {{3, 3, 3}, {5.8f, 5.5f, 0.4f}},  // HandL
    {{3, 3, 3}, {-5.8f, 5.5f, 0.4f}}, // HandR
    {{3, 2, 4}, {2.2f, 1, 0}},        // FootL
    {{3, 2, 4}, {-2.2f, 1, 0}},       // FootR
};
static const PartSpec kDummy[PartCount] = {
    {{7, 6, 7}, {0, 18, 0}},     // Head: burlap sack
    {{8, 7, 6}, {0, 11.5f, 0}},  // Torso: straw
    {{4, 2, 2}, {6, 13, 0}},     // HandL: straw arm
    {{4, 2, 2}, {-6, 13, 0}},    // HandR
    {{2, 8, 2}, {0, 6, 0}},      // FootL: post
    {{9, 2, 9}, {0, 1, 0}},      // FootR: cross-shaped base
};
static const PartSpec* specs(Model m) { return m == ModelDummy ? kDummy : kHumanoid; }

static Block dummyVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    switch (part) {
    case Head:  // sack tied at the neck, two stitched eyes on the front
        if (v.y == 0) return Belt;
        return v.z == s.z - 1 && v.y == 3 && (v.x == 2 || v.x == 4) ? Eye : Burlap;
    case Torso: return v.y == 1 || v.y == 5 ? Belt : Straw;  // rope bindings
    case HandL: case HandR: return Straw;
    case FootL: return Wood;
    default: return std::abs(v.x - 4) <= 1 || std::abs(v.z - 4) <= 1 ? Wood : Air;  // base
    }
}

Grid buildPart(Model model, Part part) {
    glm::ivec3 s = specs(model)[part].size;
    Grid g(s);
    Block tunic = model == ModelVillager ? Robe : Tunic, hair = model == ModelVillager ? DarkHair : Hair;
    for (int z = 0; z < s.z; ++z)
        for (int y = 0; y < s.y; ++y)
            for (int x = 0; x < s.x; ++x) {
                Block b = Skin;
                if (model == ModelDummy) {
                    b = dummyVoxel(part, {x, y, z}, s);
                } else if (part == Head) {
                    // Hair cap and back, sideburns; two tall eyes on the +Z (front) face.
                    if (y >= 6 || z <= 2 || ((x == 0 || x == s.x - 1) && y >= 3)) b = hair;
                    if (z == s.z - 1 && (y == 3 || y == 4) && (x == 2 || x == s.x - 3)) b = Eye;
                } else if (part == Torso) {
                    b = y == 2 ? Belt : tunic;
                } else if (part == FootL || part == FootR) {
                    b = Boots;
                }
                g.set({x, y, z}, b);
            }
    return g;
}

// Procedural walk/breathe/airborne animation as per-part offsets (voxels) and rotations.
static void animate(const Player& p, glm::vec3 (&off)[PartCount], glm::vec3 (&rot)[PartCount]) {
    float s = std::sin(p.gaitPhase), c = std::cos(p.gaitPhase);
    float a = p.gaitAmount, air = p.airAmount, a1 = std::min(a, 1.0f);
    float breathe = std::sin(p.breathPhase) * (1 - a1);
    float bob = std::abs(s) * 0.8f * a;

    off[Torso] = {0, bob + breathe * 0.25f, 0};
    rot[Torso] = {0.12f * a1, s * 0.12f * a, 0};
    off[Head] = {0, bob + breathe * 0.35f, 0.6f * a1};
    // Hands counter-swing against the feet; arms go up while airborne.
    off[HandL] = {0, bob + breathe * 0.3f + air * 2.5f, -s * 2.6f * a};
    off[HandR] = {0, bob + breathe * 0.3f + air * 2.5f, s * 2.6f * a};
    // Feet swing forward/back and lift on the forward half of their swing.
    off[FootL] = {0, std::max(0.0f, c) * 1.6f * a + air * 0.8f, s * 3.0f * a + air * 1.2f};
    off[FootR] = {0, std::max(0.0f, -c) * 1.6f * a + air * 0.8f, -s * 3.0f * a - air * 1.2f};
    rot[FootL].x = -s * 0.35f * a;
    rot[FootR].x = s * 0.35f * a;

    if (p.punching()) {  // jab: the fist shoots forward to shoulder height, the torso twists into it
        float u = p.punchTime / p.punchLength;
        float ext = u < kPunchImpact ? u / kPunchImpact : 1 - (u - kPunchImpact) / (1 - kPunchImpact);
        ext = ext * ext * (3 - 2 * ext);
        float side = p.punchRight ? 1.0f : -1.0f;  // the right hand is on -x, so it moves toward +x
        off[p.punchRight ? HandR : HandL] += glm::vec3(side * 2.5f, 2.5f, 8.0f) * ext;
        rot[Torso].y += side * 0.35f * ext;
        off[Head].z += 0.5f * ext;
    }
}

std::array<glm::mat4, PartCount> poseModel(const Player& p, Model model) {
    const PartSpec* parts = specs(model);
    glm::vec3 off[PartCount]{}, rot[PartCount]{};  // offsets in voxels, rotations (pitch x, yaw y)
    glm::mat4 tilt(1);  // the dummy above its base: wobbles back when hit, topples when out of health
    if (model == ModelDummy) {
        float fall = std::min(p.deadTime / 0.6f, 1.0f);
        float wobble = 0.3f * std::sin(p.hitTime * 18) * std::exp(-p.hitTime * 5);
        glm::vec3 pivot(0, 2, 0);  // top of the base, in voxels
        tilt = glm::translate(glm::mat4(1), pivot);
        tilt = glm::rotate(tilt, -(fall * fall * 1.45f + wobble), {1, 0, 0});
        tilt = glm::translate(tilt, -pivot);
    } else {
        animate(p, off, rot);
    }

    glm::mat4 base = glm::translate(glm::mat4(1), p.pos);
    base = glm::rotate(base, p.yaw, {0, 1, 0});
    base = glm::scale(base, glm::vec3(kVoxel));
    std::array<glm::mat4, PartCount> out;
    for (int i = 0; i < PartCount; ++i) {
        bool tilted = !(model == ModelDummy && i == FootR);
        glm::mat4 m = glm::translate(tilted ? base * tilt : base, parts[i].center + off[i]);
        m = glm::rotate(m, rot[i].y, {0, 1, 0});
        m = glm::rotate(m, rot[i].x, {1, 0, 0});
        out[i] = glm::translate(m, -glm::vec3(parts[i].size) * 0.5f);
    }
    return out;
}
