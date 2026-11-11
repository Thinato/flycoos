#include "projectile.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

#include "defs.h"
#include "edit.h"
#include "melee.h"
#include "session.h"

float drawOf(const Player& p) { return std::clamp(p.charge / gRanged.drawSeconds, 0.0f, 1.0f); }

glm::vec3 shotOrigin(const Player& p) {
    return p.pos + glm::vec3(0.5f * std::sin(p.yaw), 1.35f, 0.5f * std::cos(p.yaw));
}

glm::vec3 shotVelocity(glm::vec3 from, float yaw, std::optional<glm::vec3> at, bool lob, float speed, float pitch) {
    if (!at) return speed * glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
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
    float h = bodyHeight(e.type().model), r = gRanged.hitRadius;
    return {e.body.pos - glm::vec3(r, 0, r), e.body.pos + glm::vec3(r, h, r)};
}

// A projectile into s.projectiles: past the cap the oldest stuck one goes, else the oldest.
static void launch(Session& s, const Projectile& a) {
    if (s.projectiles.size() >= kMaxProjectiles) {
        auto old = std::ranges::find_if(s.projectiles, &Projectile::stuck);
        s.projectiles.erase(old != s.projectiles.end() ? old : s.projectiles.begin());
    }
    s.projectiles.push_back(a);
}

// An arrow off `p`'s bow by its draw, at `at` (none: along its facing), lobbed or straight; `owner` as Projectile's.
static void shoot(Session& s, Player& p, uint32_t owner, std::optional<glm::vec3> at, bool lob) {
    bool bolt = weaponClass(classOf(p.swingWeapon)).loads;  // a crossbow's (ADR-052): always full power, faster
    float draw = bolt ? 1 : drawOf(p), k = gRanged.minPower + (1 - gRanged.minPower) * draw;
    const MoveSpec& m = spec(p, Move::Shot);  // (a hit that made the shooter flinch this tick ended the move already)
    glm::vec3 from = shotOrigin(p);
    Projectile a{from, from, shotVelocity(from, p.yaw, at, lob, bolt ? gRanged.boltSpeed : gRanged.minSpeed + (gRanged.maxSpeed - gRanged.minSpeed) * draw),
                 p.swingWeapon, m.damage * k, m.stance * k};
    a.owner = owner;
    launch(s, a);
    p.noise(Noise::Loose);
}

void loose(Session& s, std::optional<glm::vec3> aimAt) {
    Player& p = s.player;
    if (weaponClass(classOf(p.swingWeapon)).loads) {  // a crossbow spends the bolt in it (ADR-052; the reload took it from the quiver)
        if (p.loaded != p.swingWeapon) return;
        p.loaded = NoItem;
    } else {
        if (p.arrows <= 0) return;
        --p.arrows;
    }
    shoot(s, p, 0, aimAt, !p.aiming);
}

void loose(Session& s, Entity& e) {
    const Player& target = s.player;
    std::uniform_real_distribution<float> off(-e.type().aimError, e.type().aimError);
    float yaw = e.body.yaw + off(e.rng), pitch = off(e.rng);
    float dist = std::max(glm::length(glm::vec2(target.pos.x - e.body.pos.x, target.pos.z - e.body.pos.z)), 1.0f);  // (right overhead: still ahead)
    glm::vec3 at = e.body.pos + glm::vec3(std::sin(yaw), 0, std::cos(yaw)) * dist;
    at.y = target.pos.y + 1.1f + dist * std::tan(pitch);  // its chest
    shoot(s, e.body, e.id, at, true);
}

void fillPouch(Combatant& c) {
    const std::vector<ItemType>& items = defs().items;
    for (size_t i = 0; i < items.size(); ++i)
        if (throwable(items[i])) {
            c.pouch[i] = uint8_t(std::max(int(c.pouch[i]), items[i].thrown.pouch));
            if (!c.readied) c.readied = ItemId(i);
        }
}

void cycleThrowable(Combatant& c) {
    const std::vector<ItemType>& items = defs().items;
    bool any = false;
    for (size_t i = 0; i < items.size(); ++i) any |= throwable(items[i]) && c.pouch[i] > 0;
    for (size_t k = 1; k <= items.size(); ++k)
        if (size_t i = (size_t(c.readied) + k) % items.size(); throwable(items[i]) && (!any || c.pouch[i] > 0)) {
            c.readied = ItemId(i);
            return;
        }
}

void hurl(Session& s, std::optional<glm::vec3> aimAt) {
    Player& p = s.player;
    ItemId item = p.throwing;
    p.throwing = NoItem;
    if (!item || !throwable(itemType(item)) || p.pouch[item] == 0) return;
    --p.pouch[item];
    const Throwable& t = itemType(item).thrown;
    glm::vec3 from = shotOrigin(p);
    Projectile a{from, from, shotVelocity(from, p.yaw, aimAt, true, t.speed, t.lob ? gRanged.lobPitch : gRanged.hipPitch),
                 item, 1, t.stance};
    launch(s, a);
}

void burst(Session& s, glm::vec3 at, ItemId item, glm::vec3 from) {
    Player& p = s.player;
    const ItemType& it = itemType(item);
    float r = it.thrown.radius;
    for (Entity& e : s.entities) {  // a dodge's i-frames let it through, as an arrow (ADR-043)
        if (!e.type().attackable || !e.body.alive() || invulnerable(e.body)) continue;
        Box3 box = hitBox(e);
        float d = glm::distance(at, glm::clamp(at, box.lo, box.hi));
        glm::vec3 to = (box.lo + box.hi) * 0.5f - at;
        if (d > r || (glm::length(to) > 1e-3f && raycast(s.world, at, glm::normalize(to), glm::length(to)))) continue;  // out of reach, or behind a wall
        float k = 1 - (1 - gRanged.burstEdge) * d / r;
        bool over = glm::length(glm::vec2(at.x - e.body.pos.x, at.z - e.body.pos.z)) < 0.5f;  // right on it: it came from where it flew in from
        Strike st{k, it.thrown.stance * k, over ? from : at, it.damage, false, true, false};
        st.buildup.emplace();
        for (size_t i = 0; i < st.buildup->size(); ++i) (*st.buildup)[i] = it.thrown.buildup[i] * k;
        ItemId held = p.swingWeapon;  // the throwable's own stats, through the one hit path
        p.swingWeapon = item;
        strike(p, e, st, s.rng, s.armorK, &s.events);
        p.swingWeapon = held;
    }
    if (it.thrown.look == ThrowLook::Bomb)  // a blast nearby shakes the camera (ADR-045)
        p.trauma = std::min(1.0f, p.trauma + 0.4f * std::clamp(1 - glm::distance(at, p.pos) / 16, 0.0f, 1.0f));
    s.bursts.push_back({at, r, item, 0});
    if (s.sounds.size() < kMaxSounds) s.sounds.push_back({Noise::Thunk, at, -1, Air, Move::None, item, 0, at, false});
}

void updateProjectiles(Session& s, float dt) {
    Player& p = s.player;
    for (Burst& b : s.bursts) b.age += dt;
    std::erase_if(s.bursts, [](const Burst& b) { return b.age >= gRanged.burstSeconds; });
    for (size_t i = 0; i < s.projectiles.size();) {
        Projectile& a = s.projectiles[i];
        a.prevPos = a.pos;
        a.age += dt;
        bool gone = false;
        const ItemType& item = itemType(a.weapon);
        bool thrown = throwable(item), bursts = item.thrown.radius > 0;  // a throwing knife back in the pouch (ADR-053)
        if (a.stuck) {  // walked over: back in the quiver
            bool room = thrown ? p.pouch[a.weapon] < item.thrown.pouch : p.arrows < kQuiver;
            if (p.alive() && room && glm::distance(a.pos, p.pos + glm::vec3(0, 0.9f, 0)) < gRanged.pickupReach) {
                if (thrown) ++p.pouch[a.weapon];
                else ++p.arrows;
                p.noise(Noise::Loot), gone = true;
            } else {
                gone = a.age >= gRanged.stuckSeconds;
            }
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
                if (a.owner || !e.type().attackable || !e.body.alive() || invulnerable(e.body)) continue;
                Box3 box = hitBox(e);
                if (std::optional<float> t = segmentBox(from, from + step, box.lo, box.hi); t && *t * length < best) best = *t * length, hit = &e;
            }
            auto owner = a.owner ? std::ranges::find(s.entities, a.owner, &Entity::id) : s.entities.end();
            Entity* shooter = owner != s.entities.end() ? &*owner : nullptr;  // an archer's arrow (ADR-051): at the player
            bool player = false;
            if (shooter && p.alive() && !invulnerable(p)) {  // (a roll's i-frames let it through)
                glm::vec3 r(gRanged.hitRadius, 0, gRanged.hitRadius);
                if (std::optional<float> t = segmentBox(from, from + step, p.pos - r, p.pos + r + glm::vec3(0, kHeight, 0)); t && *t * length < best)
                    best = *t * length, player = true;
            }
            glm::vec2 flat(dir.x, dir.z);  // where it came from, level (a guard faces that way; straight down: any)
            if (glm::length(flat) > 1e-3f) flat = glm::normalize(flat) * 3.0f;
            glm::vec3 source = from + dir * best - glm::vec3(flat.x, 0, flat.y);
            if (player) {  // with the archer's stats and the bow it loosed
                ItemId held = shooter->body.swingWeapon;
                shooter->body.swingWeapon = a.weapon;
                strikePlayer(*shooter, p, {a.power, a.stance, source, itemType(a.weapon).damage, false, true, false}, s.rng, s.armorK);
                shooter->body.swingWeapon = held;
                gone = true;
            } else if (bursts && (hit || wall)) {  // a bomb or a pot (ADR-053): bursts on the first thing it meets, a little short of it
                burst(s, from + dir * std::max(best - 0.05f, 0.0f), a.weapon, source);
                gone = true;
            } else if (hit) {  // through the one hit path, as with the bow's own stats (its swing weapon, for the moment)
                ItemId held = p.swingWeapon;
                p.swingWeapon = a.weapon;
                Strike st{a.power, a.stance, source, item.damage, false, true, false};
                if (thrown) st.buildup = item.thrown.buildup;  // a throwing knife's own bleed (ADR-053), not the weapon in hand's
                strike(p, *hit, st, s.rng, s.armorK, &s.events);
                p.swingWeapon = held;
                gone = true;
            } else if (wall) {  // in where it went: lies there
                a.pos = from + dir * best;
                a.stuck = true, a.age = 0;
                if (s.sounds.size() < kMaxSounds) s.sounds.push_back({Noise::Thunk, a.pos, -1, s.world.get(wall->cell), Move::None, a.weapon, 0, a.pos - dir, false});
            } else {
                a.pos = from + step;
                gone = a.age >= gRanged.flightSeconds || a.pos.y < 0;
            }
        }
        if (gone) s.projectiles.erase(s.projectiles.begin() + std::ptrdiff_t(i));
        else ++i;
    }
}
