#include "projectile.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "edit.h"
#include "melee.h"
#include "session.h"

float drawOf(const Player& p) { return std::clamp(p.charge / gRanged.drawSeconds, 0.0f, 1.0f); }

glm::vec3 shotOrigin(const Player& p) {
    return p.pos + glm::vec3(0.5f * std::sin(p.yaw), 1.35f, 0.5f * std::cos(p.yaw));
}

glm::vec3 shotVelocity(glm::vec3 from, float yaw, std::optional<glm::vec3> at, bool lob, float speed) {
    if (!at) return speed * glm::vec3(std::sin(yaw) * std::cos(gRanged.hipPitch), std::sin(gRanged.hipPitch), std::cos(yaw) * std::cos(gRanged.hipPitch));
    glm::vec3 d = *at - from;
    float x = glm::length(glm::vec2(d.x, d.z));
    if (!lob || x < 1e-3f || gRanged.gravity < 1e-4f) return glm::length(d) > 1e-4f ? glm::normalize(d) * speed : glm::vec3(0, 0, speed);
    // The low arc through `at`: tan(angle) = (v^2 - sqrt(v^4 - g (g x^2 + 2 y v^2))) / (g x); out of reach, 45 degrees.
    float g = gRanged.gravity, v2 = speed * speed, disc = v2 * v2 - g * (g * x * x + 2 * d.y * v2);
    float angle = disc >= 0 ? std::atan((v2 - std::sqrt(disc)) / (g * x)) : std::numbers::pi_v<float> / 4;
    glm::vec2 h = glm::vec2(d.x, d.z) / x;
    return speed * glm::vec3(h.x * std::cos(angle), std::sin(angle), h.y * std::cos(angle));
}

std::optional<float> segmentBox(glm::vec3 a, glm::vec3 b, glm::vec3 lo, glm::vec3 hi) {
    float enter = 0, leave = 1;  // the slab method
    glm::vec3 d = b - a;
    for (int k = 0; k < 3; ++k) {
        if (std::abs(d[k]) < 1e-9f) {
            if (a[k] < lo[k] || a[k] > hi[k]) return std::nullopt;
            continue;
        }
        float t0 = (lo[k] - a[k]) / d[k], t1 = (hi[k] - a[k]) / d[k];
        if (t0 > t1) std::swap(t0, t1);
        enter = std::max(enter, t0), leave = std::min(leave, t1);
        if (enter > leave) return std::nullopt;
    }
    return enter;
}

Box3 hitBox(const Entity& e) {
    Model m = e.type().model;
    float h = m == ModelBoar || m == ModelWolf ? 1.1f : m == ModelChief ? 2.0f : kHeight, r = gRanged.hitRadius;
    return {e.body.pos - glm::vec3(r, 0, r), e.body.pos + glm::vec3(r, h, r)};
}

void loose(Session& s, std::optional<glm::vec3> aimAt) {
    Player& p = s.player;
    if (p.arrows <= 0) return;
    --p.arrows;
    float draw = drawOf(p), k = gRanged.minPower + (1 - gRanged.minPower) * draw;
    const MoveSpec& m = spec(p, Move::Shot);  // (a hit that made the player flinch this tick ended the move already)
    glm::vec3 from = shotOrigin(p);
    Projectile a{from, from, shotVelocity(from, p.yaw, aimAt, !p.aiming, gRanged.minSpeed + (gRanged.maxSpeed - gRanged.minSpeed) * draw),
                 p.swingWeapon, m.damage * k, m.stance * k};
    if (s.projectiles.size() >= kMaxProjectiles) {  // the oldest stuck one goes, else the oldest
        auto old = std::ranges::find_if(s.projectiles, &Projectile::stuck);
        s.projectiles.erase(old != s.projectiles.end() ? old : s.projectiles.begin());
    }
    s.projectiles.push_back(a);
    p.noise(Noise::Loose);
}

void updateProjectiles(Session& s, float dt) {
    Player& p = s.player;
    for (size_t i = 0; i < s.projectiles.size();) {
        Projectile& a = s.projectiles[i];
        a.prevPos = a.pos;
        a.age += dt;
        bool gone = false;
        if (a.stuck) {  // walked over: back in the quiver
            if (p.alive() && p.arrows < kQuiver && glm::distance(a.pos, p.pos + glm::vec3(0, 0.9f, 0)) < gRanged.pickupReach)
                ++p.arrows, p.noise(Noise::Loot), gone = true;
            else gone = a.age >= gRanged.stuckSeconds;
        } else if (s.loaded && !s.loaded(a.pos)) {
            gone = true;  // flew out of the world that's in
        } else {
            a.vel.y -= gRanged.gravity * dt;
            glm::vec3 from = a.pos, step = a.vel * dt;
            float length = glm::length(step), best = length;
            glm::vec3 dir = step / std::max(length, 1e-6f);
            std::optional<RayHit> wall = raycast(s.world, from, dir, length);
            if (wall) best = wall->distance;
            Entity* hit = nullptr;
            for (Entity& e : s.entities) {  // a dodge's i-frames let it through (ADR-043)
                if (!e.type().attackable || !e.body.alive() || invulnerable(e.body)) continue;
                Box3 box = hitBox(e);
                if (std::optional<float> t = segmentBox(from, from + step, box.lo, box.hi); t && *t * length < best) best = *t * length, hit = &e;
            }
            if (hit) {  // through the one hit path, as with the bow's own stats (its swing weapon, for the moment)
                ItemId held = p.swingWeapon;
                p.swingWeapon = a.weapon;
                glm::vec2 flat(dir.x, dir.z);  // where it came from, level (a guard faces that way; straight down: any)
                if (glm::length(flat) > 1e-3f) flat = glm::normalize(flat) * 3.0f;
                Strike st{a.power, a.stance, from + dir * best - glm::vec3(flat.x, 0, flat.y), itemType(a.weapon).damage, false, true, false};
                strike(p, *hit, st, s.rng, s.armorK, &s.events);
                p.swingWeapon = held;
                gone = true;
            } else if (wall) {  // in where it went: lies there
                a.pos = from + dir * best;
                a.stuck = true, a.age = 0;
                if (s.sounds.size() < kMaxSounds) s.sounds.push_back({Noise::Thunk, a.pos, -1, s.world.get(wall->cell), Move::None, a.weapon, 0});
            } else {
                a.pos = from + step;
                gone = a.age >= gRanged.flightSeconds || a.pos.y < 0;
            }
        }
        if (gone) s.projectiles.erase(s.projectiles.begin() + std::ptrdiff_t(i));
        else ++i;
    }
}
