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
static const PartSpec kBoar[PartCount] = {
    {{7, 6, 6}, {0, 7.5f, 8.5f}},    // Head, out front
    {{8, 7, 12}, {0, 6.5f, 0}},      // Torso: the body, lengthwise along z
    {{3, 4, 3}, {2.5f, 2, 4}},       // HandL: front left leg
    {{3, 4, 3}, {-2.5f, 2, 4}},      // HandR: front right leg
    {{3, 4, 3}, {2.5f, 2, -4}},      // FootL: hind left leg
    {{3, 4, 3}, {-2.5f, 2, -4}},     // FootR: hind right leg
};
static const PartSpec* specs(Model m) { return m == ModelDummy ? kDummy : m == ModelBoar ? kBoar : kHumanoid; }

static Block boarVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    bool front = v.z == s.z - 1;
    switch (part) {
    case Head:  // snout with nostrils, tusks either side of it, small eyes above
        if (front && v.x >= 2 && v.x <= 4 && v.y <= 2) return v.y == 1 && v.x != 3 ? Eye : Snout;
        if (front && (v.x == 1 || v.x == 5) && v.y <= 1) return Tusk;
        if (front && (v.x == 1 || v.x == 5) && v.y == 4) return Eye;
        return v.y == s.y - 1 && v.x == 3 ? DarkHair : Hide;
    case Torso: return v.y == s.y - 1 && (v.x == 3 || v.x == 4) ? DarkHair : Hide;  // bristly ridge
    default: return v.y == 0 ? DarkHair : Hide;  // legs with dark hooves
    }
}

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

// One-handed blades, up along +y, edges along x, all gripped at kGrip.
constexpr glm::vec3 kGrip{2.5f, 2.5f, 0.5f};
static Grid buildSword() {
    Grid g({5, 15, 1});
    g.set({2, 0, 0}, Iron);                                    // pommel
    for (int y = 1; y <= 3; ++y) g.set({2, y, 0}, Belt);       // leather grip
    for (int x = 0; x < 5; ++x) g.set({x, 4, 0}, Iron);        // crossguard
    for (int y = 5; y <= 13; ++y)
        for (int x = 1; x <= 3; ++x) g.set({x, y, 0}, Steel);  // blade
    g.set({2, 14, 0}, Steel);                                  // tip
    return g;
}
static Grid buildKnife() {  // shorter and narrower, single-edged
    Grid g({5, 11, 1});
    g.set({2, 0, 0}, Iron);
    for (int y = 1; y <= 3; ++y) g.set({2, y, 0}, Belt);
    for (int x = 1; x <= 3; ++x) g.set({x, 4, 0}, Iron);
    for (int y = 5; y <= 9; ++y) g.set({2, y, 0}, Steel);
    for (int y = 5; y <= 8; ++y) g.set({3, y, 0}, Steel);
    g.set({2, 10, 0}, Steel);
    return g;
}

Model heldModel(ItemId weapon) { return weapon == Knife ? ModelKnife : ModelSword; }

Grid buildPart(Model model, Part part) {
    if (model == ModelSword || model == ModelKnife)
        return part != Head ? Grid({1, 1, 1}) : model == ModelSword ? buildSword() : buildKnife();
    glm::ivec3 s = specs(model)[part].size;
    Grid g(s);
    Block tunic = model == ModelVillager ? Robe : model == ModelBandit ? Leather : Tunic;
    Block hair = model == ModelVillager ? DarkHair : model == ModelBandit ? Hood : Hair;
    for (int z = 0; z < s.z; ++z)
        for (int y = 0; y < s.y; ++y)
            for (int x = 0; x < s.x; ++x) {
                Block b = Skin;
                if (model == ModelDummy) {
                    b = dummyVoxel(part, {x, y, z}, s);
                } else if (model == ModelBoar) {
                    b = boarVoxel(Part(part), {x, y, z}, s);
                } else if (part == Head) {
                    // Hair cap and back, sideburns; two tall eyes on the +Z (front) face. The
                    // bandit's hood is its hair, and a mask covers the lower face.
                    if (y >= 6 || z <= 2 || ((x == 0 || x == s.x - 1) && y >= 3)) b = hair;
                    if (model == ModelBandit && y <= 2 && z >= s.z - 2) b = Hood;
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

static float smooth(float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return a * a * (3 - 2 * a);
}

// Sword slash over the swing: wind up to the right side, sweep through the front at the impact
// frame, follow through to the left, settle back. x: sweep angle in radians (+ = toward the
// left hand), y: blend from the idle grip (0) into the slash pose (1).
static glm::vec2 slash(const Player& p) {
    if (!p.punching()) return {0, 0};
    constexpr float kWind = 0.15f, kFollow = 2 * kPunchImpact - kWind, kSweep = 1.4f;
    float u = p.punchTime / p.punchLength;
    if (u < kWind) return {-kSweep * smooth(u / kWind), smooth(u / kWind)};
    if (u < kFollow) return {kSweep * (2 * smooth((u - kWind) / (kFollow - kWind)) - 1), 1};
    float back = 1 - smooth((u - kFollow) / (1 - kFollow));
    return {kSweep * back, back};
}

// 0 -> 1 up to the impact frame, then back to 0.
static float thrust(const Player& p) {
    if (!p.punching()) return 0;
    float u = p.punchTime / p.punchLength;
    return smooth(u < kPunchImpact ? u / kPunchImpact : 1 - (u - kPunchImpact) / (1 - kPunchImpact));
}

// Procedural walk/breathe/airborne/attack animation as per-part offsets (voxels) and rotations.
static void animate(const Player& p, bool armed, glm::vec3 (&off)[PartCount], glm::vec3 (&rot)[PartCount]) {
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

    if (armed) {  // the sword hand sweeps an arc at chest height around the body; the torso turns with it
        glm::vec2 sl = slash(p);
        glm::vec3 arc(-1.5f + 6.5f * std::sin(sl.x), 9, 1.5f + 6.5f * std::cos(sl.x));
        off[HandR] += (arc - kHumanoid[HandR].center) * sl.y;
        rot[Torso].y += 0.4f * sl.x * sl.y;
    } else if (p.punching()) {  // jab: the fist shoots forward to shoulder height, the torso twists into it
        float ext = thrust(p);
        float side = p.punchRight ? 1.0f : -1.0f;  // the right hand is on -x, so it moves toward +x
        off[p.punchRight ? HandR : HandL] += glm::vec3(side * 2.5f, 2.5f, 8.0f) * ext;
        rot[Torso].y += side * 0.35f * ext;
        off[Head].z += 0.5f * ext;
    }
}

// Trot (diagonal legs together), breathing, and a goring head thrust.
static void animateBoar(const Player& p, glm::vec3 (&off)[PartCount], glm::vec3 (&rot)[PartCount]) {
    float s = std::sin(p.gaitPhase * 1.5f), c = std::cos(p.gaitPhase * 1.5f);  // shorter legs, quicker steps
    float a = std::min(p.gaitAmount, 1.4f), breathe = std::sin(p.breathPhase) * (1 - std::min(a, 1.0f));
    float bob = std::abs(s) * 0.4f * a + breathe * 0.2f;
    off[Torso] = {0, bob, 0};
    off[Head] = {0, bob + std::sin(p.gaitPhase) * 0.3f * a, 0};
    for (auto [leg, sign] : {std::pair{HandL, 1.0f}, std::pair{FootR, 1.0f}, std::pair{HandR, -1.0f}, std::pair{FootL, -1.0f}}) {
        off[leg] = {0, std::max(0.0f, sign * c) * 1.2f * a, sign * s * 2.2f * a};
        rot[leg].x = -sign * s * 0.4f * a;
    }
    float g = thrust(p);  // head down and forward, then tusks up at impact
    off[Head] += glm::vec3(0, -0.8f, 3.0f) * g;
    rot[Head].x = -0.35f * g;
    off[Torso].z += 1.0f * g;
}

std::array<glm::mat4, PartCount> poseModel(const Player& p, Model model, glm::mat4* held) {
    bool armed = model != ModelDummy && model != ModelBoar && p.weapon() != NoItem;
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
        if (model == ModelBoar) animateBoar(p, off, rot);
        else animate(p, armed, off, rot);
        if (!p.alive()) {  // falls over onto its side
            float fall = std::min(p.deadTime / 0.5f, 1.0f);
            glm::vec3 pivot(0, 4, 0);  // about half a body's thickness up, so it lies on the ground
            tilt = glm::translate(glm::mat4(1), pivot);
            tilt = glm::rotate(tilt, fall * fall * 1.5f, {0, 0, 1});
            tilt = glm::translate(tilt, -pivot);
        }
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
    if (held && armed) {  // blade tilted forward from the fist; flatter and swept around while slashing
        glm::vec2 sl = slash(p);
        glm::mat4 m = glm::translate(base * tilt, parts[HandR].center + off[HandR]);
        m = glm::rotate(m, sl.x, {0, 1, 0});
        m = glm::rotate(m, 1.0f + 0.5f * sl.y, {1, 0, 0});
        *held = glm::translate(m, -kGrip);
    }
    return out;
}
