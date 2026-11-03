#include "entity.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

#include "defs.h"

namespace {

constexpr float kTau = 2 * std::numbers::pi_v<float>;
constexpr float kSpeechSeconds = 5;
constexpr float kShoutSeconds = 8;  // at least this long between shouts
constexpr float kBodyRadius = 0.8f;  // player-to-entity distance kept by separate()

constexpr const char* kNames[] = {
    "Alda", "Bram", "Corin", "Dessa", "Edric", "Fenna", "Garrick", "Hilde", "Ivo", "Juna",
    "Keld", "Liora", "Maren", "Nils", "Orla", "Perrin", "Rosk", "Sabine", "Tamsin", "Wendel",
};

constexpr const char* kPhrases[] = {
    "Lovely weather today, isn't it?",
    "Safe travels, stranger.",
    "The harvest was good this year.",
    "Have you seen my goat? She wandered off again.",
    "I've lived here all my life. Never been past those hills.",
    "The river is cold this time of year.",
    "They say there's something shiny up in the rocky lands.",
    "Go easy on the dummy, it's the only one we have.",
    "Mind the dark. Strange things walk at night.",
    "Good day to you!",
};

glm::vec2 xz(glm::vec3 v) { return {v.x, v.z}; }

float rand01(std::mt19937& rng) { return std::uniform_real_distribution<float>(0, 1)(rng); }

// Can a body standing at `feet` take a step along `dir`? No walls it can't step up (including no
// headroom above it for the step), no water, no drops of 2+ blocks, and not into the player.
bool walkable(const Grid& g, glm::vec3 feet, glm::vec2 dir, glm::vec3 player) {
    glm::ivec3 ahead(glm::floor(feet + glm::vec3(dir.x, 0, dir.y) * 0.9f));
    auto at = [&](int dy) { return g.get(ahead + glm::ivec3(0, dy, 0)); };
    bool step = isOpaque(at(0));
    bool wall = isOpaque(at(1)) || (step && (isOpaque(at(2)) || isOpaque(g.get(glm::ivec3(glm::floor(feet)) + glm::ivec3(0, 2, 0)))));
    bool water = at(0) == Water || at(-1) == Water || (step && at(1) == Water);
    bool cliff = !isOpaque(at(0)) && !isOpaque(at(-1)) && !isOpaque(at(-2));
    glm::vec2 toPlayer = xz(player - feet);
    bool blocked = glm::length(toPlayer) < kBodyRadius + 0.9f && glm::dot(toPlayer, dir) > 0;
    return !wall && !water && !cliff && !blocked;
}

void turnToward(Player& body, glm::vec2 dir, float dt) {
    float diff = std::remainder(std::atan2(dir.x, dir.y) - body.yaw, kTau);
    body.yaw = std::remainder(body.yaw + diff * (1 - std::exp(-8 * dt)), kTau);
}

void wander(Entity& e, const Grid& g, glm::vec3 player, float dt) {
    e.wanderTimer -= dt;
    if (e.wanderTimer <= 0) {  // next decision: stand around for a while, or stroll somewhere
        if (rand01(e.rng) < 0.4f) {
            e.wanderDir = {};
            e.wanderTimer = 2 + 4 * rand01(e.rng);
        } else {
            float a = kTau * rand01(e.rng);
            e.wanderDir = {std::sin(a), std::cos(a)};
            e.wanderTimer = 2 + 3 * rand01(e.rng);
        }
    }
    glm::vec2 toHome = xz(e.home - e.body.pos);
    if (glm::length(toHome) > e.wanderRadius && glm::dot(e.wanderDir, toHome) <= 0)
        e.wanderDir = glm::normalize(toHome);  // strayed (or was chasing someone): head back
    if (e.wanderDir != glm::vec2(0) && !walkable(g, e.body.pos, e.wanderDir, player)) {
        e.wanderDir = {};  // stop and think again soon
        e.wanderTimer = std::min(e.wanderTimer, 1.0f);
    }
}

// Turns a fighter on the player; one with spawn marker lines shouts the next one (not more often
// than every kShoutSeconds, e.g. when the player hits it from outside its territory, where it can't stay angry).
void anger(Entity& e) {
    if (!e.angry && e.body.alive() && !e.spawn.lines.empty() && e.shoutTimer <= 0) {
        talk(e);
        e.shoutTimer = std::max(kShoutSeconds, e.speechTimer);
    }
    e.angry = true;
}

// A new entity of `kind` standing at (and at home on) `pos`, facing `face`.
Entity makeEntity(EntityKind kind, uint32_t rngSeed, glm::vec3 pos, glm::vec3 face) {
    Entity e{};
    e.kind = kind;
    e.rng.seed(rngSeed);
    const EntityType& t = e.type();
    e.name = t.name ? t.name : kNames[e.rng() % std::size(kNames)];
    e.level = t.level;
    e.wanderRadius = t.wanderRadius;
    e.body.stats = t.stats;
    e.body.health = float(maxHealth(t.stats));
    e.body.tuning.walkSpeed = 2;  // a stroll
    e.body.tuning.runSpeed = t.chaseSpeed;
    e.body.inv.equipped[SlotMainHand] = t.weapon;
    e.body.breathPhase = kTau * rand01(e.rng);
    glm::vec2 toFace = xz(face) - xz(pos);
    if (glm::length(toFace) > 0.01f) e.body.yaw = std::atan2(toFace.x, toFace.y);
    e.body.teleport(e.home = pos);
    return e;
}

}  // namespace

const EntityType& typeOf(EntityKind kind) { return defs().creatures[size_t(kind)]; }

const char* Entity::speech() const {
    if (speechTimer <= 0 || phrase < 0) return nullptr;
    if (spawn.lines.empty()) return kPhrases[phrase];
    return size_t(phrase) < spawn.lines.size() ? spawn.lines[size_t(phrase)].c_str() : nullptr;
}

const std::string& Entity::displayName() const {
    return !spawn.lines.empty() && spawn.count == 1 && !spawn.name.empty() ? spawn.name : name;
}

std::vector<Entity> spawnEntities(const Grid& world, glm::vec3 near, uint32_t seed) {
    std::vector<Entity> out;
    auto add = [&](EntityKind kind, glm::vec2 offset) {
        // Nearest grass to the offset, but not on top of the player or an earlier entity.
        auto crowded = [&](glm::vec3 p) {
            bool c = glm::distance(xz(p), xz(near)) < 1.5f;
            for (const Entity& o : out) c |= glm::distance(xz(p), xz(o.body.pos)) < 1.5f;
            return c;
        };
        glm::vec3 pos;
        for (int tries = 0; tries < 8; ++tries, offset.x += 2) {
            pos = findSpawn(world, glm::ivec2(glm::floor(xz(near) + offset)));
            if (!crowded(pos)) break;
        }
        out.push_back(makeEntity(kind, seed * 2654435761u + uint32_t(out.size()), pos, near));
    };
    // In front of the starting camera, which looks toward +Z. The bandit's camp is over 20 blocks
    // out: beyond its leash from the spawn, and even wandering it stays out of aggro range.
    add(EntityKind::TrainingDummy, {3, 6});
    add(EntityKind::Villager, {-4, 7});
    add(EntityKind::Boar, {9, 11});
    add(EntityKind::Bandit, {-5, 22});
    return out;
}

// The entities of spawn marker `m`: the first on the marker, the others on a small spiral around it,
// each on the first dry ground within a few blocks of the marker's height (else on the marker).
static void spawnAt(std::vector<Entity>& out, const Grid& world, const Marker& m, uint32_t seed, glm::vec3 face) {
    for (int i = 0; i < m.count; ++i) {
        glm::vec3 pos = clearOfTerrain(world, m.pos);
        glm::vec2 at = xz(m.pos) + (i ? 1.6f * std::sqrt(float(i)) * glm::vec2(std::cos(2.4f * float(i)), std::sin(2.4f * float(i))) : glm::vec2(0));
        glm::ivec2 c(glm::floor(at));
        for (int y = int(m.pos.y) + 3; i && y > int(m.pos.y) - 4; --y) {
            auto block = [&](int dy) { return world.get({c.x, y + dy, c.y}); };
            if (isOpaque(block(-1)) && block(0) == Air && block(1) == Air) {
                pos = {float(c.x) + 0.5f, float(y), float(c.y) + 0.5f};
                break;
            }
        }
        // Seeded like spawnEntities for a marker's first entity, so the default markers keep the old names.
        Entity e = makeEntity(m.entity, seed * 2654435761u + (m.id - 1) + uint32_t(i) * 0x9E3779B9u, pos, face);
        e.wanderRadius = m.radius;
        e.spawn = m;
        out.push_back(std::move(e));
    }
}

void syncSpawns(std::vector<Entity>& entities, const Grid& world, const std::vector<Marker>& markers, uint32_t seed,
                glm::vec3 face) {
    auto spawnOf = [&](uint32_t id) -> const Marker* {
        auto it = std::ranges::lower_bound(markers, id, {}, &Marker::id);
        return it != markers.end() && it->id == id && it->kind == MarkerKind::Spawn ? &*it : nullptr;
    };
    // A marker respawns all of its entities when any is out of date or their number is off.
    std::unordered_map<uint32_t, int> counts;
    std::unordered_set<uint32_t> stale;
    for (Entity& e : entities) {
        const Marker* m = spawnOf(e.spawn.id);
        if (m && m->lines != e.spawn.lines) e.phrase = -1, e.speechTimer = 0;  // new lines start from the first
        if (m) e.spawn.name = m->name, e.spawn.lines = m->lines;  // a rename or new lines need no respawn
        if (!m || m->pos != e.spawn.pos || m->radius != e.spawn.radius || m->entity != e.spawn.entity || m->count != e.spawn.count)
            stale.insert(e.spawn.id);
        ++counts[e.spawn.id];
    }
    for (const Marker& m : markers)
        if (m.kind == MarkerKind::Spawn && counts[m.id] != m.count) stale.insert(m.id);
    if (stale.empty()) return;
    std::erase_if(entities, [&](const Entity& e) { return stale.contains(e.spawn.id); });
    for (const Marker& m : markers)
        if (m.kind == MarkerKind::Spawn && stale.contains(m.id)) spawnAt(entities, world, m, seed, face);
}

glm::vec3 playerSpawn(const Grid& world, const std::vector<Marker>& markers) {
    auto it = std::ranges::find(markers, MarkerKind::PlayerSpawn, &Marker::kind);
    return clearOfTerrain(world, it != markers.end() ? it->pos : findSpawn(world));
}

std::vector<Marker> defaultMarkers(const Grid& world, uint32_t seed) {
    std::vector<Marker> out;
    for (const Entity& e : spawnEntities(world, findSpawn(world), seed))
        out.push_back({.id = uint32_t(out.size() + 1), .kind = MarkerKind::Spawn, .name = e.type().name ? e.type().name : "Villager",
                       .pos = e.home, .radius = e.wanderRadius, .entity = e.kind, .count = 1});
    return out;
}

void updateEntities(std::vector<Entity>& entities, const Grid& world, const Player& player, float dt) {
    for (Entity& e : entities) {
        e.speechTimer = std::max(0.0f, e.speechTimer - dt);
        e.shoutTimer = std::max(0.0f, e.shoutTimer - dt);
        e.body.pos = clearOfTerrain(world, e.body.pos);  // e.g. respawned at a home that was built over since
        const EntityType& t = e.type();
        if (!t.wanders) continue;
        Input in;
        glm::vec2 toPlayer = xz(player.pos - e.body.pos);
        float dist = glm::length(toPlayer);
        glm::vec2 dir = dist > 1e-3f ? toPlayer / dist : glm::vec2(0, 1);
        // Fighters only care about a living player inside their territory.
        bool inTerritory = player.alive() && glm::distance(xz(player.pos), xz(e.home)) < kLeashRadius;
        if (t.temper == Temper::Aggressive && inTerritory && dist < t.aggroRange) anger(e);
        if (!inTerritory || !e.body.alive()) e.angry = false;
        if (!e.body.alive()) {
            // lies where it fell
        } else if (e.angry) {  // run at the player until in reach, then swing
            if (walkable(world, e.body.pos, dir, player.pos)) {
                in.move = dir;
                in.run = true;
            } else if (!e.body.punching()) {  // ponytail: no pathfinding; it waits until the player comes back or leaves
                turnToward(e.body, dir, dt);
            }
            if (dist < kPunchReach - 0.2f) startPunch(e.body, dir);
        } else if (e.speechTimer > 0 && e.shoutTimer == 0) {  // stands still and faces the player while talking (not after a shout)
            if (dist > 0.01f) turnToward(e.body, toPlayer, dt);
        } else {
            wander(e, world, player.pos, dt);
            in.move = e.wanderDir;
        }
        updatePlayer(e.body, world, in, dt);
    }
}

void separate(Player& player, const Grid& world, const std::vector<Entity>& entities) {
    if (player.tuning.noclip) return;
    for (const Entity& e : entities) {
        glm::vec2 d = xz(player.pos - e.body.pos);
        float len = glm::length(d);
        bool overlapY = player.pos.y < e.body.pos.y + kHeight && player.pos.y + kHeight > e.body.pos.y;
        if (len >= kBodyRadius || !overlapY) continue;
        glm::vec2 n = len > 1e-4f ? d / len : glm::vec2(1, 0);
        glm::vec3 next = player.pos + glm::vec3(n.x, 0, n.y) * (kBodyRadius - len);
        if (!overlapsSolid(world, next)) player.pos = next;
    }
}

Entity* talkTarget(std::vector<Entity>& entities, const Player& player) {
    Entity* best = nullptr;
    float bestDist = kTalkRange;
    for (Entity& e : entities) {
        float d = glm::distance(e.body.pos, player.pos);
        if (e.type().talks && e.body.alive() && d < bestDist) {
            best = &e;
            bestDist = d;
        }
    }
    return best;
}

void talk(Entity& e) {
    e.shoutTimer = 0;  // spoken to: it listens (anger sets it again after a shout)
    if (!e.spawn.lines.empty()) {  // the marker's lines, in order and round again
        e.phrase = int(size_t(e.phrase + 1) % e.spawn.lines.size());
        e.speechTimer = std::max(kSpeechSeconds, float(e.spawn.lines[size_t(e.phrase)].size()) / 18);  // long lines stay longer
        return;
    }
    int n = int(std::size(kPhrases)), p;
    do p = int(e.rng() % unsigned(n));
    while (p == e.phrase);
    e.phrase = p;
    e.speechTimer = kSpeechSeconds;
}

Entity* lootTarget(std::vector<Entity>& entities, const Player& player) {
    if (!player.alive() || freeSlots(player.inv) == 0) return nullptr;  // E talks instead
    Entity* best = nullptr;
    float bestDist = kTalkRange;
    for (Entity& e : entities) {
        float d = glm::distance(e.body.pos, player.pos);
        if (!e.body.alive() && e.loot != NoItem && d < bestDist) {
            best = &e;
            bestDist = d;
        }
    }
    return best;
}

bool takeLoot(Entity& e, Inventory& inv) {
    if (!giveItem(inv, e.loot)) return false;
    e.loot = NoItem;
    return true;
}

bool hurt(Player& b, int amount) {
    if (amount <= 0 || !b.alive()) return false;
    b.health = std::max(0.0f, b.health - float(amount));
    b.lastDamage = amount;
    b.hitTime = 0;
    b.combatTimer = kCombatSeconds;
    return true;
}

bool damage(Entity& e, int amount) {
    const EntityType& t = e.type();
    if (!t.attackable || !hurt(e.body, amount)) return false;
    if (t.temper != Temper::Harmless) anger(e);
    if (!e.body.alive() && (t.lootChance >= 1 || rand01(e.rng) < t.lootChance)) e.loot = t.loot;  // rand01 may return 1
    return true;
}

bool startPunch(Player& b, glm::vec2 aim) {
    if (!b.alive() || b.swingTimer > 0) return false;
    b.swingWeapon = b.inv.equipped[SlotMainHand];
    b.swingTimer += swingTime(b.stats, weaponAttack(b.swingWeapon));  // keeps the overshoot of the last frame
    b.punchLength = std::min(kPunchSeconds, b.swingTimer);
    b.punchTime = 0;
    b.punchRight = !b.punchRight;
    b.hitPending = true;
    if (glm::length(aim) > 1e-4f) b.yaw = std::atan2(aim.x, aim.y);
    return true;
}

bool inReach(const Player& a, glm::vec3 target) {
    glm::vec2 forward{std::sin(a.yaw), std::cos(a.yaw)};
    glm::vec2 d = xz(target - a.pos);
    float dist = glm::length(d);
    bool inFront = dist < 1e-3f || glm::dot(d / dist, forward) > 0.5f;  // within 60 degrees
    return inFront && dist < kPunchReach && std::abs(target.y - a.pos.y) < kHeight;
}

Entity* punchTarget(std::vector<Entity>& entities, const Player& a) {
    Entity* best = nullptr;
    float bestDist = kPunchReach;
    for (Entity& e : entities) {
        float dist = glm::distance(xz(e.body.pos), xz(a.pos));
        if (e.type().attackable && e.body.alive() && inReach(a, e.body.pos) && dist < bestDist) {
            best = &e;
            bestDist = dist;
        }
    }
    return best;
}

static void tick(Player& b, float dt) {
    // May go up to one frame negative, so back-to-back swings don't lose time to frame rounding.
    b.swingTimer = std::max(b.swingTimer - dt, -dt);
    b.punchTime += dt;
    b.hitTime += dt;
    b.combatTimer = std::max(0.0f, b.combatTimer - dt);
    if (!b.alive()) b.deadTime += dt;
    else if (b.combatTimer == 0) b.health += healthRegen(b.stats) * dt;
    b.health = std::min(b.health, float(maxHealth(b.stats)));
}

// True once per swing, at its impact frame. A swing dies with its swinger.
static bool lands(Player& b) {
    if (!b.alive()) b.hitPending = false;
    if (!b.hitPending || b.punchTime < kPunchImpact * b.punchLength) return false;
    b.hitPending = false;
    return true;
}

// A swing's damage after the victim's armor, which depends on the attacker's level.
static int roll(const Player& b, int level, const Player& victim, float armorK, std::mt19937& rng) {
    return mitigate(rollDamage(b.stats, weaponAttack(b.swingWeapon), rng), armor(victim.inv), level, armorK);
}

void updateCombat(Player& player, std::vector<Entity>& entities, float dt, std::mt19937& rng, float armorK) {
    tick(player, dt);
    if (lands(player)) {  // the player has no level yet: 1
        Entity* target = punchTarget(entities, player);
        if (target && damage(*target, roll(player, 1, target->body, armorK, rng))) player.combatTimer = kCombatSeconds;
    }
    for (Entity& e : entities) {
        tick(e.body, dt);
        if (lands(e.body) && inReach(e.body, player.pos) && hurt(player, roll(e.body, e.level, player, armorK, rng)))
            e.body.combatTimer = kCombatSeconds;
        if (!e.body.alive() && e.body.deadTime >= e.type().respawnSeconds) {  // back home, fully healed
            e.body.teleport(e.home);
            e.body.vel = {};
            e.body.health = float(maxHealth(e.body.stats));
            e.body.deadTime = 0;
            e.body.combatTimer = 0;
            e.loot = NoItem;
            e.wanderDir = {};
        }
    }
}
