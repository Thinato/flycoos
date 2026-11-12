#include "entity.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <iterator>
#include <numbers>
#include <string_view>
#include <tuple>

#include "defs.h"
#include "edit.h"
#include "melee.h"
#include "projectile.h"

namespace {

constexpr float kTau = 2 * std::numbers::pi_v<float>;
constexpr float kSpeechSeconds = 5;
constexpr float kShoutSeconds = 8;  // at least this long between shouts
constexpr float kBodyRadius = 0.8f;  // player-to-entity distance kept by separate()
constexpr float kNpcSpacing = 1.0f;  // NPC-to-NPC distance kept by updateEntities

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

// A way round whatever is in the way of `dir` (a crate, a pillar, a pool's edge; ADR-055): 45 degrees to one side,
// the other, then 90 (each checked for the body's width: a slanted doorway a body just fits still works); zero if none. Always that order, so it keeps to one side. ponytail: no pathfinding; a
// detour counts toward giving up (lostTime) and a free step only winds it back, so it can't circle a pillar
// with the player on top forever.
// walkable() for the body's width: its middle, and the terrain ahead of both its edges (a corner it would catch on).
bool roomy(const Grid& g, glm::vec3 feet, glm::vec2 dir, glm::vec3 player) {
    glm::vec3 side = glm::vec3(-dir.y, 0, dir.x) * kHalfWidth, nobody(1e9f);
    return walkable(g, feet, dir, player) && walkable(g, feet + side, dir, nobody) && walkable(g, feet - side, dir, nobody);
}
glm::vec2 detour(const Grid& g, glm::vec3 feet, glm::vec2 dir, glm::vec3 player) {
    for (float a : {0.785f, -0.785f, 1.571f, -1.571f}) {
        glm::vec2 d(dir.x * std::cos(a) - dir.y * std::sin(a), dir.x * std::sin(a) + dir.y * std::cos(a));
        if (roomy(g, feet, d, player)) return d;
    }
    return {};
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
    static std::atomic<uint32_t> made{0};
    Entity e{};
    e.id = ++made;
    e.kind = kind;
    e.rng.seed(rngSeed);
    e.attackRng.seed(rngSeed ^ 0xA77AC4u);  // its own attacks' draws (ADR-057), apart: a fight without them goes as before
    const EntityType& t = e.type();
    e.name = t.name ? t.name : kNames[e.rng() % std::size(kNames)];
    e.wanderRadius = t.wanderRadius;
    e.body.stats = t.stats;
    e.body.level = t.level;
    e.body.tuning.walkSpeed = 2;  // a stroll
    e.body.inv.equipped[SlotMainHand] = t.weapon;
    e.body.inv.equipped[SlotOffHand] = t.shield;
    enterPhase(e, 0);  // its run speed and own modifiers
    e.body.health = float(maxHealth(sheet(e.body)));
    e.body.stance = t.stance;
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
    if (speechTimer <= 0) return nullptr;
    if (saying >= 0) return size_t(saying) < defs().questLines.size() ? defs().questLines[size_t(saying)].text.c_str() : nullptr;
    if (phrase < 0) return nullptr;
    if (spawn.lines.empty()) return kPhrases[phrase];
    return size_t(phrase) < spawn.lines.size() ? spawn.lines[size_t(phrase)].c_str() : nullptr;
}

const std::string& Entity::displayName() const {
    return !spawn.lines.empty() && spawn.count == 1 && !spawn.name.empty() ? spawn.name : name;
}

Entity& spawnAhead(std::vector<Entity>& entities, const Grid& world, const Player& player, EntityKind kind,
                   uint32_t seed, float distance) {
    glm::vec3 at = player.pos + glm::vec3(std::sin(player.yaw), 0, std::cos(player.yaw)) * distance;
    return entities.emplace_back(makeEntity(kind, seed, clearOfTerrain(world, at), player.pos));
}

// Where the i-th of a group stands from the first (spawn markers and spawnGroup): a golden-angle spiral.
static glm::vec2 spiral(int i) {
    float r = 1.6f * std::sqrt(float(i)), a = 2.4f * float(i);
    return r * glm::vec2(std::cos(a), std::sin(a));
}

int spawnGroup(std::vector<Entity>& entities, const Grid& world, EntityKind kind, int count, glm::vec3 at,
               glm::vec3 face, uint32_t seed) {
    if (!(std::abs(at.x) < 1e6f && std::abs(at.y) < 1e6f && std::abs(at.z) < 1e6f)) return 0;  // NaN, far out: nowhere
    int made = 0, y0 = int(std::floor(at.y));
    for (int i = 0; i < std::min(count, kMaxSpawnedNpcs); ++i) {
        glm::vec2 p = xz(at) + spiral(i);
        glm::ivec2 c(glm::floor(p));
        // The standing place nearest `at`'s height (not a roof or a crown above it), else the column's top.
        int feet = -1;
        for (int k = 0; k < 9 && feet < 0; ++k) {
            int y = y0 + (k % 2 ? (k + 1) / 2 : -k / 2);  // 0, +1, -1, +2, -2 ... +-4
            auto b = [&](int dy) { return world.get({c.x, y + dy, c.y}); };
            if (isOpaque(b(-1)) && !isOpaque(b(0)) && !isOpaque(b(1))) feet = y;
        }
        if (feet < 0) feet = groundTop(world, c.x, c.y) + 1;
        if (feet <= 0) continue;
        entities.push_back(makeEntity(kind, seed + uint32_t(i) * 0x9E3779B9u, {p.x, float(feet), p.y}, face));
        ++made;
    }
    return made;
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
    // In front of the starting camera, which looks toward +Z. The bandit's camp (its archer's too) is far enough
    // out that even at the edge of their wandering the spawn is out of their aggro range (ADR-055).
    add(EntityKind::TrainingDummy, {3, 6});
    add(EntityKind::Villager, {-4, 7});
    add(EntityKind::Boar, {9, 11});
    add(EntityKind::Bandit, {-5, 34});
    add(EntityKind::BanditArcher, {-9, 40});  // (ADR-051) at the bandit's camp, behind it
    return out;
}

// The entities of spawn marker `m` in the slots of bit mask `slots`: the first on the marker, the
// others on a small spiral around it, each on the first dry ground within a few blocks of the
// marker's height (else on the marker).
static void spawnAt(std::vector<Entity>& out, const Grid& world, const Marker& m, uint32_t seed, glm::vec3 face, uint32_t slots) {
    for (int i = 0; i < m.count; ++i) {
        if (!(slots >> i & 1)) continue;
        glm::vec3 pos = clearOfTerrain(world, m.pos);
        glm::vec2 at = xz(m.pos) + spiral(i);
        glm::ivec2 c(glm::floor(at));
        for (int y = int(m.pos.y) + 3; i && y > int(m.pos.y) - 4; --y) {
            auto block = [&](int dy) { return world.get({c.x, y + dy, c.y}); };
            if (isOpaque(block(-1)) && isClear(block(0)) && isClear(block(1))) {
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
                glm::vec3 face, float timeOfDay, const DeadSlots& dead) {
    // Runs every tick in worlds with timed markers: no hashing, one small buffer, no copies of unchanged lines.
    auto spawnOf = [&](uint32_t id) -> ptrdiff_t {  // the index of spawn marker `id`, or -1
        auto it = std::ranges::lower_bound(markers, id, {}, &Marker::id);
        return it != markers.end() && it->id == id && it->kind == MarkerKind::Spawn ? it - markers.begin() : -1;
    };
    // Outside its marker's window an NPC leaves once it isn't fighting, a corpse once looted (after
    // kCorpseLinger, so a kill shows) or back up.
    std::erase_if(entities, [&](const Entity& e) {
        ptrdiff_t i = spawnOf(e.spawn.id);
        return i >= 0 && !activeAt(markers[size_t(i)], timeOfDay) && !e.hunting() &&
               (e.body.alive() || (e.loot == NoItem && e.body.deadTime >= kCorpseLinger));
    });
    // A marker respawns all of its entities when any is out of date, and spawns the slots it misses
    // (a window opening, a removed marker undone) while active, keeping the rest as they are.
    struct Tally {
        uint32_t slots = 0;  // bit per slot present
        uint32_t kept = 0;   // stale, but a corpse with loot that stays
        bool stale = false;
    };
    std::vector<Tally> tally(markers.size());
    bool any = false;  // something to erase or spawn
    for (Entity& e : entities) {
        if (!e.spawn.id) continue;  // not from a marker (spawnAhead): left alone
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
    for (Entity& e : entities) {  // a changed marker's corpse with loot on it stays, under the marker as it is now (ADR-056)
        ptrdiff_t i = spawnOf(e.spawn.id);
        if (e.spawn.id && i >= 0 && tally[size_t(i)].stale && !e.body.alive() && e.loot != NoItem && e.slot < markers[size_t(i)].count)
            e.spawn = markers[size_t(i)], tally[size_t(i)].kept |= 1u << e.slot;
    }
    auto missing = [&](size_t i) {  // the slots marker i spawns now (all once its stale ones are erased), never the dead's
        const Marker& m = markers[i];
        if (m.kind != MarkerKind::Spawn || !activeAt(m, timeOfDay)) return 0u;
        auto d = dead.find(m.id);
        return ((1u << m.count) - 1) & ~(tally[i].stale ? tally[i].kept : tally[i].slots) & ~(d != dead.end() ? d->second : 0);
    };
    for (size_t i = 0; i < markers.size(); ++i) any |= tally[i].stale || missing(i);
    if (!any) return;
    std::erase_if(entities, [&](const Entity& e) {
        ptrdiff_t i = spawnOf(e.spawn.id);
        return e.spawn.id && (i < 0 || (tally[size_t(i)].stale && (e.body.alive() || !(tally[size_t(i)].kept >> e.slot & 1))));
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

// Forgets the attack of its own under way (ADR-057), ending its move if it's still on (a forced state, a defs reload):
// a leap's hold in the air can't outlive it.
static void dropAttack(Entity& e) {
    if (e.attack >= 0 && attackMove(e.body.move)) interrupt(e.body, 0, false);
    e.attack = -1;
}

void setState(Entity& e, AiState to, const char* reason) {
    AiState from = e.state;
    if (from == AiState::Windup) e.body.hitPending = false;  // exit: a windup cut short never lands (it has landed if it ran out)
    if (from == AiState::Roar) e.body.roar = 0;  // (its pose ends)
    e.history[e.transitions++ % e.history.size()] = {from, to, reason, e.stateTime};
    e.state = to;
    e.stateTime = 0;
    e.lostTime = 0;
    if (to != AiState::Windup) e.string = 0;  // a string ends with its windups (ADR-042)
    if (to != AiState::Windup && to != AiState::Recover) dropAttack(e);  // an attack of its own ends with its recovery (ADR-057)
    if (to == AiState::Stagger || to == AiState::Dead || to == AiState::Return || to == AiState::Flee || to == AiState::Search)
        e.counterTimer = e.backOff = e.guardTimer = 0;  // a counter or a plan doesn't outlast a hit or the fight
    if (std::string_view(reason) == "forced") {  // from the debug window: no enter actions; a forced chase or search starts where it stands
        bool fresh = from != AiState::Chase && from != AiState::Windup && from != AiState::Recover && from != AiState::Stagger && from != AiState::Search;
        if ((to == AiState::Chase || to == AiState::Search) && fresh) e.lastSeen = e.body.pos, e.unseen = 0;
        return;
    }
    // Enter Return: it ignores whatever happened before it turned back, too.
    if (to == AiState::Return) e.events = 0;
    // Enter Chase from calm: one with spawn marker lines shouts the next one, not more often than every
    // kShoutSeconds (e.g. when hit from outside its territory, where it can't stay in Chase).
    bool calm = from != AiState::Windup && from != AiState::Recover && from != AiState::Chase && from != AiState::Stagger &&
                from != AiState::Search;  // (finding the player again isn't a new fight)
    if (to == AiState::Chase && calm && e.body.alive() && !e.spawn.lines.empty() && e.shoutTimer <= 0) {
        talk(e);
        e.shoutTimer = std::max(kShoutSeconds, e.speechTimer);
    }
}

// A creature attack's timing (ADR-057): a moment into its windup pose (held there for the telegraph), the part
// that goes (a charge's run, a leap's landing, a sweep, a grab's reach), then what follows (a grab's: its bites).
static MoveSpec attackSpec(const CreatureAttack& a) {
    float wind = 0.3f, go = 0.2f, after = 0.45f;
    switch (a.type) {
    case AttackType::Charge: go = a.seconds, after = 0.35f; break;
    case AttackType::Leap: go = 0.08f, after = 0.4f; break;
    case AttackType::Area: wind = 0.35f; break;
    case AttackType::Grab: after = float(a.hits) * a.interval + 0.3f; break;
    case AttackType::Count: break;
    }
    float n = wind + go + after;
    MoveSpec m;
    m.seconds = n, m.holdAt = wind / n, m.impact = (wind + go) / n, m.chain = m.cancel = 1;
    m.damage = a.power, m.stance = a.stance, m.heavy = a.hyperarmor;
    return m;
}

const char* attackBlocked(const Entity& e, const Grid& world, const Player& target, size_t i) {
    const EntityType& t = e.type();
    if (i >= t.attacks.size()) return "none";
    const CreatureAttack& a = t.attacks[i];
    const Player& b = e.body;
    if (!b.alive() || !target.alive() || e.state != AiState::Chase) return "not fighting";
    if (target.heldBy && target.heldBy != e.id) return "player held";  // the others wait while one holds the player
    if (!e.token) return "no token";
    if (b.punching() || b.rolling() || b.stagger > 0 || !b.onGround || ranged(b) || e.backOff > 0 || e.guardTimer > 0 ||
        (e.threatened && e.answer != Answer::None))
        return "busy";
    if (e.attackWait[i] > 0 || e.stateTime < t.cooldown) return "cooling down";
    glm::vec2 d = xz(target.pos - b.pos);
    float dist = glm::length(d);
    if (dist < a.rangeMin || dist > a.rangeMax || std::abs(target.pos.y - b.pos.y) >= 1.5f) return "out of range";
    if (a.type != AttackType::Area && !canSee(world, b, target)) return "can't see";  // (a sweep goes round a corner)
    if ((a.type == AttackType::Charge || a.type == AttackType::Leap) && dist > 1e-3f && !roomy(world, b.pos, d / dist, glm::vec3(1e9f)))
        return "no room";
    return nullptr;
}

void startAttack(Entity& e, size_t i, glm::vec2 aim) {
    const CreatureAttack& a = e.type().attacks[i];
    Player& b = e.body;
    setState(e, AiState::Windup, kAttackTypeIds[size_t(a.type)]);  // (before the move: leaving a windup cancels its hit)
    e.attack = int(i);
    e.attackWait[i] = a.cooldown;
    e.sinceSwing = e.counterTimer = e.guardTimer = 0;
    e.leapt = false, e.bites = 0;
    startMove(b, Move(int(Move::Charge) + int(a.type)), aim);
    b.special = attackSpec(a);
    b.moveScale = 1;  // its own timing: weapon speed doesn't change it
    b.hold = std::max(0.0f, a.windup * (0.85f + 0.5f * rand01(e.attackRng)) - holdTime(b));  // the telegraph varies, as a swing's (ADR-035)
    b.noise(Noise::Windup);
}

void enterPhase(Entity& e, int n) {
    e.phase = std::clamp(n, 0, int(typeOf(e.kind).phases.size()));
    const EntityType& t = e.type();
    e.body.tuning.runSpeed = t.chaseSpeed;
    e.body.innate = t.mods;
    for (Modifier& m : e.body.innate) m.source = "innate";  // (not its kind's name: defs may be reloaded under it)
    e.attackWait.fill(0);  // a new phase opens with its attacks ready (its stance tops out at the phase's in updateCombat)
}

int phaseFor(const Entity& e) {
    const std::vector<EntityType>& phases = typeOf(e.kind).phases;
    if (phases.empty()) return 0;  // (most creatures: no sheet to build)
    float share = e.body.health / float(maxHealth(sheet(e.body)));
    int n = 0;
    for (size_t i = 0; i < phases.size(); ++i)
        if (share <= phases[i].phaseAt) n = int(i) + 1;
    return n;
}

std::optional<BossBar> bossBar(const std::vector<Entity>& entities, const Player& player) {
    const Entity* best = nullptr;
    for (const Entity& e : entities)
        if (e.type().boss && e.body.alive() && e.hunting() && player.alive() &&
            (!best || glm::distance(e.body.pos, player.pos) < glm::distance(best->body.pos, player.pos)))
            best = &e;
    if (!best) return std::nullopt;
    const EntityType& t = best->type();
    float stance = best->body.broken ? 1 : t.stance > 0 ? std::clamp(1 - best->body.stance / t.stance, 0.0f, 1.0f) : 0;
    return BossBar{best, std::clamp(best->body.health / float(maxHealth(sheet(best->body))), 0.0f, 1.0f), stance};
}

namespace {

bool quadruped(const EntityType& t) { return t.model == ModelBoar || t.model == ModelWolf; }

// Sees the player's attack coming and answers it (ADR-043). The answer is picked once per attack, by
// its kind's chances, as soon as it's chasing (a swing of its own is committed). It comes after a
// reaction time: a guard goes up and stays while the attack can land; a parry and a dodge wait for
// the moment the hit is due, give or take its aim. A heavy can't be parried: it guards instead.
void answer(Entity& e, const Grid& g, const Player& target, glm::vec2 dir, float dist, float dt, Input& in) {
    const EntityType& t = e.type();
    glm::vec2 fwd{std::sin(target.yaw), std::cos(target.yaw)};
    const WeaponClassDef& w = weaponClass(classOf(target.weapon()));  // a great weapon reaches farther and wider (ADR-049)
    bool threat = target.alive() && target.punching() && target.hitPending && (!codeMove(target.move) || target.move == Move::Throw) && target.move != Move::Reload &&  // (a reload hits nothing, ADR-052)
                  dist < std::max(kPunchReach, w.reach) + 1.5f && glm::dot(-dir, fwd) > std::min(0.3f, w.arcCos) &&
                  std::abs(target.pos.y - e.body.pos.y) < kHeight;
    if (!threat) e.picked = false;
    if (threat && !e.picked && e.state == AiState::Chase && t.guard + t.parry + t.dodge > 0) {
        e.picked = true;
        float r = rand01(e.rng);
        e.answer = r < t.guard ? Answer::Guard : r < t.guard + t.parry ? Answer::Parry : r < t.guard + t.parry + t.dodge ? Answer::Dodge : Answer::None;
        if (e.answer == Answer::Parry && (spec(target).heavy || target.move == Move::Riposte || target.move == Move::Shot || target.move == Move::Throw))
            e.answer = Answer::Guard;  // (an arrow can't be parried: ADR-050, nor a throwable: ADR-053)
        e.react = gAi.reactFrom + (gAi.reactTo - gAi.reactFrom) * rand01(e.rng);
        e.aimOff = gAi.aimError * (2 * rand01(e.rng) - 1);
    }
    e.threatened = threat;
    if (!threat || e.state != AiState::Chase) e.answer = Answer::None;
    if (e.answer == Answer::None || (e.react -= dt) > 0) return;
    bool free = !e.body.punching() && !e.body.rolling() && e.body.stagger <= 0;
    float due = target.holding ? 99 : target.hold + std::max(0.0f, impactTime(target) - target.moveTime);  // seconds to its hit
    if (e.answer == Answer::Guard) {
        e.guardTimer = gAi.guardHold;
    } else if (e.answer == Answer::Parry && free && due <= 0.5f * (gMelee.parryFrom + gMelee.parryTo) * parryOf(e.body) + e.aimOff) {
        startMove(e.body, Move::Parry, dir);  // as the player's: deflects in its window, else a weak guard
        e.body.moveScale = parryOf(e.body);
        e.body.hitPending = false;
        e.guardTimer = 0;
        e.answer = Answer::None;
    } else if (e.answer == Answer::Dodge && free && e.body.onGround && e.body.rollTime >= e.body.rollLength + 0.3f &&
               due <= gAi.dodgeLead + e.aimOff) {  // (a roll needs a moment after the last one)
        // A roll to the side (whichever way it can), else a backstep; four-legged ones hop back.
        glm::vec2 side = rand01(e.rng) < 0.5f ? glm::vec2(dir.y, -dir.x) : glm::vec2(-dir.y, dir.x);
        in.roll = true;
        in.move = quadruped(t)                             ? glm::vec2(0)
                : walkable(g, e.body.pos, side, target.pos)  ? side
                : walkable(g, e.body.pos, -side, target.pos) ? -side
                                                             : glm::vec2(0);
        e.guardTimer = 0;
        e.answer = Answer::None;
    }
}

// The next hit of the string it swings (ADR-042): the next light in turn, now or (its kind's `delay`
// chance) held in its windup a while, to catch a roll that came too soon. Each costs its swing time
// as a hit on its own would: a string bunches its hits up, with a longer pause after, but it hits no
// more often.
void nextInString(Entity& e, glm::vec2 dir) {
    const EntityType& t = e.type();
    --e.string;
    Move m = e.body.move;
    Move next = m >= Move::Light1 && m < Move::Light4 ? Move(int(m) + 1) : Move::Light1;
    bool delayed = t.delay > 0 && rand01(e.rng) < t.delay;
    float hold = delayed ? gAi.delaySeconds * (0.8f + 0.4f * rand01(e.rng)) : 0;
    setState(e, AiState::Windup, delayed ? "delayed" : "combo");  // before the move: leaving a windup cancels its hit
    startMove(e.body, next, dir, hold);
    e.body.swingTimer += swingTime(sheet(e.body, skillOf(e.body.swingWeapon)), weaponAttack(e.body.swingWeapon));  // the pause after is longer
    if (delayed) e.body.noise(Noise::Windup);
}

// Whether nothing solid stands between `from` and `to`.
bool clearLine(const Grid& g, glm::vec3 from, glm::vec3 to) {
    glm::vec3 d = to - from;
    float length = glm::length(d);
    return length < 1e-4f || !raycast(g, from, d / length, length);
}

// An archer with its bow out, chasing (ADR-051): steps in while too far or with nothing to see (no line from its bow
// to the player's chest), backs off facing it while too close, and in its band (or cornered, too close with no
// room behind) looses at it once its cooldown has passed since its last shot started (the first comes at once),
// strafing round it in between. True with a shot started (it's in Windup).
bool keepDistance(Entity& e, const Grid& g, const Player& target, glm::vec2 dir, float dist, float dt, Input& in) {
    const EntityType& t = e.type();
    e.sight = clearLine(g, shotOrigin(e.body), target.pos + glm::vec3(0, 1.1f, 0));
    bool closing = dist > t.keepMax || !e.sight, backing = !closing && dist < t.keepMin && walkable(g, e.body.pos, -dir, target.pos);
    bool waits = target.heldBy && target.heldBy != e.id;  // another holds the player (ADR-057)
    if (!closing && !backing && !waits && e.sinceSwing >= t.cooldown && !e.body.punching() && e.body.stagger <= 0) {  // (cornered: point-blank)
        e.sinceSwing = 0;
        setState(e, AiState::Windup, "shoots");
        startMove(e.body, Move::Shot, dir, gAi.drawHold * (0.85f + 0.3f * rand01(e.rng)));
        e.body.noise(Noise::Windup);  // the bow creaks: the tell
        return true;
    }
    if (e.strafeTimer <= 0) e.strafe = rand01(e.rng) < 0.5f ? 1.0f : -1.0f, e.strafeTimer = 1.5f + 2 * rand01(e.rng);
    glm::vec2 go = closing ? dir : backing ? -dir : glm::vec2(-dir.y, dir.x) * e.strafe;
    if (walkable(g, e.body.pos, go, target.pos)) {
        in.move = go, in.face = dir, in.run = closing && dist > t.keepMax + 2;
        e.lostTime = 0;
    } else {
        turnToward(e.body, dir, dt);
        if (closing) e.lostTime += dt;
        else e.strafe = -e.strafe;  // a wall: the other way round
    }
    return false;
}

// The first of its kind's attacks it may start now (ADR-057), by each one's chance: a failed one waits
// gAi.attackRetry, so the chance is drawn once per opening, not every tick. -1: none.
int pickAttack(Entity& e, const Grid& g, const Player& target) {
    const std::vector<CreatureAttack>& list = e.type().attacks;
    for (size_t i = 0; i < list.size(); ++i) {
        if (attackBlocked(e, g, target, i)) continue;
        if (list[i].chance >= 1 || rand01(e.attackRng) < list[i].chance) return int(i);
        e.attackWait[i] = gAi.attackRetry;  // not this time
    }
    return -1;
}

// The attack under way (ADR-057), this tick: it tracks the player through its telegraph as a swing does (until
// gMelee.trackSeconds before it goes), then a charge runs straight on (into a wall it reels: true), a leap takes
// off for where the player is, along its facing, and holds in the air until it lands, and a grab steps in.
bool attackStep(Entity& e, const Grid& g, glm::vec2 dir, float dist, float dt, Input& in) {
    Player& b = e.body;
    const CreatureAttack& a = e.type().attacks[size_t(e.attack)];
    glm::vec2 fwd{std::sin(b.yaw), std::cos(b.yaw)};
    if (b.hitPending && (b.moveTime < holdTime(b) || b.hold > gMelee.trackSeconds)) turnToward(b, dir, dt);
    if (!b.punching() || b.moveTime < holdTime(b) || b.hold > 0) return false;  // still telegraphing
    switch (a.type) {
    case AttackType::Charge: {
        if (b.moveTime >= impactTime(b)) break;  // its run is over: it slows to a stop
        glm::ivec3 ahead(glm::floor(b.pos + glm::vec3(fwd.x, 0, fwd.y) * 0.9f));
        if (walkable(g, b.pos, fwd, glm::vec3(1e9f))) {
            in.move = fwd, in.speed = a.speed;
        } else if (isOpaque(g.get(ahead + glm::ivec3(0, 1, 0)))) {  // a wall: it crashes, reeling a moment
            interrupt(b, gAi.crashSeconds, false);
            b.landSpeed = 12, b.noise(Noise::Land);  // the thud
            return true;
        } else {
            b.moveTime = impactTime(b);  // a drop or water ahead: it pulls up
        }
        break;
    }
    case AttackType::Leap:
        if (!e.leapt) {  // takes off for where the player is now, along its facing (a late sidestep beats it)
            float flight = 2 * a.height / b.tuning.gravity, speed = std::min(std::max(0.0f, dist - 0.8f) / flight, 25.0f);
            b.vel = {fwd.x * speed, a.height, fwd.y * speed};
            b.onGround = false;
            b.moveTime = holdTime(b);
            e.leapt = true, e.leapAt = e.stateTime;
            b.noise(Noise::Jump);
        }
        b.holding = !b.onGround && !b.inWater && e.stateTime - e.leapAt < 3;  // in the air (at most 3 s): it strikes as it lands
        if (b.holding) in.move = fwd, in.speed = glm::length(glm::vec2(b.vel.x, b.vel.z));
        break;
    case AttackType::Grab:
        if (b.hitPending) in.move = fwd, in.speed = gAi.grabLunge;  // steps in to its catch
        break;
    case AttackType::Area:
    case AttackType::Count: break;
    }
    return false;
}

// One AI update of a wanderer: transitions (as many as the situation calls for, so e.g. a hit from
// beyond its chase range is Chase and back to Return in one update), then the input of the state it ends in.
Input think(Entity& e, const Grid& g, const Player& target, float dt) {
    const EntityType& t = e.type();
    glm::vec2 toTarget = xz(target.pos - e.body.pos), toHome = xz(e.home - e.body.pos);
    float dist = glm::length(toTarget), range = glm::distance(target.pos, e.body.pos);  // range: in 3D (a cliff, another floor)
    glm::vec2 dir = dist > 1e-3f ? toTarget / dist : glm::vec2(0, 1);
    // In reach as inReach() counts it (about level), so a player up a pillar counts toward giving up.
    bool fighter = t.temper != Temper::Harmless, inReach = dist < kPunchReach - 0.2f && std::abs(target.pos.y - e.body.pos.y) < kHeight;
    // Sight (ADR-055): nothing solid from its eyes to the player's chest. Looked up once per update, and only when asked.
    int seen = -1;
    auto sees = [&] {
        if (seen < 0) seen = target.alive() && canSee(g, e.body, target);
        return seen == 1;
    };
    if (fighter && (e.events & EventDamaged)) e.lastSeen = target.pos, e.unseen = 0;  // a hit tells it where the player is, even one it reels from
    // A boss's territory (ADR-059): within it, it always knows where the player is; it fights them to the end there.
    bool territorial = t.territory > 0, inside = territorial && glm::length(xz(target.pos - e.home)) <= t.territory;
    if (fighter && e.hunting()) {  // fighting or searching: where it last saw the player
        if (inside || (range < t.chaseRange && sees())) e.lastSeen = target.pos, e.unseen = 0;
        else e.unseen += dt;
    }
    Input in;
    // An archer's hands (ADR-051): its sidearm once the player is close in a fight, the bow back past its keepMin
    // (or closeIn, if that's farther: no flip-flopping between the two).
    if (ItemId& hand = e.body.inv.equipped[SlotMainHand]; t.sidearm && !e.body.punching())
        hand = e.angry() && dist < (hand == t.sidearm ? std::max(t.keepMin, gAi.closeIn) : gAi.closeIn) ? t.sidearm : t.weapon;
    if (fighter) answer(e, g, target, dir, dist, dt, in);
    if (e.attack >= int(t.attacks.size())) dropAttack(e);  // (defs reloaded with fewer)
    bool waits = target.heldBy && target.heldBy != e.id;  // another holds the player: the rest wait (ADR-057)
    constexpr int kMaxHops = 8;  // the longest chain is 4 transitions (e.g. Return, Chase, Return, Wander), then the input
    for (int hops = 0; hops < kMaxHops; ++hops) {
        if (!e.body.alive() && e.state != AiState::Dead) setState(e, AiState::Dead, "died");
        else if (e.body.alive() && e.body.stagger > 0 && e.state != AiState::Stagger && e.state != AiState::Return && e.state != AiState::Flee)
            setState(e, AiState::Stagger, e.body.broken ? "stance_broken" : "flinched");
        else if (int n = phaseFor(e); n > e.phase && e.body.alive() && e.hunting() && e.state != AiState::Stagger && (!territorial || inside)) {
            // A boss's next phase (ADR-059), once a stagger has played out: it roars, then fights with the phase's numbers.
            static constexpr const char* kPhaseReasons[] = {"phase_1", "phase_2", "phase_3", "phase_4"};
            static_assert(std::size(kPhaseReasons) == kMaxPhases);
            setState(e, AiState::Roar, kPhaseReasons[n]);
            interrupt(e.body, 0, false);  // (a swing on its way stops; setState dropped an attack)
            enterPhase(e, n);
            e.body.noise(Noise::Windup);
        }
        switch (e.state) {
        case AiState::Dead:  // lies where it fell
            if (!e.body.alive()) return in;
            setState(e, AiState::Wander, "respawned");
            continue;
        case AiState::Idle:
        case AiState::Wander:
        case AiState::Talk:
            if (e.events & EventDamaged) {
                e.events = 0;  // read once
                setState(e, fighter ? AiState::Chase : AiState::Flee, "damaged");  // a villager runs from it (ADR-056)
                continue;
            }
            // Aggro range is in 3D (a player high above, or a floor below, is out of it) and needs sight (ADR-055).
            // ponytail: no field of view; it sees all round (add one with stealth).
            if (t.temper == Temper::Aggressive && target.alive() && e.calmTimer <= 0 && range < t.aggroRange && (!territorial || inside) && sees()) {
                e.lastSeen = target.pos, e.unseen = 0;  // (not a fight yet: nothing above noted it)
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
        case AiState::Chase:  // runs at the target (around it to its side, with others) until in reach, then winds up a swing
            if (!fighter) setState(e, AiState::Wander, "harmless");  // forced from the debug window
            else if (!target.alive()) setState(e, AiState::Return, "target_down");
            else if (territorial && !inside) setState(e, AiState::Return, "left_territory");  // a boss fights to the end only in its territory (ADR-059)
            else if (!territorial && range >= t.chaseRange) setState(e, AiState::Return, "target_far");  // no leash from home (ADR-055)
            else if (e.unseen > 0 && e.unseen >= gAi.sightGrace) setState(e, AiState::Search, "lost_sight");  // (> 0: a grace of 0 can't flap)
            else if (!territorial && e.lostTime >= t.giveUpSeconds) setState(e, AiState::Return, "lost_target");
            else if (e.body.health < t.fleeHealth * float(maxHealth(sheet(e.body)))) setState(e, AiState::Flee, "low_health");
            else if (in.roll || e.body.move == Move::Parry || e.body.rolling()) return in;  // dodging, or in a parry
            else if (t.keepMax > 0 && ranged(e.body)) {  // an archer's bow out: it keeps its distance (ADR-051)
                if (!keepDistance(e, g, target, dir, dist, dt, in)) return in;
            } else if (int a = pickAttack(e, g, target); a >= 0) {  // an attack of its own (ADR-057), before a swing
                startAttack(e, size_t(a), dir);
            }
            // Swings with a token, not while it answers an attack (ADR-042, ADR-043). The windup varies a
            // little, so its timing can't just be counted (ADR-035); right after a block or a parry it's quick.
            else if (bool counter = e.counterTimer > 0;
                     inReach && e.token && !waits && e.backOff <= 0 && !(e.threatened && e.answer != Answer::None) &&
                     (e.guardTimer <= 0 || counter) && (e.stateTime >= t.cooldown || counter) &&  // (an archer's spaces its knife too: a poor fighter up close)
                     startPunch(e.body, dir, counter ? gAi.counterWindup : t.windup * (0.85f + 0.5f * rand01(e.rng)))) {
                e.string = t.combo > 1 ? 1 + int(e.rng() % unsigned(t.combo)) : 1;
                e.sinceSwing = e.counterTimer = e.guardTimer = 0;
                setState(e, AiState::Windup, counter ? "counter" : "in_reach");
            } else if (e.backOff > 0) {  // walks away facing the target a moment after its string
                if (walkable(g, e.body.pos, -dir, target.pos)) in.move = -dir, in.face = dir;
                else turnToward(e.body, dir, dt);
                return in;
            } else if (!e.token && e.pack > 1 && dist < gAi.waitRing + 3) {  // no token: circles on the ring, facing it
                float at = std::atan2(-dir.x, -dir.y);
                float a = at + std::clamp(std::remainder(e.side - at, kTau), -0.6f, 0.6f);
                glm::vec2 to = xz(target.pos) + glm::vec2(std::sin(a), std::cos(a)) * gAi.waitRing - xz(e.body.pos);
                if (e.strafeTimer <= 0) e.strafe = rand01(e.rng) < 0.5f ? 1.0f : -1.0f, e.strafeTimer = 1.5f + 2 * rand01(e.rng);
                bool there = glm::length(to) < 1.5f;
                glm::vec2 go = there ? to + glm::vec2(-dir.y, dir.x) * e.strafe : to;
                if (glm::length(go) > 0.05f && walkable(g, e.body.pos, go = glm::normalize(go), target.pos)) {
                    in.move = go, in.face = dir;
                    if (there) in.speed = e.body.tuning.walkSpeed * gAi.strafeSpeed;
                } else {
                    turnToward(e.body, dir, dt);
                    if (there) e.strafe = -e.strafe;  // blocked: the other way round
                }
                return in;
            } else if (inReach && dist < kPunchReach - 0.5f) {  // a sword's length off: holds there, facing it, until it can swing
                turnToward(e.body, dir, dt);
                return in;
            } else {
                glm::vec2 go = dir;
                if (e.pack > 1 && !inReach && dist < 6) {  // with others, once near: circle round to its side, then close in
                    float at = std::atan2(-dir.x, -dir.y);  // where it stands around the target
                    float off = std::remainder(e.side - at, 2 * std::numbers::pi_v<float>);
                    bool there = std::abs(off) < 0.4f;
                    float a = there ? e.side : at + std::clamp(off, -0.8f, 0.8f);
                    float r = there ? kPunchReach - 0.5f : std::max(dist - 1.5f, 2.5f);  // spirals in
                    glm::vec2 to = xz(target.pos) + glm::vec2(std::sin(a), std::cos(a)) * r - xz(e.body.pos);
                    if (glm::length(to) > 0.05f) go = glm::normalize(to);
                }
                if (inReach ? walkable(g, e.body.pos, go, target.pos) : roomy(g, e.body.pos, go, target.pos)) {
                    in.move = go;
                    in.run = true;
                    e.lostTime = std::max(0.0f, e.lostTime - dt);
                } else if (glm::vec2 d = inReach ? glm::vec2(0) : detour(g, e.body.pos, go, target.pos); d != glm::vec2(0)) {
                    in.move = d, in.run = true;  // round it (ADR-055)
                    e.lostTime += dt;
                } else {  // it waits, and gives up after giveUpSeconds
                    turnToward(e.body, dir, dt);
                    if (!inReach) e.lostTime += dt;
                }
                return in;
            }
            continue;
        case AiState::Windup:  // stands in its windup pose turning after the target, then commits to the strike; updateCombat lands it
            if (e.attack >= 0) {  // an attack of its own (ADR-057): telegraphed, then it goes; updateCombat lands it
                if (attackStep(e, g, dir, dist, dt, in)) {
                    setState(e, AiState::Stagger, "crashed");
                    continue;
                }
                if (e.body.punching()) return in;
                setState(e, AiState::Recover, kAttackTypeIds[size_t(t.attacks[size_t(e.attack)].type)]);
                continue;
            }
            if (e.body.hitPending) {
                Player& b = e.body;  // an archer's draw builds at its hold point, as the player's (ADR-051)
                if (b.move == Move::Shot && b.moveTime >= holdTime(b) && !b.charged && (b.charge += dt) >= gRanged.drawSeconds)
                    b.charged = true, b.noise(Noise::Charged);
                if (e.body.moveTime < holdTime(e.body) || e.body.hold > gMelee.trackSeconds) turnToward(e.body, dir, dt);
                return in;
            }
            if (e.string > 1 && !waits && e.body.punching() && target.alive() && dist < kPunchReach + 1) {  // a string, the target near:
                if (moveProgress(e.body) >= spec(e.body).chain) nextInString(e, dir);  // the next hit from this one's chain point
                return in;
            }
            setState(e, AiState::Recover, e.body.move == Move::Shot ? "shot" : "swung");
            continue;
        case AiState::Roar:  // a boss's new phase (ADR-059): stands roaring, stagger-proof and taking no damage, then fights on
            if (e.stateTime == 0 || e.stateTime < e.type().roar) {  // (a tick at least: the rest of this update read the old phase)
                e.body.roar = std::clamp(e.stateTime / std::max(e.type().roar, 1e-3f), 1e-3f, 1.0f);
                return {};  // (stands: nothing answer() meant to do)
            }
            e.events = 0;
            setState(e, AiState::Chase, "roared");
            continue;
        case AiState::Stagger:  // reels (a stance break: open to a riposte), then fights on
            if (e.body.stagger > 0) return in;
            e.events = 0;  // the hit that staggered it is answered by fighting
            setState(e, fighter ? AiState::Chase : AiState::Flee, "steadied");  // (only wanderers think: a villager runs, ADR-056)
            continue;
        case AiState::Recover:
            if (e.stateTime < (e.attack >= 0 ? t.attacks[size_t(e.attack)].recover : t.recover)) return in;
            if (t.backOff > 0 && rand01(e.rng) < t.backOff) e.backOff = gAi.backOffSeconds;  // makes room (ADR-043)
            if (e.pack > gAi.attackers) e.token = false;  // its turn is over, others wait: the next sides pass hands the tokens out again (ADR-042)
            setState(e, AiState::Chase, "recovered");
            continue;
        case AiState::Flee:  // runs from the target a while (hit again: a while longer), then goes home
            if (!fighter && (e.events & EventDamaged)) {
                e.events = 0;
                setState(e, AiState::Flee, "damaged");
                continue;
            }
            if (e.stateTime >= kFleeSeconds) {
                setState(e, AiState::Return, "fled");
                continue;
            }
            if (glm::vec2 d = roomy(g, e.body.pos, -dir, target.pos) ? -dir : detour(g, e.body.pos, -dir, target.pos); d != glm::vec2(0)) {
                in.move = d;
                in.run = true;
            }
            return in;
        case AiState::Search:  // goes to where it last saw the target and looks round there, then gives up (ADR-055)
            if (!fighter) setState(e, AiState::Wander, "harmless");  // forced from the debug window
            else if (e.events & EventDamaged) {
                e.events = 0;
                setState(e, AiState::Chase, "damaged");
            } else if (!target.alive()) setState(e, AiState::Return, "target_down");
            else if (e.unseen == 0) setState(e, AiState::Chase, "found_target");
            else if (e.stateTime >= gAi.searchSeconds) setState(e, AiState::Return, "gave_up_search");
            else {
                glm::vec2 to = xz(e.lastSeen - e.body.pos);
                if (glm::length(to) > 1 && e.lostTime < t.giveUpSeconds) {  // runs there
                    glm::vec2 d = glm::normalize(to);
                    if (roomy(g, e.body.pos, d, target.pos)) e.lostTime = std::max(0.0f, e.lostTime - dt);
                    else d = detour(g, e.body.pos, d, target.pos), e.lostTime += dt;
                    in.move = d, in.run = true;
                } else  // there (or stuck): turns one way and the other, looking
                    e.body.yaw = std::remainder(e.body.yaw + dt * 1.6f * (std::fmod(e.stateTime, 4.0f) < 2 ? 1.0f : -1.0f), kTau);
                return in;
            }
            continue;
        case AiState::Return:  // walks home, ignoring the player; a hit brings it back (ADR-055), unless it gave up for want of a way to them and still has none
            if (!fighter && (e.events & EventDamaged)) {  // a villager hit on its way home runs again (ADR-056)
                e.events = 0;
                setState(e, AiState::Flee, "damaged");
                continue;
            }
            if (fighter && (e.events & EventDamaged) && (!territorial || inside) &&  // (a boss isn't drawn out of its territory)
                (inReach || std::string_view(e.history[(e.transitions - 1) % e.history.size()].reason) != "lost_target")) {
                e.events = 0;
                setState(e, AiState::Chase, "damaged");
                continue;
            }
            if (glm::length(toHome) <= std::max(e.wanderRadius, 1.0f)) {
                // Heals only after walking back: one that gives up at home (hit from beyond its chase range)
                // arrives in the same update, with stateTime still 0, and keeps its wounds.
                // A boss starts over, even one that never left home (ADR-059): no wounds to poke at from outside its territory.
                bool reset = (e.stateTime > 0 || territorial) && e.phase;
                if (e.stateTime > 0 || territorial) e.body.health = float(maxHealth(sheet(e.body)));
                if (reset) enterPhase(e, 0);
                e.calmTimer = t.reaggroCooldown;
                setState(e, AiState::Wander, "home");
                if (reset) return in;  // (`t` is the old phase's for the rest of this update)
                continue;
            }
            if (e.lostTime >= t.giveUpSeconds) {  // stuck on the way: wandering may get it unstuck
                setState(e, AiState::Wander, "cannot_get_home");
                continue;
            }
            if (glm::vec2 d = glm::normalize(toHome); roomy(g, e.body.pos, d, target.pos)) {
                in.move = d;
                e.lostTime = std::max(0.0f, e.lostTime - dt);
            } else {
                in.move = detour(g, e.body.pos, d, target.pos);  // round it, if it can (ADR-055)
                e.lostTime += dt;
            }
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

bool canSee(const Grid& world, const Player& from, const Player& target) {
    return clearLine(world, from.pos + glm::vec3(0, 1.5f, 0), target.pos + glm::vec3(0, 1.1f, 0));
}

void buryDead(std::vector<Entity>& entities, DeadSlots& dead, const Grid& world, const Player& player,
              const std::function<bool(glm::vec3)>& loaded) {
    for (const Entity& e : entities)
        if (e.spawn.id && !e.body.alive()) dead[e.spawn.id] |= 1u << e.slot;
    std::erase_if(entities, [&](const Entity& e) {
        if (e.body.alive() || e.loot != NoItem || glm::distance(e.body.pos, player.pos) < gAi.corpseGone) return false;
        return (loaded && !loaded(e.body.pos)) || !canSee(world, player, e.body);  // (ponytail: no view cone: unseen is behind something)
    });
}

// Sides for the fighters on the player: evenly spaced around it, in the order they stand, turned to
// fit where they are (the mean of their offsets), so none has to cross the others to get there.
// ponytail: every tick, O(n log n) in the fighters; a pack of dozens is still microseconds.
static void assignSides(std::vector<Entity>& entities, const Player& player) {
    std::vector<std::pair<float, Entity*>> pack;
    for (Entity& e : entities) {
        e.pack = 0;
        if (e.angry() && e.body.alive() && e.type().wanders && !ranged(e.body)) {  // (an archer's bow out: it keeps away, ADR-051)
            glm::vec2 d = xz(e.body.pos - player.pos);
            pack.emplace_back(std::atan2(d.x, d.y), &e);
        }
    }
    std::ranges::sort(pack, {}, &std::pair<float, Entity*>::first);
    float spacing = 2 * std::numbers::pi_v<float> / float(std::max<size_t>(pack.size(), 1));
    glm::vec2 mean{};
    for (size_t i = 0; i < pack.size(); ++i) {
        float off = pack[i].first - float(i) * spacing;
        mean += glm::vec2(std::sin(off), std::cos(off));
    }
    float base = std::atan2(mean.x, mean.y);
    for (size_t i = 0; i < pack.size(); ++i) {
        pack[i].second->side = std::remainder(base + float(i) * spacing, 2 * std::numbers::pi_v<float>);
        pack[i].second->pack = int(pack.size());
    }
    // Attack tokens (ADR-042): gAi.attackers of them may swing; one in the middle of its string (or
    // recovering from it) keeps its token, the rest go to those in the fight (within 6 blocks) that
    // have waited longest.
    for (Entity& e : entities) e.token = true;
    auto rank = [&](const Entity* e) {
        float dist = glm::distance(xz(e->body.pos), xz(player.pos));
        return std::tuple(e->state != AiState::Windup && e->state != AiState::Recover, dist > 6, -e->sinceSwing, dist);
    };
    std::vector<Entity*> order;
    for (auto& [angle, e] : pack) order.push_back(e);
    std::ranges::sort(order, {}, rank);
    for (size_t i = 0; i < order.size(); ++i) order[i]->token = int(i) < gAi.attackers;
}

// Living NPCs push each other apart (half each; one that stands where it was put isn't moved), never
// into terrain. ponytail: all pairs, fine for the tens an area holds; a grid of cells if it's hundreds.
static void spread(std::vector<Entity>& entities, const Grid& world, const std::function<bool(glm::vec3)>& loaded) {
    auto movable = [&](const Entity& e) { return e.type().wanders && (!loaded || loaded(e.body.pos)); };
    for (size_t i = 0; i < entities.size(); ++i)
        for (size_t j = i + 1; j < entities.size(); ++j) {
            Player &a = entities[i].body, &b = entities[j].body;
            glm::vec2 d = xz(b.pos - a.pos);
            float len = glm::length(d);
            if (len >= kNpcSpacing || !a.alive() || !b.alive() || std::abs(a.pos.y - b.pos.y) >= kHeight) continue;
            bool ma = movable(entities[i]), mb = movable(entities[j]);
            if (!ma && !mb) continue;
            glm::vec2 n = len > 1e-4f ? d / len : glm::vec2(std::sin(float(i)), std::cos(float(i)));  // same spot: any way
            float push = (kNpcSpacing - len) / float(ma + mb);
            if (glm::vec3 to = a.pos - glm::vec3(n.x, 0, n.y) * push; ma && !overlapsSolid(world, to)) a.pos = to;
            if (glm::vec3 to = b.pos + glm::vec3(n.x, 0, n.y) * push; mb && !overlapsSolid(world, to)) b.pos = to;
        }
}

void updateEntities(std::vector<Entity>& entities, const Grid& world, const Player& player, float dt,
                    const std::function<bool(glm::vec3)>& loaded) {
    assignSides(entities, player);
    for (Entity& e : entities) {
        if (loaded && !loaded(e.body.pos)) continue;  // its part of the world isn't in: it waits there
        e.speechTimer = std::max(0.0f, e.speechTimer - dt);
        e.shoutTimer = std::max(0.0f, e.shoutTimer - dt);
        e.calmTimer = std::max(0.0f, e.calmTimer - dt);
        e.stateTime += dt;
        e.body.pos = clearOfTerrain(world, e.body.pos);  // e.g. respawned at a home that was built over since
        if (!e.type().wanders) {  // stands where it was put: only dies (the dummy not even that)
            if (!e.body.alive() && e.state != AiState::Dead) setState(e, AiState::Dead, "died");
            else if (e.body.alive() && e.state != AiState::Idle) setState(e, AiState::Idle, e.state == AiState::Dead ? "respawned" : "stands");
            e.events = 0;
            continue;
        }
        e.sinceSwing = std::min(e.sinceSwing + dt, 99.0f);
        for (float* timer : {&e.guardTimer, &e.counterTimer, &e.backOff, &e.strafeTimer}) *timer = std::max(0.0f, *timer - dt);
        for (float& wait : e.attackWait) wait = std::max(0.0f, wait - dt);
        Input in = think(e, world, player, dt);
        e.events = 0;  // read by this update only: later hits arrive with the next
        // Its guard (ADR-043): up while it means to, and behind a shield whenever it's within 6 blocks;
        // only while chasing with nothing else to do.
        Player& b = e.body;
        bool was = b.guarding;
        bool near = glm::distance(xz(b.pos), xz(player.pos)) < 6;
        b.guarding = (e.guardTimer > 0 || (e.type().shield && near)) && e.state == AiState::Chase && !b.punching() && !b.rolling() && !in.roll && b.stagger <= 0 &&
                     (b.inv.equipped[SlotMainHand] || b.inv.equipped[SlotOffHand]) && !ranged(b);  // (a bow doesn't guard)
        if (b.guarding && !was) b.noise(Noise::GuardUp);
        b.guardBlend += ((b.guarding ? 1.0f : 0.0f) - b.guardBlend) * (1 - std::exp(-25 * dt));
        if (b.guarding) in.run = false;  // as the player's: a guard walks
        updatePlayer(b, world, in, dt);
    }
    spread(entities, world, loaded);
}

void separate(Player& player, const Grid& world, const std::vector<Entity>& entities) {
    for (const Entity& e : entities)
        if (e.id == player.heldBy && glm::distance(e.body.pos, player.pos) <= 3) {  // held by a grab (ADR-057): kept in front of it, facing it
            glm::vec3 to = e.body.pos + glm::vec3(std::sin(e.body.yaw), 0, std::cos(e.body.yaw)) * gAi.grabGap;
            if (!overlapsSolid(world, to)) player.pos = to;
            player.vel = {}, player.yaw = std::remainder(e.body.yaw + kTau / 2, kTau);
        }  // (farther, a teleport: updateCombat lets go)
    if (player.tuning.noclip) return;
    for (const Entity& e : entities) {
        if (e.id == player.heldBy) continue;
        if (e.body.move == Move::Charge && e.body.hitPending && e.body.moveTime >= holdTime(e.body) && e.body.hold <= 0)
            continue;  // a charge on its run goes through (ADR-057): a roll or a sidestep lets it pass
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

void talk(Entity& e, int questLine) {
    e.shoutTimer = 0;  // spoken to: it listens (anger sets it again after a shout)
    bool fresh = questLine != e.saying || e.spawn.lines.empty();  // the same quest line again: the marker's turn
    e.saying = size_t(questLine) < defs().questLines.size() && fresh ? questLine : -1;
    if (e.saying >= 0) {  // a quest's line for this marker: the same one until the quest moves on
        e.speechTimer = std::max(kSpeechSeconds, float(defs().questLines[size_t(e.saying)].text.size()) / 18);
        return;
    }
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
    if (!player.alive()) return nullptr;
    Entity* best = nullptr;
    float bestDist = kTalkRange;
    for (Entity& e : entities) {
        float d = glm::distance(e.body.pos, player.pos);
        if (!e.body.alive() && e.loot != NoItem && d < bestDist && canStow(player.inv, e.loot)) {  // else E talks instead
            best = &e;
            bestDist = d;
        }
    }
    return best;
}

bool takeLoot(Entity& e, Inventory& inv) {
    if (!stowItem(inv, e.loot)) return false;
    e.loot = NoItem;
    return true;
}

bool hurt(Player& b, int amount, bool crit, Status kind, DamageType type, bool guarded) {
    if (amount <= 0 || !b.alive()) return false;
    b.health = std::max(0.0f, b.health - float(amount));
    if (kind == Status::Count) b.lastDamage = amount, b.lastCrit = crit;  // the last hit's (a status's has its number)
    if (kind != Status::Poison) b.hitTime = 0;  // a poisoned back can still be stabbed
    b.combatTimer = kCombatSeconds;
    *std::ranges::max_element(b.numbers, {}, &DamageNumber::age) = {amount, 0, crit, kind, type, guarded};
    if (!b.alive()) b.effects.clear(), b.buildup = {};  // death ends them (ADR-046)
    return true;
}

bool damage(Entity& e, int amount, bool crit, Status kind, DamageType type, bool guarded) {
    const EntityType& t = e.type();
    if (!t.attackable || !e.body.alive() || e.state == AiState::Roar) return false;  // (a roaring boss takes none, ADR-059)
    if (t.immortal) e.body.health += float(std::max(amount, 0));  // takes none (ADR-056); hurt() still shows the number and counts the hit
    if (!hurt(e.body, amount, crit, kind, type, guarded)) return false;
    if (kind != Status::Poison) e.events |= EventDamaged;  // poison's ticks don't call it back
    if (!e.body.alive()) {  // the usual drop, else (ADR-049) a rare one; rand01 may return 1
        if (t.lootChance >= 1 || rand01(e.rng) < t.lootChance) e.loot = t.loot;
        else if (t.rareChance > 0 && (t.rareChance >= 1 || rand01(e.rng) < t.rareChance)) e.loot = t.rareLoot;
    }
    return true;
}

bool startPunch(Player& b, glm::vec2 aim, float windup) {
    if (!b.alive() || b.swingTimer > 0 || b.rolling() || b.stagger > 0) return false;
    bool chains = b.lastMove >= Move::Light1 && b.lastMove < Move::Light3;  // the first three lights in turn
    startMove(b, chains ? Move(int(b.lastMove) + 1) : Move::Light1, aim);
    b.hold = std::max(0.0f, windup - impactTime(b));
    b.noise(Noise::Windup);  // the tell: heard before the swing
    b.swingTimer += swingTime(sheet(b, skillOf(b.swingWeapon)), weaponAttack(b.swingWeapon));  // keeps the overshoot of the last frame
    return true;
}

bool inReach(const Player& a, glm::vec3 target) {
    const WeaponClassDef& w = weaponClass(classOf(a.weapon()));  // a sword's: 2 blocks, 60 degrees; great weapons more (ADR-049)
    glm::vec2 forward{std::sin(a.yaw), std::cos(a.yaw)};
    glm::vec2 d = xz(target - a.pos);
    float dist = glm::length(d);
    bool inFront = dist < 1e-3f || glm::dot(d / dist, forward) > w.arcCos;
    return inFront && dist < w.reach && std::abs(target.y - a.pos.y) < kHeight;
}

Entity* punchTarget(std::vector<Entity>& entities, const Player& a) {
    Entity* best = nullptr;
    float bestDist = 1e9f;  // inReach says how far
    for (Entity& e : entities) {
        float dist = glm::distance(xz(e.body.pos), xz(a.pos));
        if (e.type().attackable && e.body.alive() && inReach(a, e.body.pos) && dist < bestDist) {
            best = &e;
            bestDist = dist;
        }
    }
    return best;
}

bool behind(const Entity& e, const Player& a) {
    glm::vec2 back{-std::sin(e.body.yaw), -std::cos(e.body.yaw)}, d = xz(a.pos - e.body.pos);
    float dist = glm::length(d);
    return dist > 1e-3f && glm::dot(d / dist, back) > gMelee.backstabAngle;
}

bool opensToCritical(const Entity& e, const Player& a) {
    const Player& b = e.body;
    if (bystander(e.type())) return false;  // (a villager near a fight mustn't pull the player's attack onto it, ADR-056)
    if (e.state == AiState::Roar) return false;  // (stagger-proof, ADR-059)
    return b.broken || (behind(e, a) && !b.punching() && !b.rolling() && b.stagger == 0 && b.hitTime >= gMelee.backstabWait);
}

Entity* criticalTarget(std::vector<Entity>& entities, const Player& a, bool caught) {
    Entity* best = nullptr;
    float bestDist = 1e9f;
    for (Entity& e : entities) {
        float dist = glm::distance(xz(e.body.pos), xz(a.pos));
        const Player& b = e.body;
        bool open = caught ? e.state != AiState::Roar && (b.broken || (behind(e, a) && !b.rolling() && b.hitTime >= gMelee.backstabWait))
                           : opensToCritical(e, a);  // (a roaring boss is stagger-proof, ADR-059: not held either)
        if (e.type().attackable && e.body.alive() && open && inReach(a, e.body.pos) && dist < bestDist) best = &e, bestDist = dist;
    }
    return best;
}

void holdCritical(std::vector<Entity>& entities, const Player& a) {
    if (a.move == Move::Riposte && a.hitPending)
        if (Entity* e = criticalTarget(entities, a, true); e && !e->body.broken) interrupt(e->body, 0.1f, false);
}

// An NPC's attack of its own with hyperarmor (ADR-057), gone and not yet landed: a hit doesn't make it flinch.
static bool armored(const Player& b) {
    return attackMove(b.move) && spec(b).heavy && b.hitPending && b.moveTime >= holdTime(b) && b.hold <= 0;
}

void staggerHit(Entity& e, float stance, bool critical) {
    const EntityType& t = e.type();
    Player& b = e.body;
    if (e.state == AiState::Roar) return;  // a boss roaring into its next phase is stagger-proof (ADR-059)
    if (critical) {  // knocked down, its stance back
        interrupt(b, 0, false);
        b.stagger = b.staggerLength = gMelee.downSeconds;
        b.broken = false;
        b.stance = t.stance;
        return;
    }
    if (b.broken || stance <= 0) return;
    if (t.stance > 0) {  // 0: it never breaks (but may still flinch)
        b.stance -= stance;
        b.stanceWait = t.stance / 13;  // as Elden Ring's: it starts coming back after base/13 s, at 13 a second
    }
    if (t.stance > 0 && b.stance <= 0) {
        b.stance = t.stance;
        interrupt(b, gMelee.breakSeconds, true);
        b.noise(Noise::StanceBreak);
    } else if (stance >= t.poise && !armored(b)) {
        interrupt(b, gMelee.flinchSeconds, false);
    }
}

int afflict(Player& b, Status s, std::string_view source) {
    if (!b.alive()) return 0;
    const StatusTuning& t = gStatus;
    float most = float(maxHealth(sheet(b)));
    auto put = [&](Effect kind, float seconds, std::vector<Modifier> mods) {
        std::erase_if(b.effects, [&](const TimedEffect& e) { return e.kind == kind; });
        b.effects.push_back({kind, std::string(source), seconds, seconds, 1, std::move(mods), 1});
    };
    b.buildup[size_t(s)] = 0;
    switch (s) {
    case Status::Bleed: b.noise(Noise::Bleed); return int(std::lround(t.bleedShare * most + t.bleedFlat));
    case Status::Poison: put(Effect::Poisoned, t.poisonSeconds, {}), b.noise(Noise::Poison); return 0;
    case Status::Frost:
        put(Effect::Frostbitten, t.frostSeconds, {{Stat::DamageTaken, ModKind::Increased, t.frostTaken}}), b.noise(Noise::Frost);
        return int(std::lround(t.frostShare * most + t.frostFlat));
    case Status::Count: break;
    }
    return 0;
}

std::array<int, size_t(Status::Count)> buildUp(const Combatant& attacker, Player& b, float share, std::string_view source,
                                               const std::array<float, size_t(Status::Count)>* own) {
    std::array<int, size_t(Status::Count)> burst{};
    if (share <= 0 || !b.alive()) return burst;
    StatSheet a = sheet(attacker, skillOf(attacker.swingWeapon)), d = sheet(b);
    for (size_t i = 0; i < burst.size(); ++i) {
        Status s = Status(i);
        float add = (own ? (*own)[i] : a[buildupStat(s)]) * share;
        if (add <= 0 || (s != Status::Bleed && b.effect(s == Status::Poison ? Effect::Poisoned : Effect::Frostbitten))) continue;
        if ((b.buildup[i] += add) >= d[resistStat(s)]) burst[i] = afflict(b, s, source);
    }
    return burst;
}

void drink(Player& b) {
    if (b.flasks <= 0) return b.noise(Noise::Empty);
    StatSheet s = sheet(b);
    --b.flasks;
    b.health = std::min(b.health + s[Stat::FlaskHealing] * float(maxHealth(s)), float(maxHealth(s)));
    b.noise(Noise::Drink);
}

void rest(Player& b) {
    StatSheet s = sheet(b);
    b.flasks = maxFlasks(s);
    b.arrows = std::max(b.arrows, kQuiver);  // the quiver full again (ADR-050)
    fillPouch(b);                            // and the pouch (ADR-053)
    b.health = float(maxHealth(s));
    b.stamina = gMelee.staminaMax;
    b.effects.clear(), b.buildup = {};
    b.noise(Noise::Rest);
}

// The clocks of `b`'s body: its swing, move, reeling and combat timers, its status build-up and timed
// effects, its regeneration. Returns the poison damage due this tick, for the caller to deal.
static int tick(Player& b, float dt) {
    // May go up to one frame negative, so back-to-back swings don't lose time to frame rounding.
    b.swingTimer = std::max(b.swingTimer - dt, -dt);
    advanceMove(b, dt);
    b.stagger = std::max(0.0f, b.stagger - dt);
    if (b.stagger == 0) b.broken = false;
    b.hitTime += dt;
    b.combatTimer = std::max(0.0f, b.combatTimer - dt);
    b.trauma = std::max(0.0f, b.trauma - dt);  // a full shake wears off in a second
    for (DamageNumber& n : b.numbers) n.age = std::min(n.age + dt, 99.0f);
    for (float& v : b.buildup) v = std::max(0.0f, v - gStatus.decay * dt);
    StatSheet s = sheet(b);
    if (!b.alive()) b.deadTime += dt;
    else if (b.combatTimer == 0) b.health += healthRegen(s) * dt;
    b.health = std::min(b.health, float(maxHealth(s)));
    int poison = 0;
    for (TimedEffect& e : b.effects) {
        e.seconds -= dt;
        if (e.kind == Effect::Poisoned && (e.tick -= dt) <= 1e-4f)  // (the last tick lands as it runs out)
            e.tick += 1, poison += e.stacks * int(std::lround(gStatus.poisonShare * float(maxHealth(s)) + gStatus.poisonFlat));
    }
    std::erase_if(b.effects, [](const TimedEffect& e) { return e.seconds <= 1e-4f; });
    return poison;
}

// True once per swing, at its impact frame. A swing dies with its swinger.
static bool lands(Player& b) {
    if (!b.alive()) b.hitPending = false;
    if (!b.hitPending || b.moveTime < impactTime(b)) return false;
    b.hitPending = false;
    return true;
}

int resolveHit(const Combatant& attacker, const Combatant& defender, const Attack& attack, const HitContext& ctx) {
    StatSheet a = sheet(attacker, skillOf(attacker.swingWeapon)), d = sheet(defender);
    float hit = float(rollDamage(a, attack, ctx.rng)) * a[Stat::Damage] * ctx.power;
    return mitigate(hit, d[Stat::Armor], attacker.level, ctx.armorK, d[Stat::DamageTaken]);
}

// Whether the swing `attacker` lands now is a random critical (ADR-045), by its crit chance with the swing's weapon skill.
static bool rollCrit(const Combatant& attacker, std::mt19937& rng) {
    float chance = sheet(attacker, skillOf(attacker.swingWeapon))[Stat::CritChance];
    return chance > 0 && (rand01(rng) < chance || chance >= 1);  // rand01 may return 1; draws either way
}
static float critDamage(const Combatant& attacker) { return 1 + sheet(attacker, skillOf(attacker.swingWeapon))[Stat::CritDamage]; }
static void shake(Player& p, float trauma) { p.trauma = std::min(1.0f, p.trauma + trauma); }

// A kill's character XP: the creature's `xp`, worn by the player's hits on it lately (`wear`).
static void awardKillXp(Combatant& player, const Entity& npc, float wear) {
    addCharacterXp(player.progress, player.level, int(std::lround(float(npc.type().xp) * std::exp2(-wear / kXpWearHalving))));
}

void awardHitXp(Combatant& player, Entity& npc, bool playerHit, bool killed) {
    const EntityType& t = npc.type();
    if (!t.attackable) return;  // can't be hurt or killed: nothing to learn from (the Skyrim companion exploit)
    float& wear = playerHit ? npc.xpWear : npc.armorWear;
    float before = wear, worn = std::exp2(-wear / kXpWearHalving), rate = (t.temper == Temper::Harmless ? kTrainingRate : 1) * worn;
    auto use = [&](Skill s, const Attack& a) {  // base damage, so gear, strength and talents don't speed it up
        const SkillDef& d = skillDef(s);
        addSkillXp(player.progress, player.level, s, skillUseXp(d.useMult, d.useOffset, float(a.minDamage + a.maxDamage) / 2) * rate);
        wear += 1;
    };
    if (playerHit) {
        use(skillOf(player.swingWeapon), weaponAttack(player.swingWeapon));
        if (killed) awardKillXp(player, npc, before);
    } else if (sheet(player)[Stat::Armor] > 0) {
        use(Skill::Armor, weaponAttack(npc.body.swingWeapon));
    }
}

void strike(Player& player, Entity& target, const Strike& s, std::mt19937& rng, float armorK, std::vector<GameEvent>* events) {
    const MeleeTuning& t = gMelee;
    Player& b = target.body;
    // A random critical (ADR-045) by the attacker's crit chance; a riposte or backstab always is one.
    bool crit = s.riposte || (s.canCrit && rollCrit(player, rng));
    float power = s.power, stance = s.stance;
    if (crit && !s.riposte) power *= critDamage(player);
    power *= 1 - target.type().resist[size_t(s.type)];  // its weakness or resistance (ADR-040)
    int full = resolveHit(player, b, weaponAttack(player.swingWeapon), {rng, armorK, power});
    std::optional<Guard> guard = !s.riposte ? guardAgainst(b, s.from, s.type) : std::nullopt;
    if (guard) {  // its guard (ADR-043), as the player's: part of the damage by type, its stamina; empty, it breaks
        b.stamina -= (t.blockStamina + t.blockStaminaPerDamage * float(full)) * (1 - guard->boost);
        b.staminaWait = t.staminaDelay;
        b.combatTimer = player.combatTimer = kCombatSeconds;
        b.noise(Noise::Block);
        if (s.melee) player.freeze = b.freeze = 0.06f;
        target.events |= EventDamaged;
        if (b.stamina <= 0) {
            b.stamina = 0;
            interrupt(b, t.guardBreakSeconds, false);
            b.noise(Noise::GuardBreak);
        } else {
            target.counterTimer = gAi.counterWindow;
            if (s.melee && bounces(*guard, stance)) interrupt(player, t.recoilSeconds, false), b.noise(Noise::Bounce);
        }
        full = int(std::lround(float(full) * (1 - guard->negation)));
        stance = 0;  // a guard takes the stance damage on its stamina
    }
    bool landed = damage(target, full, crit && !guard, Status::Count, s.type, guard.has_value());  // a blocked crit is marked as the block
    if (landed || guard) {  // statuses build up by what a guard let through, a light hit's too (ADR-046)
        std::string_view with = player.swingWeapon ? itemType(player.swingWeapon).name : "Fists";
        auto bursts = buildUp(player, b, guard ? 1 - guard->negation : 1, with, s.buildup ? &*s.buildup : nullptr);
        for (size_t k = 0; k < bursts.size(); ++k) landed |= damage(target, bursts[k], false, Status(k));
    }
    if (!landed) return;
    player.combatTimer = kCombatSeconds;
    bool killed = !b.alive();
    awardHitXp(player, target, true, killed);
    if (killed || !guard) b.noise(killed ? Noise::Death : Noise::Hurt);  // a blocked hit sounds as the block
    if (crit && !guard) b.noise(Noise::Crit);
    b.hitWith = player.swingWeapon;
    b.hitFrom = s.from;
    if (killed && events) events->push_back(Killed{target.kind, target.spawn.id});
    b.hitStance = s.riposte ? kRiposteStance : stance;
    if (!killed) staggerHit(target, stance, s.riposte);
    if (!guard && s.melee) {  // hit-stop: heavier hits, longer, and longer or shorter by the weapon class (ADR-044)
        player.freeze = b.freeze = std::min(0.04f + 0.003f * stance, 0.12f) * weaponClass(classOf(player.swingWeapon)).hitStop;
        // Camera shake (ADR-045): a riposte or backstab's full, a random crit's more than its stance's, heavy hits a little.
        shake(player, t.hitShake * (s.riposte ? 1 : std::max(crit ? 0.6f : 0.0f, std::clamp((stance - 20) / 60, 0.0f, 1.0f))));
    }
}

void strikePlayer(Entity& e, Player& player, const Strike& s, std::mt19937& rng, float armorK) {
    e.body.combatTimer = kCombatSeconds;
    bool crit = rollCrit(e.body, rng);  // a random critical (ADR-045)
    int full = resolveHit(e.body, player, weaponAttack(e.body.swingWeapon), {rng, armorK, s.power * (crit ? critDamage(e.body) : 1)});
    const MeleeTuning& t = gMelee;
    bool guarded = false;
    float through = 1;  // the share a guard let through (status build-up)
    if (s.melee && s.parryable && e.type().parryable && parries(player, e.body.pos)) {  // deflected (ADR-036): the last of its parries opens it
        player.combatTimer = kCombatSeconds;
        if (++e.parried >= e.type().parries) e.parried = 0, interrupt(e.body, t.parrySeconds, true), e.body.noise(Noise::StanceBreak);
        else interrupt(e.body, t.recoilSeconds, false);
        player.noise(Noise::Parry);
        player.freeze = e.body.freeze = 0.1f;
        full = 0;
    } else if (std::optional<Guard> guard = s.guardable ? guardAgainst(player, s.from, s.type) : std::nullopt) {
        // Blocked: part of the damage (by its type), and stamina; a guard raised just now takes none (ADR-041).
        bool perfect = perfectGuard(player);
        player.stamina -= (t.blockStamina + t.blockStaminaPerDamage * float(full)) * (1 - guard->boost) * (perfect ? t.perfectStamina : 1) * s.blockStamina;
        player.staminaWait = t.staminaDelay;
        player.combatTimer = kCombatSeconds;
        player.noise(Noise::Block), guarded = true;
        if (player.stamina < 0)  // guard broken
            player.stamina = 0, interrupt(player, t.guardBreakSeconds, true), player.noise(Noise::GuardBreak), shake(player, 0.3f);
        else if (player.guarding) player.blockTime = 0;  // a guard counter only off a real guard, not a parry's weak one
        if (s.melee) {
            e.body.freeze = 0.08f;
            if (player.stamina > 0 && bounces(*guard, s.stance)) interrupt(e.body, t.recoilSeconds, false), player.noise(Noise::Bounce);  // off a greatshield
        }
        perfect &= !player.broken;  // a guard that broke wasn't perfect
        full = perfect ? 0 : int(std::lround(float(full) * (1 - guard->negation)));
        through = perfect ? 0 : 1 - guard->negation;
        if (perfect) player.noise(Noise::PerfectGuard);
    } else if (full > 0 && !hyperarmor(player) && s.stance >= playerPoise(player)) {
        interrupt(player, gMelee.flinchSeconds, false);
    }
    bool landed = hurt(player, full, crit && !guarded, Status::Count, s.type, guarded);
    if (landed || guarded) {  // statuses build up by what the guard let through, a light hit's too (ADR-046)
        auto bursts = buildUp(e.body, player, through, e.type().name ? e.type().name : e.name.c_str());
        for (size_t k = 0; k < bursts.size(); ++k) landed |= hurt(player, bursts[k], false, Status(k));
    }
    if (landed) {
        awardHitXp(player, e, false, false);
        if (!player.alive()) player.noise(Noise::Death);
        else if (!guarded) player.noise(Noise::Hurt);  // a blocked hit sounds as the block
        if (crit && !guarded) player.noise(Noise::Crit);
        shake(player, std::min(0.6f, 1.5f * float(full) / float(maxHealth(sheet(player)))));  // by the share of its health it took
        player.hitWith = e.body.swingWeapon, player.hitStance = s.stance;
        player.hitFrom = s.from;
    }
}

// An attack of `e`'s own (ADR-057), every tick of it: a charge hits on first contact in its run (a roll lets it
// through), a leap as it lands within its reach, a sweep all around at its impact, a grab catches at its impact (in
// front, past any guard or parry, not through a roll) and then bites, the last throwing the player down.
static void attackLands(Entity& e, Player& player, std::mt19937& rng, float armorK) {
    Player& b = e.body;
    const EntityType& t = e.type();
    const CreatureAttack& a = t.attacks[size_t(e.attack)];
    glm::vec2 d = xz(player.pos - b.pos), fwd{std::sin(b.yaw), std::cos(b.yaw)};
    float dist = glm::length(d);
    bool ahead = dist < 1e-3f || glm::dot(d / dist, fwd) > 0.5f;
    bool open = player.alive() && !invulnerable(player) && std::abs(player.pos.y - b.pos.y) < kHeight &&
                (!player.heldBy || player.heldBy == e.id);  // not rolling through it, about level, not another's
    auto hit = [&](bool guardable, bool parryable) {
        Strike s{a.power, a.stance, b.pos, damageOf(b, t.damage)};
        s.guardable = guardable, s.parryable = parryable, s.blockStamina = a.blockStamina;
        strikePlayer(e, player, s, rng, armorK);
    };
    switch (a.type) {
    case AttackType::Charge:
        if (b.hitPending && b.moveTime >= holdTime(b) && b.hold <= 0 && dist < gAi.chargeContact && ahead && open)
            b.hitPending = false, b.moveTime = std::max(b.moveTime, impactTime(b)), hit(true, true);  // it stops on the hit
        lands(b);  // its run over without touching the player: a miss
        break;
    case AttackType::Leap:
        if (lands(b) && dist < a.reach && open) hit(true, true);
        break;
    case AttackType::Area:
        if (lands(b) && dist < a.radius && open) hit(true, false);  // all around; a guard takes it from the front
        break;
    case AttackType::Grab:
        if (!lands(b)) {
        } else if (dist < a.reach && ahead && open && !player.heldBy) {  // caught: held through its bites
            player.heldBy = e.id, e.bites = 0;
            interrupt(player, float(a.hits) * a.interval + 0.2f, false);
        } else {  // missed: it stumbles out of it (its last 0.3 s), then its recovery
            b.moveTime = std::max(b.moveTime, moveLength(b) - 0.3f);
        }
        while (player.heldBy == e.id && e.bites < a.hits && b.moveTime - impactTime(b) >= float(e.bites + 1) * a.interval) {
            ++e.bites;
            hit(false, false);
            if (e.bites == a.hits || !player.alive())  // the last bite throws the player down
                player.heldBy = 0, player.stagger = player.staggerLength = gAi.grabThrow;
        }
        break;
    case AttackType::Count: break;
    }
}

void updateCombat(Player& player, std::vector<Entity>& entities, float dt, std::mt19937& rng, float armorK,
                  std::vector<GameEvent>* events) {
    if (hurt(player, tick(player, dt), false, Status::Poison) && !player.alive()) player.noise(Noise::Death);
    bool strikes = lands(player);
    if (strikes && player.move == Move::Drink) strikes = false, drink(player);  // the flask's drink frame (ADR-047)
    if (strikes && player.move == Move::Throw) strikes = false, player.hurled = true;  // let go: step() throws it (ADR-053)
    if (strikes && player.move == Move::Shot) {  // a bow's release: step() looses it (ADR-050)
        strikes = false, player.loosed = true;
        if (weaponClass(classOf(player.swingWeapon)).loads) player.want = Want::Light, player.wantAge = 0, player.reloadWant = true;  // a crossbow reloads next, like a press (ADR-052)
    }
    if (strikes && player.move == Move::Reload) {  // a crossbow's bolt seated (ADR-052), from the quiver
        strikes = false;
        if (player.arrows > 0) --player.arrows, player.loaded = player.swingWeapon, player.noise(Noise::Charged);
    }
    if (strikes) {
        const MoveSpec& m = spec(player);
        // A riposte lands on the broken stance it was for, a backstab on the back it caught; if that's gone it's a plain hit.
        bool riposte = player.move == Move::Riposte;
        Entity* target = riposte ? criticalTarget(entities, player, true) : nullptr;
        if (!target) target = punchTarget(entities, player);
        riposte &= target && (target->body.broken || behind(*target, player));
        if (target && invulnerable(target->body)) target = nullptr;  // it rolled through the hit (ADR-043)
        const MeleeTuning& t = gMelee;
        if (target && !m.heavy && player.move != Move::Riposte && parries(target->body, player.pos)) {
            // Parried by it (ADR-043): the player reels, and its answer comes fast. Heavies can't be.
            Player& b = target->body;
            interrupt(player, gAi.parriedSeconds, false);
            player.combatTimer = b.combatTimer = kCombatSeconds;
            player.freeze = b.freeze = 0.1f;
            b.noise(Noise::Parry);
            target->counterTimer = gAi.counterWindow;
            target->events |= EventDamaged;  // a passive one fights back
            target = nullptr;
        }
        if (target) {  // the riposte move whose opening is gone hits as a plain light
            bool plain = player.move == Move::Riposte && !riposte;
            Strike st;
            st.power = plain ? 1 : m.damage * (player.charged ? t.chargeDamage : 1);
            st.stance = plain ? spec(player, Move::Light1).stance : m.stance * (player.charged ? t.chargeStance : 1);
            st.from = player.pos;
            st.type = damageOf(player);
            st.riposte = riposte;
            st.canCrit = player.move != Move::Riposte;  // it is one, or a plain hit
            strike(player, *target, st, rng, armorK, events);
        }
    }
    if (player.heldBy) {  // a grab lets go once its grabber stops (ADR-057): a flinch, a stance break, its death, the player's
        auto it = std::ranges::find(entities, player.heldBy, &Entity::id);
        if (!player.alive() || it == entities.end() || !it->body.alive() || it->body.move != Move::Grab ||
            glm::distance(it->body.pos, player.pos) > 3)  // (moved away: a teleport)
            player.heldBy = 0, player.stagger = std::min(player.stagger, 0.2f);
    }
    for (Entity& e : entities) {
        if (int poison = tick(e.body, dt); damage(e, poison, false, Status::Poison) && !e.body.alive()) {  // the player's kill
            e.body.noise(Noise::Death);
            awardKillXp(player, e, e.xpWear);
            if (events) events->push_back(Killed{e.kind, e.spawn.id});
        }
        e.xpWear = std::max(0.0f, e.xpWear - dt / kXpWearSeconds);
        e.armorWear = std::max(0.0f, e.armorWear - dt / kXpWearSeconds);
        if (e.body.stanceWait > 0) e.body.stanceWait = std::max(0.0f, e.body.stanceWait - dt);
        else e.body.stance = std::min(e.body.stance + 13 * dt, e.type().stance);
        if (e.body.staminaWait > 0) e.body.staminaWait = std::max(0.0f, e.body.staminaWait - dt);  // what its guard spends (ADR-043)
        else e.body.stamina = std::min(e.body.stamina + gMelee.staminaRegen * (e.body.guarding ? gMelee.guardRegen : 1) * dt, gMelee.staminaMax);
        if (e.attack >= int(e.type().attacks.size())) dropAttack(e);  // (defs reloaded with fewer)
        if (e.attack >= 0 && attackMove(e.body.move)) {  // an attack of its own (ADR-057)
            attackLands(e, player, rng, armorK);
            continue;
        }
        bool swings = lands(e.body);
        bool waits = player.heldBy && player.heldBy != e.id;  // another holds the player (ADR-057): its draw is let down
        if (swings && e.body.move == Move::Shot) swings = false, e.body.loosed = !waits;  // an archer's release: step() looses it (ADR-051)
        if (swings && !waits && inReach(e.body, player.pos) && player.alive() && !invulnerable(player))
            strikePlayer(e, player, {1, e.type().hitStance, e.body.pos, damageOf(e.body, e.type().damage)}, rng, armorK);
    }
}
