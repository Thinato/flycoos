#include "entity.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iterator>
#include <numbers>
#include <string_view>

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

// A new entity of `kind` standing at (and at home on) `pos`, facing `face`.
Entity makeEntity(EntityKind kind, uint32_t rngSeed, glm::vec3 pos, glm::vec3 face) {
    Entity e{};
    e.kind = kind;
    e.rng.seed(rngSeed);
    const EntityType& t = e.type();
    e.name = t.name ? t.name : kNames[e.rng() % std::size(kNames)];
    e.wanderRadius = t.wanderRadius;
    e.body.stats = t.stats;
    e.body.level = t.level;
    e.body.tuning.walkSpeed = 2;  // a stroll
    e.body.tuning.runSpeed = t.chaseSpeed;
    e.body.inv.equipped[SlotMainHand] = t.weapon;
    e.body.health = float(maxHealth(sheet(e.body)));
    e.body.breathPhase = kTau * rand01(e.rng);
    glm::vec2 toFace = xz(face) - xz(pos);
    if (glm::length(toFace) > 0.01f) e.body.yaw = std::atan2(toFace.x, toFace.y);
    e.body.teleport(e.home = pos);
    e.state = t.wanders ? AiState::Wander : AiState::Idle;
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

// The entities of spawn marker `m` in the slots of bit mask `slots`: the first on the marker, the
// others on a small spiral around it, each on the first dry ground within a few blocks of the
// marker's height (else on the marker).
static void spawnAt(std::vector<Entity>& out, const Grid& world, const Marker& m, uint32_t seed, glm::vec3 face, uint32_t slots) {
    for (int i = 0; i < m.count; ++i) {
        if (!(slots >> i & 1)) continue;
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
        e.slot = i;
        out.push_back(std::move(e));
    }
}

void syncSpawns(std::vector<Entity>& entities, const Grid& world, const std::vector<Marker>& markers, uint32_t seed,
                glm::vec3 face, float timeOfDay) {
    // Runs every tick in worlds with timed markers: no hashing, one small buffer, no copies of unchanged lines.
    auto spawnOf = [&](uint32_t id) -> ptrdiff_t {  // the index of spawn marker `id`, or -1
        auto it = std::ranges::lower_bound(markers, id, {}, &Marker::id);
        return it != markers.end() && it->id == id && it->kind == MarkerKind::Spawn ? it - markers.begin() : -1;
    };
    // Outside its marker's window an NPC leaves once it isn't fighting, a corpse once looted (after
    // kCorpseLinger, so a kill shows) or back up.
    std::erase_if(entities, [&](const Entity& e) {
        ptrdiff_t i = spawnOf(e.spawn.id);
        return i >= 0 && !activeAt(markers[size_t(i)], timeOfDay) && !e.angry() &&
               (e.body.alive() || (e.loot == NoItem && e.body.deadTime >= kCorpseLinger));
    });
    // A marker respawns all of its entities when any is out of date, and spawns the slots it misses
    // (a window opening, a removed marker undone) while active, keeping the rest as they are.
    struct Tally {
        uint32_t slots = 0;  // bit per slot present
        bool stale = false;
    };
    std::vector<Tally> tally(markers.size());
    bool any = false;  // something to erase or spawn
    for (Entity& e : entities) {
        ptrdiff_t i = spawnOf(e.spawn.id);
        if (i < 0) {  // its marker is gone
            any = true;
            continue;
        }
        const Marker& m = markers[size_t(i)];
        if (m.lines != e.spawn.lines) e.phrase = -1, e.speechTimer = 0, e.spawn.lines = m.lines;  // new lines start from the first
        if (m.name != e.spawn.name) e.spawn.name = m.name;  // a rename, new lines or a new window need no respawn
        e.spawn.activeFrom = m.activeFrom, e.spawn.activeTo = m.activeTo;
        Tally& t = tally[size_t(i)];
        uint32_t bit = e.slot >= 0 && e.slot < m.count ? 1u << e.slot : 0;
        if (m.pos != e.spawn.pos || m.radius != e.spawn.radius || m.entity != e.spawn.entity || m.count != e.spawn.count ||
            !bit || (t.slots & bit))
            t.stale = true;
        t.slots |= bit;
    }
    auto missing = [&](size_t i) {  // the slots marker i spawns now (all once its stale ones are erased)
        const Marker& m = markers[i];
        if (m.kind != MarkerKind::Spawn || !activeAt(m, timeOfDay)) return 0u;
        return ((1u << m.count) - 1) & ~(tally[i].stale ? 0 : tally[i].slots);
    };
    for (size_t i = 0; i < markers.size(); ++i) any |= tally[i].stale || missing(i);
    if (!any) return;
    std::erase_if(entities, [&](const Entity& e) {
        ptrdiff_t i = spawnOf(e.spawn.id);
        return i < 0 || tally[size_t(i)].stale;
    });
    for (size_t i = 0; i < markers.size(); ++i)
        if (uint32_t slots = missing(i)) spawnAt(entities, world, markers[i], seed, face, slots);
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

void setState(Entity& e, AiState to, const char* reason) {
    AiState from = e.state;
    if (from == AiState::Windup) e.body.hitPending = false;  // exit: a windup cut short never lands (it has landed if it ran out)
    e.history[e.transitions++ % e.history.size()] = {from, to, reason, e.stateTime};
    e.state = to;
    e.stateTime = 0;
    e.lostTime = 0;
    if (std::string_view(reason) == "forced") return;  // from the debug window: no enter actions
    // Enter Return: it ignores whatever happened before it turned back, too.
    if (to == AiState::Return) e.events = 0;
    // Enter Chase from calm: one with spawn marker lines shouts the next one, not more often than every
    // kShoutSeconds (e.g. when hit from outside its territory, where it can't stay in Chase).
    bool calm = from != AiState::Windup && from != AiState::Recover && from != AiState::Chase;
    if (to == AiState::Chase && calm && e.body.alive() && !e.spawn.lines.empty() && e.shoutTimer <= 0) {
        talk(e);
        e.shoutTimer = std::max(kShoutSeconds, e.speechTimer);
    }
}

namespace {

// One AI update of a wanderer: transitions (as many as the situation calls for, so e.g. a hit from
// beyond the leash is Chase and back to Return in one update), then the input of the state it ends in.
Input think(Entity& e, const Grid& g, const Player& target, float dt) {
    const EntityType& t = e.type();
    glm::vec2 toTarget = xz(target.pos - e.body.pos), toHome = xz(e.home - e.body.pos);
    float dist = glm::length(toTarget);
    glm::vec2 dir = dist > 1e-3f ? toTarget / dist : glm::vec2(0, 1);
    float targetFromHome = glm::distance(xz(target.pos), xz(e.home));
    // In reach as inReach() counts it (about level), so a player up a pillar counts toward giving up.
    bool fighter = t.temper != Temper::Harmless, inReach = dist < kPunchReach - 0.2f && std::abs(target.pos.y - e.body.pos.y) < kHeight;
    Input in;
    constexpr int kMaxHops = 8;  // the longest chain is 3 transitions (e.g. Recover, Chase, Return, Wander), then the input
    for (int hops = 0; hops < kMaxHops; ++hops) {
        if (!e.body.alive() && e.state != AiState::Dead) setState(e, AiState::Dead, "died");
        switch (e.state) {
        case AiState::Dead:  // lies where it fell
            if (!e.body.alive()) return in;
            setState(e, AiState::Wander, "respawned");
            continue;
        case AiState::Idle:
        case AiState::Wander:
        case AiState::Talk:
            if (fighter && (e.events & EventDamaged)) {
                e.events = 0;  // read once
                setState(e, AiState::Chase, "damaged");
                continue;
            }
            if (t.temper == Temper::Aggressive && target.alive() && e.calmTimer <= 0 && targetFromHome < t.reaggroRadius &&
                dist < t.aggroRange) {
                setState(e, AiState::Chase, "saw_target");
                continue;
            }
            if (e.speechTimer > 0 && e.shoutTimer == 0) {  // stands still and faces the player while talking (not after a shout)
                if (e.state != AiState::Talk) setState(e, AiState::Talk, "spoken_to");
                if (dist > 0.01f) turnToward(e.body, toTarget, dt);
                return in;
            }
            if (e.state != AiState::Wander) setState(e, AiState::Wander, e.state == AiState::Talk ? "done_talking" : "wanders");
            wander(e, g, target.pos, dt);
            in.move = e.wanderDir;
            return in;
        case AiState::Chase:  // runs at the target until in reach, then winds up a swing
            if (!fighter) setState(e, AiState::Wander, "harmless");  // forced from the debug window
            else if (!target.alive()) setState(e, AiState::Return, "target_down");
            else if (targetFromHome >= t.leashRadius) setState(e, AiState::Return, "target_left_leash");
            else if (e.lostTime >= t.giveUpSeconds) setState(e, AiState::Return, "lost_target");
            else if (e.body.health < t.fleeHealth * float(maxHealth(sheet(e.body)))) setState(e, AiState::Flee, "low_health");
            else if (inReach && e.stateTime >= t.cooldown && startPunch(e.body, dir, t.windup)) setState(e, AiState::Windup, "in_reach");
            else {
                if (walkable(g, e.body.pos, dir, target.pos)) {
                    in.move = dir;
                    in.run = true;
                    e.lostTime = 0;
                } else {  // ponytail: no pathfinding; it waits, and gives up after giveUpSeconds
                    turnToward(e.body, dir, dt);
                    if (!inReach) e.lostTime += dt;
                }
                return in;
            }
            continue;
        case AiState::Windup:  // committed: stands and doesn't turn; updateCombat lands the hit at its end, if still in reach
            if (e.body.hitPending) return in;
            setState(e, AiState::Recover, "swung");
            continue;
        case AiState::Recover:
            if (e.stateTime < t.recover) return in;
            setState(e, AiState::Chase, "recovered");
            continue;
        case AiState::Flee:
            if (e.stateTime >= kFleeSeconds) {
                setState(e, AiState::Return, "fled");
                continue;
            }
            if (walkable(g, e.body.pos, -dir, target.pos)) {
                in.move = -dir;
                in.run = true;
            }
            return in;
        case AiState::Return:  // ignores the player (and hits) until home
            if (glm::length(toHome) <= std::max(e.wanderRadius, 1.0f)) {
                // Heals only after walking back: one that gives up at home (hit from beyond the leash)
                // arrives in the same update, with stateTime still 0, and keeps its wounds.
                if (e.stateTime > 0) e.body.health = float(maxHealth(sheet(e.body)));
                e.calmTimer = t.reaggroCooldown;
                setState(e, AiState::Wander, "home");
                continue;
            }
            if (e.lostTime >= t.giveUpSeconds) {  // stuck on the way: wandering may get it unstuck
                setState(e, AiState::Wander, "cannot_get_home");
                continue;
            }
            if (glm::vec2 d = glm::normalize(toHome); walkable(g, e.body.pos, d, target.pos)) {
                in.move = d;
                e.lostTime = 0;
            } else e.lostTime += dt;
            return in;
        case AiState::Count:
            break;
        }
        break;
    }
    assert(!"think: transitions didn't settle");  // a cycle in the graph; release builds just stand still this tick
    return in;
}

}  // namespace

void updateEntities(std::vector<Entity>& entities, const Grid& world, const Player& player, float dt) {
    for (Entity& e : entities) {
        e.speechTimer = std::max(0.0f, e.speechTimer - dt);
        e.shoutTimer = std::max(0.0f, e.shoutTimer - dt);
        e.calmTimer = std::max(0.0f, e.calmTimer - dt);
        e.stateTime += dt;
        e.body.pos = clearOfTerrain(world, e.body.pos);  // e.g. respawned at a home that was built over since
        if (!e.type().wanders) {  // stands where it was put: only dies and comes back
            if (!e.body.alive() && e.state != AiState::Dead) setState(e, AiState::Dead, "died");
            else if (e.body.alive() && e.state != AiState::Idle) setState(e, AiState::Idle, e.state == AiState::Dead ? "respawned" : "stands");
            e.events = 0;
            continue;
        }
        Input in = think(e, world, player, dt);
        e.events = 0;  // read by this update only: later hits arrive with the next
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
    if (t.temper != Temper::Harmless) e.events |= EventDamaged;
    if (!e.body.alive() && (t.lootChance >= 1 || rand01(e.rng) < t.lootChance)) e.loot = t.loot;  // rand01 may return 1
    return true;
}

bool startPunch(Player& b, glm::vec2 aim, float windup) {
    if (!b.alive() || b.swingTimer > 0) return false;
    b.swingWeapon = b.inv.equipped[SlotMainHand];
    b.swingTimer += swingTime(sheet(b), weaponAttack(b.swingWeapon));  // keeps the overshoot of the last frame
    b.punchLength = std::min(windup / kPunchImpact, b.swingTimer);
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
    StatSheet s = sheet(b);
    if (!b.alive()) b.deadTime += dt;
    else if (b.combatTimer == 0) b.health += healthRegen(s) * dt;
    b.health = std::min(b.health, float(maxHealth(s)));
}

// True once per swing, at its impact frame. A swing dies with its swinger.
static bool lands(Player& b) {
    if (!b.alive()) b.hitPending = false;
    if (!b.hitPending || b.punchTime < kPunchImpact * b.punchLength) return false;
    b.hitPending = false;
    return true;
}

int resolveHit(const Combatant& attacker, const Combatant& defender, const Attack& attack, const HitContext& ctx) {
    StatSheet a = sheet(attacker), d = sheet(defender);
    float hit = float(rollDamage(a, attack, ctx.rng)) * a[Stat::Damage];
    return mitigate(hit, d[Stat::Armor], attacker.level, ctx.armorK, d[Stat::DamageTaken]);
}

void updateCombat(Player& player, std::vector<Entity>& entities, float dt, std::mt19937& rng, float armorK) {
    HitContext ctx{rng, armorK};
    tick(player, dt);
    if (lands(player)) {
        Entity* target = punchTarget(entities, player);
        if (target && damage(*target, resolveHit(player, target->body, weaponAttack(player.swingWeapon), ctx)))
            player.combatTimer = kCombatSeconds;
    }
    for (Entity& e : entities) {
        tick(e.body, dt);
        if (lands(e.body) && inReach(e.body, player.pos) && hurt(player, resolveHit(e.body, player, weaponAttack(e.body.swingWeapon), ctx)))
            e.body.combatTimer = kCombatSeconds;
        if (!e.body.alive() && e.body.deadTime >= e.type().respawnSeconds) {  // back home, fully healed
            e.body.teleport(e.home);
            e.body.vel = {};
            e.body.health = float(maxHealth(sheet(e.body)));
            e.body.deadTime = 0;
            e.body.combatTimer = 0;
            e.loot = NoItem;
            e.wanderDir = {};
        }
    }
}
