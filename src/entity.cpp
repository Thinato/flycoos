#include "entity.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace {

constexpr float kTau = 2 * std::numbers::pi_v<float>;
constexpr float kSpeechSeconds = 5;
constexpr float kBodyRadius = 0.8f;  // player-to-entity distance kept by separate()

constexpr EntityType kTypes[] = {
    // name              lvl  hp   attackable wanders talks model
    {"Training Dummy",   1,   100, true,      false,  false, ModelDummy},
    {nullptr,            5,   250, false,     true,   true,  ModelVillager},
};

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
    if (e.wanderDir != glm::vec2(0) && glm::length(toHome) > kWanderRadius && glm::dot(e.wanderDir, toHome) < 0)
        e.wanderDir = glm::normalize(toHome);
    if (e.wanderDir != glm::vec2(0) && !walkable(g, e.body.pos, e.wanderDir, player)) {
        e.wanderDir = {};  // stop and think again soon
        e.wanderTimer = std::min(e.wanderTimer, 1.0f);
    }
}

}  // namespace

const EntityType& typeOf(EntityKind kind) { return kTypes[int(kind)]; }

const char* Entity::speech() const { return speechTimer > 0 && phrase >= 0 ? kPhrases[phrase] : nullptr; }

std::vector<Entity> spawnEntities(const Grid& world, glm::vec3 near, uint32_t seed) {
    std::vector<Entity> out;
    auto add = [&](EntityKind kind, glm::vec2 offset) {
        Entity e{};
        e.kind = kind;
        e.rng.seed(seed * 2654435761u + uint32_t(out.size()));
        const EntityType& t = e.type();
        e.name = t.name ? t.name : kNames[e.rng() % std::size(kNames)];
        e.level = t.level;
        e.body.health = e.body.maxHealth = t.maxHealth;
        e.body.tuning.walkSpeed = 2;  // a stroll
        e.body.breathPhase = kTau * rand01(e.rng);
        // Nearest grass to the offset, but not on top of the player or an earlier entity.
        auto crowded = [&](glm::vec3 p) {
            bool c = glm::distance(xz(p), xz(near)) < 1.5f;
            for (const Entity& o : out) c |= glm::distance(xz(p), xz(o.body.pos)) < 1.5f;
            return c;
        };
        for (int tries = 0; tries < 8; ++tries, offset.x += 2) {
            e.body.pos = findSpawn(world, glm::ivec2(glm::floor(xz(near) + offset)));
            if (!crowded(e.body.pos)) break;
        }
        glm::vec2 toNear = xz(near) - xz(e.body.pos);
        if (glm::length(toNear) > 0.01f) e.body.yaw = std::atan2(toNear.x, toNear.y);
        e.home = e.body.pos;
        out.push_back(e);
    };
    // In front of the starting camera, which looks toward +Z.
    add(EntityKind::TrainingDummy, {3, 6});
    add(EntityKind::Villager, {-4, 7});
    return out;
}

void updateEntities(std::vector<Entity>& entities, const Grid& world, const Player& player, float dt) {
    for (Entity& e : entities) {
        e.speechTimer = std::max(0.0f, e.speechTimer - dt);
        if (!e.type().wanders) continue;
        bool listening = e.speechTimer > 0;  // stands still and faces the player while talking
        if (!listening) wander(e, world, player.pos, dt);
        Input in;
        if (!listening) in.move = e.wanderDir;
        updatePlayer(e.body, world, in, dt);
        glm::vec2 toPlayer = xz(player.pos - e.body.pos);
        if (listening && glm::length(toPlayer) > 0.01f) turnToward(e.body, toPlayer, dt);
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
        if (e.type().talks && d < bestDist) {
            best = &e;
            bestDist = d;
        }
    }
    return best;
}

void talk(Entity& e) {
    int n = int(std::size(kPhrases)), p;
    do p = int(e.rng() % unsigned(n));
    while (p == e.phrase);
    e.phrase = p;
    e.speechTimer = kSpeechSeconds;
}

bool damage(Entity& e, int amount) {
    if (!e.type().attackable || amount <= 0 || e.body.health == 0) return false;
    e.body.health = std::max(0, e.body.health - amount);
    return true;
}
