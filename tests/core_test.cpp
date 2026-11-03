// Assert-style checks for the engine core. Not using <cassert>: release builds define NDEBUG.
#include <algorithm>
#include <bit>
#include <chrono>
#include <climits>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <print>
#include <ranges>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "commands.h"
#include "edit.h"
#include "entity.h"
#include "mesher.h"
#include "prefab.h"
#include "session.h"

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::println(stderr, "FAIL {}:{}: {}", __FILE__, __LINE__, #cond);   \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static int countFaces(const Mesh& m, int face, int y) {
    int n = 0;
    for (Quad q : m.opaque) {
        QuadInfo i = unpack(q);
        n += i.face == face && i.pos.y == y;
    }
    return n;
}

static void meshing() {
    {  // single block -> 6 fully lit quads
        Grid g({3, 3, 3});
        g.set({1, 1, 1}, Stone);
        Mesh m = meshRegion(g, {0, 0, 0}, g.size);
        CHECK(m.opaque.size() == 6);
        CHECK(m.water.empty());
        for (Quad q : m.opaque) CHECK((unpack(q).ao == std::array{3, 3, 3, 3}));
    }
    {  // 2x1x1 slab -> greedy merge keeps it at 6 quads
        Grid g({4, 3, 3});
        g.set({1, 1, 1}, Stone);
        g.set({2, 1, 1}, Stone);
        Mesh m = meshRegion(g, {0, 0, 0}, g.size);
        CHECK(m.opaque.size() == 6);
        CHECK(countFaces(m, 2, 1) == 1);
    }
    {  // a block diagonal to one slab voxel changes only that top face's AO -> no merge
        Grid g({4, 4, 3});
        g.set({1, 1, 1}, Stone);
        g.set({2, 1, 1}, Stone);
        g.set({0, 2, 1}, Stone);
        Mesh m = meshRegion(g, {0, 0, 0}, g.size);
        CHECK(countFaces(m, 2, 1) == 2);
    }
    {  // water: only faces against air, kept in the separate water list
        Grid g({3, 3, 3});
        g.set({1, 1, 1}, Water);
        g.set({1, 0, 1}, Stone);
        Mesh m = meshRegion(g, {0, 0, 0}, g.size);
        CHECK(m.water.size() == 5);  // bottom face touches stone
    }
    {  // meshing a sub-region reads neighbours outside it (no faces between chunks)
        Grid g({4, 1, 1});
        for (int x = 0; x < 4; ++x) g.set({x, 0, 0}, Stone);
        Mesh left = meshRegion(g, {0, 0, 0}, {2, 1, 1});
        bool hasInnerFace = false;
        for (Quad q : left.opaque) hasInnerFace |= unpack(q).face == 0;
        CHECK(!hasInnerFace);
    }
}

static void packing() {
    QuadInfo cases[] = {
        {{0, 0, 0}, 1, 1, 0, {0, 0, 0, 0}, Grass},
        {{31, 31, 31}, 32, 32, 5, {3, 2, 1, 0}, Eye},
        {{7, 19, 2}, 13, 4, 3, {1, 3, 0, 2}, Water},
    };
    for (const QuadInfo& q : cases) CHECK(unpack(pack(q)) == q);
}

static Grid flatWorld(int wallHeight) {
    Grid g({16, 16, 16});
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            for (int y = 0; y < 4; ++y) g.set({x, y, z}, Stone);  // ground top at y = 4
            for (int y = 4; y < 4 + wallHeight; ++y) g.set({10, y, z}, Stone);
        }
    return g;
}

static void physics() {
    {  // falling lands on the ground
        Grid g = flatWorld(0);
        Player p;
        p.pos = {5.5f, 10, 8.5f};
        for (int i = 0; i < 240; ++i) updatePlayer(p, g, {}, 1 / 120.0f);
        CHECK(p.onGround);
        CHECK(std::abs(p.pos.y - 4) < 0.01f);
        CHECK(!overlapsSolid(g, p.pos));
    }
    {  // a 2-block wall stops the player flush, never inside
        Grid g = flatWorld(2);
        Player p;
        p.pos = {5.5f, 4.001f, 8.5f};
        Input in;
        in.move = {1, 0};
        in.run = true;
        for (int i = 0; i < 360; ++i) {
            updatePlayer(p, g, in, 1 / 120.0f);
            CHECK(!overlapsSolid(g, p.pos));
        }
        CHECK(p.pos.x + kHalfWidth <= 10.0f);
        CHECK(p.pos.x + kHalfWidth > 9.9f);
        CHECK(std::abs(p.pos.y - 4) < 0.01f);
    }
    {  // a 1-block ledge is stepped up
        Grid g = flatWorld(1);
        Player p;
        p.pos = {5.5f, 4.001f, 8.5f};
        Input in;
        in.move = {1, 0};
        float maxY = 0;
        for (int i = 0; i < 360; ++i) {
            updatePlayer(p, g, in, 1 / 120.0f);
            maxY = std::max(maxY, p.pos.y);
        }
        CHECK(maxY > 4.9f);
        CHECK(p.pos.x > 11);
    }
    {  // grounding holds at high frame rates (one frame of gravity < rest gap)
        Grid g = flatWorld(0);
        Player p;
        p.pos = {5.5f, 4.001f, 8.5f};
        updatePlayer(p, g, {}, 1 / 1000.0f);
        bool alwaysGrounded = true;
        for (int i = 0; i < 2000; ++i) {
            updatePlayer(p, g, {}, 1 / 1000.0f);
            alwaysGrounded &= p.onGround;
        }
        CHECK(alwaysGrounded);
        CHECK(p.airAmount < 0.01f);
    }
    {  // swimming player can climb onto a shore level with the water surface
        Grid g({16, 16, 16});
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) {
                for (int y = 0; y < 4; ++y) g.set({x, y, z}, Stone);  // pool floor top at y = 4
                for (int y = 4; y < 10; ++y) g.set({x, y, z}, x < 10 ? Water : Stone);  // shore top y = 10
            }
        Player p;
        p.pos = {5.5f, 4.001f, 8.5f};
        Input in;
        in.move = {1, 0};
        in.jump = true;
        for (int i = 0; i < 1200; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.pos.x > 10.5f);
        CHECK(p.pos.y >= 10.0f);
    }
    {  // flight: jump rises without gravity; collision still applies unless noclip
        Grid g = flatWorld(2);
        Player p;
        p.pos = {5.5f, 4.001f, 8.5f};
        p.tuning.fly = true;
        Input in;
        in.jump = true;
        for (int i = 0; i < 120; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.pos.y > 10);
        float flyHeight = p.pos.y;
        in = {};
        for (int i = 0; i < 120; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(std::abs(p.pos.y - flyHeight) < 1.5f);  // hovers (only coasting), no falling
        p.pos = {5.5f, 4.001f, 8.5f};
        in.move = {1, 0};
        for (int i = 0; i < 240; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.pos.x + kHalfWidth <= 10.0f);  // the 2-high wall still blocks flying
        p.tuning.noclip = true;
        for (int i = 0; i < 240; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.pos.x > 12);  // noclip passes through
    }
    {  // camera clearance stops a camera-sized box short of terrain
        Grid g = flatWorld(8);  // wall at x = 10, y 4..11
        float d = cameraClearance(g, {5.5f, 6, 8.5f}, {1, 0, 0}, 9, 0.3f);
        CHECK(d <= 10 - 5.5f - 0.3f + 1e-3f);
        CHECK(d >= 10 - 5.5f - 0.3f - 0.11f);
        CHECK(cameraClearance(g, {5.5f, 6, 8.5f}, {-1, 0, 0}, 3, 0.3f) == 3);  // open
        // Low camera over the ground: the box must not dip into the floor (top at y = 4).
        float low = cameraClearance(g, {5.5f, 5.4f, 8.5f}, glm::normalize(glm::vec3(-1, -0.3f, 0)), 9, 0.3f);
        glm::vec3 eye = glm::vec3(5.5f, 5.4f, 8.5f) + glm::normalize(glm::vec3(-1, -0.3f, 0)) * low;
        CHECK(eye.y - 0.3f >= 4.0f);
    }
    {  // walking advances the gait and turns the body toward the motion
        Grid g = flatWorld(0);
        Player p;
        p.pos = {5.5f, 4.001f, 3.5f};
        Input in;
        in.move = {0, 1};
        for (int i = 0; i < 60; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.gaitAmount > 0.5f);
        in.move = {1, 0};
        for (int i = 0; i < 120; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(std::abs(p.yaw - std::numbers::pi_v<float> / 2) < 0.05f);
        for (int i = 0; i < 240; ++i) updatePlayer(p, g, {}, 1 / 120.0f);
        CHECK(p.gaitAmount < 0.05f);
    }
}

// FNV-1a of every voxel: pins generated terrain bit for bit across refactors.
static uint32_t fnv(const Grid& g) {
    uint32_t h = 2166136261u;
    for (Block b : g.cells) h = (h ^ b) * 16777619u;
    return h;
}

static void generation() {
    Grid a({64, 128, 64}), b({64, 128, 64}), c({64, 128, 64});
    generateTerrain(a, 7);
    generateTerrain(b, 7);
    generateTerrain(c, 8);
    CHECK(a.cells == b.cells);
    CHECK(fnv(a) == 0x41eabed1u);  // recorded before trees and rocks became shared prefab generators
    CHECK(a.cells != c.cells);
    glm::vec3 spawn = findSpawn(a);
    CHECK(a.get(glm::ivec3(glm::floor(spawn)) - glm::ivec3(0, 1, 0)) == Grass);
}

// Large-scale layout: forests and rocky land exist, and somewhere there is an open plain of at
// least 96x96 columns with no trees or rocks (the "vast plains" requirement).
static void regions() {
    Grid g({512, 192, 512});
    generateTerrain(g, 42);
    int n = g.size.x, forest = 0, rocky = 0;
    std::vector<int> sum(size_t(n + 1) * (n + 1), 0);  // 2D prefix sum of decorated columns
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            bool tree = false, rock = false;
            for (int y = kSeaLevel; y < g.size.y; ++y) {
                Block b = g.get({x, y, z});
                tree |= b == Wood || b == Leaves;
                rock |= b == Rock;
            }
            forest += tree;
            rocky += rock;
            sum[size_t(z + 1) * (n + 1) + x + 1] = (tree || rock) + sum[size_t(z) * (n + 1) + x + 1] +
                                                   sum[size_t(z + 1) * (n + 1) + x] - sum[size_t(z) * (n + 1) + x];
        }
    CHECK(fnv(g) == 0xbd6a5444u);  // forests, spires and boulders unchanged (recorded before the prefab refactor)
    CHECK(forest > n * n / 20);   // > 5% of columns under trees
    CHECK(rocky > n * n / 200);   // > 0.5% of columns with rocks
    constexpr int kOpen = 96;
    bool openPlain = false;
    for (int z = 0; z + kOpen <= n && !openPlain; z += 8)
        for (int x = 0; x + kOpen <= n && !openPlain; x += 8) {
            auto at = [&](int xx, int zz) { return sum[size_t(zz) * (n + 1) + xx]; };
            openPlain = at(x + kOpen, z + kOpen) - at(x, z + kOpen) - at(x + kOpen, z) + at(x, z) == 0;
        }
    CHECK(openPlain);
}

// A grass meadow (top at y = 3) with a pond covering x >= 32.
static Grid meadow() {
    Grid g({64, 16, 64});
    for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x) {
            for (int y = 0; y < 3; ++y) g.set({x, y, z}, Dirt);
            g.set({x, 3, z}, x >= 32 ? Water : Grass);
        }
    return g;
}

static void entities() {
    Grid g = meadow();
    Player player;
    CHECK(player.health == 100 && maxHealth(player.stats) == 100);
    player.pos = {28.5f, 4, 20.5f};
    std::vector<Entity> es = spawnEntities(g, player.pos, 1);
    CHECK(es.size() == 4);
    Entity &dummy = es[0], &villager = es[1];

    CHECK(dummy.kind == EntityKind::TrainingDummy && dummy.name == "Training Dummy" && dummy.level == 1);
    CHECK(dummy.body.health == 100 && maxHealth(dummy.body.stats) == 100);
    CHECK(dummy.type().attackable && !dummy.type().wanders && !dummy.type().talks);
    CHECK(villager.kind == EntityKind::Villager && villager.level == 5 && !villager.name.empty());
    CHECK(villager.body.health == 250 && maxHealth(villager.body.stats) == 250);
    CHECK(!villager.type().attackable && villager.type().wanders && villager.type().talks);
    CHECK(spawnEntities(g, player.pos, 1)[1].name == villager.name);  // names are seeded
    for (size_t i = 0; i < es.size(); ++i)  // nobody spawns inside anybody
        for (size_t j = 0; j < i; ++j) CHECK(glm::distance(es[i].body.pos, es[j].body.pos) >= 1.5f);
    for (const Entity& e : es) CHECK(glm::distance(e.body.pos, player.pos) >= 1.5f);
    for (const Entity& e : es) CHECK(g.get(glm::ivec3(glm::floor(e.body.pos)) - glm::ivec3(0, 1, 0)) == Grass);

    // Damage: only attackable entities take it, and health stops at 0.
    CHECK(damage(dummy, 30) && dummy.body.health == 70);
    CHECK(damage(dummy, 500) && dummy.body.health == 0);
    CHECK(!damage(dummy, 10));  // nothing left to hit
    CHECK(!damage(villager, 10) && villager.body.health == 250);

    // Talking: only the villager, only in range; a new phrase each time.
    player.pos = dummy.body.pos + glm::vec3(1, 0, 0);
    CHECK(talkTarget(es, player) == nullptr || glm::distance(villager.body.pos, player.pos) < kTalkRange);
    player.pos = villager.body.pos + glm::vec3(1.5f, 0, 0);
    CHECK(talkTarget(es, player) == &villager);
    CHECK(villager.speech() == nullptr);
    talk(villager);
    const char* first = villager.speech();
    CHECK(first != nullptr);
    talk(villager);
    CHECK(villager.speech() != nullptr && villager.speech() != first);
    for (int i = 0; i < 60; ++i) updateEntities(es, g, player, 1 / 60.0f);
    CHECK(glm::length(glm::vec2(villager.body.vel.x, villager.body.vel.z)) < 0.1f);  // stops to talk
    player.pos = {5.5f, 4, 60.5f};
    CHECK(talkTarget(es, player) == nullptr);

    // Separation: the player can't stand inside an entity.
    Player p2;
    p2.pos = dummy.body.pos + glm::vec3(0.1f, 0, 0);
    separate(p2, g, es);
    CHECK(glm::distance(p2.pos, dummy.body.pos) > 0.79f);

    // Wandering: two minutes of AI. The villager strolls, stays near home and out of the pond;
    // the dummy never moves.
    glm::vec3 dummyPos = dummy.body.pos;
    float farthest = 0, maxFromHome = 0;
    bool wet = false, stuck = false;
    for (int i = 0; i < 120 * 60; ++i) {
        updateEntities(es, g, player, 1 / 60.0f);
        farthest = std::max(farthest, glm::distance(villager.body.pos, villager.home));
        maxFromHome = std::max(maxFromHome, glm::distance(glm::vec2(villager.body.pos.x, villager.body.pos.z),
                                                          glm::vec2(villager.home.x, villager.home.z)));
        wet |= villager.body.inWater || villager.body.pos.x > 32;
        stuck |= overlapsSolid(g, villager.body.pos);
    }
    CHECK(farthest > 3);
    CHECK(maxFromHome < villager.type().wanderRadius + 2);
    CHECK(!wet);
    CHECK(!stuck);
    CHECK(dummy.body.pos == dummyPos);
    CHECK(villager.speech() == nullptr);  // the balloon timed out
}

static void combat() {
    // Attributes: 10 health per stamina; a punch does 1-4 + strength x speed, the sword 6-10 + it.
    Stats base;  // the player's
    CHECK(maxHealth(base) == 100 && Player{}.health == 100);
    std::mt19937 rng(7);
    int lo = 1000, hi = 0;
    for (int i = 0; i < 2000; ++i) {
        int d = rollDamage(base, kPunch, rng);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    CHECK(lo == 3 && hi == 6);  // strength 2, speed 1: [3, 6]
    lo = 1000, hi = 0;
    for (int i = 0; i < 2000; ++i) {
        int d = rollDamage(base, itemType(Shortsword).attack, rng);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    CHECK(lo == 8 && hi == 12);
    CHECK(strengthBonus(base, Attack{6, 10, 2.5f}) == 5);  // slow attacks get more per hit
    Stats quick = base;
    quick.agility = 50;
    CHECK(swingTime(quick, kPunch) < swingTime(base, kPunch));  // agility is attack speed...
    CHECK(strengthBonus(quick, kPunch) == strengthBonus(base, kPunch));  // ...not damage per hit
    CHECK(swingTime(Stats{10, 0, 0, 0, 0}, kPunch) == kPunch.speed);

    Grid g = meadow();
    Player player;
    player.pos = {28.5f, 4, 20.5f};
    std::vector<Entity> es = spawnEntities(g, player.pos, 1);
    Entity &dummy = es[0], &villager = es[1];
    auto run = [&](float seconds) {
        for (int i = 0; i < int(seconds * 60); ++i) updateCombat(player, es, 1 / 60.0f, rng);
    };
    auto standBeside = [&](const Entity& e, float dist) {
        player.pos = e.body.pos + glm::vec3(dist, 0, 0);
        player.yaw = -std::numbers::pi_v<float> / 2;  // facing -x, toward it
    };

    // A punch lands at its impact frame; the swing timer gates the next one.
    standBeside(dummy, 1.2f);
    CHECK(punchTarget(es, player) == &dummy);
    CHECK(startPunch(player, {-1, 0}));
    CHECK(!startPunch(player, {-1, 0}));
    updateCombat(player, es, 0.01f, rng);
    CHECK(dummy.body.health == 100);  // the fist is still on its way
    run(0.3f);
    float dealt = 100 - dummy.body.health;
    CHECK(dealt >= 3 && dealt <= 6 && dummy.body.lastDamage == int(dealt));
    CHECK(player.combatTimer > 0 && dummy.body.combatTimer > 0);
    run(1);
    // With the sword in the main hand the same swing hits for the sword's damage.
    giveItem(player.inv, Shortsword);
    CHECK(quickMove(player.inv, {SlotRef::Item, 0, 0}));
    float before = dummy.body.health;
    CHECK(startPunch(player, {-1, 0}));
    run(0.3f);
    CHECK(before - dummy.body.health >= 8 && before - dummy.body.health <= 12);
    player.inv = Inventory{};
    run(1);
    CHECK(startPunch(player, {-1, 0}));

    // Misses: facing away, out of reach, and the villager can't be attacked.
    player.yaw = std::numbers::pi_v<float> / 2;
    CHECK(punchTarget(es, player) == nullptr);
    standBeside(dummy, kPunchReach + 0.5f);
    CHECK(punchTarget(es, player) == nullptr);
    standBeside(villager, 1.2f);
    CHECK(punchTarget(es, player) == nullptr);
    {  // The nearest of two targets in front gets hit, but only within 60 degrees of facing.
        std::vector<Entity> two{dummy, dummy};
        two[1].body.pos = two[0].body.pos - glm::vec3(1, 0, 0);
        Player p;
        p.pos = two[0].body.pos + glm::vec3(1.2f, 0, 0);
        p.yaw = -std::numbers::pi_v<float> / 2;
        CHECK(punchTarget(two, p) == &two[0]);
        p.pos = two[1].body.pos + glm::vec3(0.3f, 0, 1.2f);  // the second is nearer, 76 degrees off
        CHECK(punchTarget(two, p) == nullptr);
    }

    // Held swings chain without losing time to frame rounding: at 60 fps a 0.98 s swing would
    // round up to 59 frames, and 50 swings would take 49 x 59 = 2891 frames.
    run(2);
    int swings = 0;
    for (int i = 0; i < int(49 * swingTime(player.stats, kPunch) * 60) + 5; ++i) {
        swings += startPunch(player, {-1, 0});
        updateCombat(player, es, 1 / 60.0f, rng);
    }
    CHECK(swings == 50);

    // Spirit regenerates health, but only out of combat.
    dummy.body.health = 50;
    dummy.body.combatTimer = kCombatSeconds;
    run(kCombatSeconds - 0.5f);
    CHECK(dummy.body.health == 50);
    run(2.5f);  // ~2 s out of combat at 10 spirit = ~10 health
    CHECK(dummy.body.health > 55 && dummy.body.health < 62);
    run(20);
    CHECK(dummy.body.health == 100);  // stops at max
    player.health = 50;
    player.combatTimer = kCombatSeconds;
    run(1);
    CHECK(player.health == 50);  // the player regenerates out of combat only, too

    // At 0 health the dummy topples, can't be hit, and stands back up fully healed.
    auto standing = poseModel(dummy.body, ModelDummy);
    CHECK(damage(dummy, 1000) && !dummy.body.alive());
    standBeside(dummy, 1.2f);
    CHECK(punchTarget(es, player) == nullptr);
    run(1);
    CHECK(poseModel(dummy.body, ModelDummy)[Head][3].y < standing[Head][3].y - 0.8f);
    run(dummy.type().respawnSeconds - 1.2f);
    CHECK(!dummy.body.alive());
    run(0.3f);
    CHECK(dummy.body.alive() && dummy.body.health == 100);
}

// Boar and bandit: who they are, who they fight, when they give up, what they drop.
static void fighters() {
    auto flat = [](glm::vec3 v) { return glm::vec2(v.x, v.z); };
    Grid g = meadow();
    Player player;
    player.pos = {28.5f, 4, 20.5f};
    std::vector<Entity> es = spawnEntities(g, player.pos, 1);
    Entity &boar = es[2], &bandit = es[3];
    std::mt19937 rng(11);
    auto sim = [&](float seconds) {
        for (int i = 0; i < int(seconds * 60); ++i) {
            updateEntities(es, g, player, 1 / 60.0f);
            separate(player, g, es);
            updateCombat(player, es, 1 / 60.0f, rng);
        }
    };

    // The boar is the player's match: level, health and unarmed damage. The bandit is a level
    // above, tougher, and fights with a knife: quicker than the sword but weaker per hit.
    CHECK(boar.kind == EntityKind::Boar && boar.level == 1 && boar.type().temper == Temper::Passive);
    CHECK(boar.body.health == player.health && maxHealth(boar.body.stats) == maxHealth(player.stats));
    CHECK(strengthBonus(boar.body.stats, kPunch) == strengthBonus(player.stats, kPunch));
    CHECK(mainAttack(boar.body.inv).minDamage == kPunch.minDamage && mainAttack(boar.body.inv).maxDamage == kPunch.maxDamage);
    CHECK(bandit.kind == EntityKind::Bandit && bandit.level == 2 && bandit.type().temper == Temper::Aggressive);
    CHECK(maxHealth(bandit.body.stats) > maxHealth(player.stats) && bandit.body.stats.strength > player.stats.strength);
    CHECK(bandit.body.inv.equipped[SlotMainHand] == Knife);
    const Attack &knife = itemType(Knife).attack, &sword = itemType(Shortsword).attack;
    CHECK(knife.speed < sword.speed && knife.maxDamage < sword.maxDamage && knife.minDamage < sword.minDamage);
    // Its camp is out of sight of the spawn: even at the edge of its wandering it can't see it,
    // and the spawn is outside its territory.
    float campDist = glm::distance(flat(bandit.home), flat(player.pos));
    CHECK(campDist - bandit.type().wanderRadius > bandit.type().aggroRange && campDist > kLeashRadius);

    // A minute at the spawn: nobody starts a fight.
    sim(60);
    CHECK(player.health == 100 && !boar.angry && !bandit.angry);

    // Passive: standing right next to the boar is fine, until it gets hit. Then it gores back.
    player.pos = boar.body.pos + glm::vec3(1.5f, 0, 0);
    sim(5);
    CHECK(player.health == 100 && !boar.angry);
    CHECK(damage(boar, 1) && boar.angry);
    sim(5);
    CHECK(player.health < 100 && player.lastDamage >= 3 && player.lastDamage <= 6);  // a punch, like the player's
    CHECK(!bandit.angry);
    boar.angry = false;

    // Aggressive: the bandit charges a player inside its aggro range and hits with the knife.
    player.health = 100;
    player.pos = bandit.body.pos + glm::vec3(-8, 0, 0);
    sim(0.1f);
    CHECK(bandit.angry);
    sim(6);
    CHECK(player.health < 100 && player.lastDamage >= 5 && player.lastDamage <= 8);  // 3-6 + strength 3 x speed 0.75
    CHECK(glm::distance(flat(bandit.body.pos), flat(player.pos)) < kPunchReach);

    // Leash: once the player is out of its territory it gives up and walks home.
    player.pos = bandit.home + glm::vec3(-kLeashRadius - 1, 0, 0);
    sim(0.1f);
    CHECK(!bandit.angry);
    sim(30);
    CHECK(!bandit.angry && glm::distance(flat(bandit.body.pos), flat(bandit.home)) < bandit.type().wanderRadius + 1);

    // Knocked out: fighters stop, and the player can't swing until back up.
    player.health = 1;
    player.pos = bandit.body.pos + glm::vec3(-3, 0, 0);
    sim(6);
    CHECK(!player.alive() && player.deadTime > 0 && !bandit.angry && !startPunch(player, {1, 0}));
    player.health = 100;
    player.deadTime = 0;
    player.pos = {5.5f, 4, 5.5f};  // out of everyone's way

    // Loot: most boars leave a skin, and a kill is the only way to get one.
    int drops = 0;
    for (int i = 0; i < 400; ++i) {
        boar.body.health = 100;
        boar.loot = NoItem;
        CHECK(damage(boar, 1000) && (boar.loot == NoItem || boar.loot == BoarSkin));
        drops += boar.loot == BoarSkin;
    }
    CHECK(drops > 400 * 0.65f && drops < 400 * 0.85f);  // 75%
    for (int i = 0; i < 50; ++i) CHECK(damage(bandit, 1) && bandit.loot == NoItem);  // still alive, nothing yet
    bandit.angry = false;
    bandit.body.health = float(maxHealth(bandit.body.stats));
    // The corpse is looted with E from close by; full bags leave the loot on it.
    boar.loot = BoarSkin;
    player.pos = boar.body.pos + glm::vec3(2, 0, 0);
    CHECK(lootTarget(es, player) == &boar);
    Inventory full;
    while (giveItem(full, Satchel)) {}
    CHECK(!takeLoot(boar, full) && boar.loot == BoarSkin);
    std::swap(full, player.inv);
    CHECK(lootTarget(es, player) == nullptr);  // nothing to loot into: E talks instead
    std::swap(full, player.inv);
    CHECK(takeLoot(boar, player.inv) && boar.loot == NoItem && player.inv.containers[0][0] == BoarSkin);
    CHECK(lootTarget(es, player) == nullptr);
    // It lies still where it fell, then comes back at home, healed and calm.
    glm::vec3 fell = boar.body.pos;
    boar.body.deadTime = 0;
    boar.angry = true;
    sim(boar.type().respawnSeconds - 1);
    CHECK(!boar.body.alive() && !boar.angry && flat(boar.body.pos) == flat(fell));
    sim(1.2f);
    CHECK(boar.body.alive() && boar.body.health == 100 && boar.loot == NoItem);
    CHECK(glm::distance(flat(boar.body.pos), flat(boar.home)) < 1);

    // A swing dies with its swinger: neither a corpse nor a knocked-out player lands a hit.
    player.pos = bandit.body.pos + glm::vec3(-1.5f, 0, 0);
    player.yaw = std::numbers::pi_v<float> / 2;  // facing +x, toward the bandit
    bandit.body.yaw = -std::numbers::pi_v<float> / 2;
    bandit.body.swingTimer = 0;
    CHECK(startPunch(bandit.body, {-1, 0}));
    CHECK(damage(bandit, 1000));
    for (int i = 0; i < 60; ++i) updateCombat(player, es, 1 / 60.0f, rng);
    CHECK(player.health == 100 && !bandit.body.hitPending);
    boar.body.pos = player.pos + glm::vec3(1.2f, 0, 0);
    float boarHealth = boar.body.health;
    player.swingTimer = 0;
    CHECK(startPunch(player, {1, 0}));
    player.health = 0;
    for (int i = 0; i < 60; ++i) updateCombat(player, es, 1 / 60.0f, rng);
    CHECK(boar.body.health == boarHealth && !player.hitPending);
    player.health = 100;

    // Models: every part has voxels, the bandit holds its knife, the boar's head lunges when it
    // gores, and the dead fall over.
    for (Model m : {ModelBoar, ModelBandit, ModelKnife}) {
        Grid part = buildPart(m, Head);
        CHECK(std::ranges::count(part.cells, Air) < std::ssize(part.cells));
    }
    CHECK(heldModel(Knife) == ModelKnife && heldModel(Shortsword) == ModelSword);
    glm::mat4 held(0);
    poseModel(bandit.body, ModelBandit, &held);
    CHECK(held != glm::mat4(0));
    held = glm::mat4(0);
    poseModel(boar.body, ModelBoar, &held);
    CHECK(held == glm::mat4(0));  // no hands
    Player b = boar.body;
    auto idle = poseModel(b, ModelBoar);
    CHECK(startPunch(b, {0, 1}));
    b.punchTime = kPunchImpact * b.punchLength;
    CHECK(poseModel(b, ModelBoar)[Head][3].z > idle[Head][3].z + 0.2f);
    b.health = 0;
    b.deadTime = 1;
    CHECK(poseModel(b, ModelBoar)[Torso][3].y < idle[Torso][3].y - 0.1f);
    Player knocked;
    knocked.health = 0;
    knocked.deadTime = 1;
    CHECK(poseModel(knocked, ModelPlayer)[Head][3].y < poseModel(Player{}, ModelPlayer)[Head][3].y - 0.8f);
}

static void items() {
    using enum SlotRef::Kind;
    Inventory inv;
    CHECK(itemSlots(inv) == kBackpackSlots && freeSlots(inv) == 12);
    CHECK(mainAttack(inv).maxDamage == kPunch.maxDamage);
    CHECK(giveItem(inv, Shortsword) && giveItem(inv, Satchel) && freeSlots(inv) == 10);

    // Right-click equips: the sword goes to the main hand, the satchel to the bag bar and adds slots.
    CHECK(quickMove(inv, {Item, 0, 0}) && inv.equipped[SlotMainHand] == Shortsword);
    CHECK(mainAttack(inv).minDamage == 6 && mainAttack(inv).maxDamage == 10);
    CHECK(quickMove(inv, {Item, 1, 0}) && inv.bags[0] == Satchel);
    CHECK(itemSlots(inv) == 12 + itemType(Satchel).bagSlots && freeSlots(inv) == itemSlots(inv));
    // ...and unequips into the first free slot.
    CHECK(quickMove(inv, {Equip, SlotMainHand}) && inv.containers[0][0] == Shortsword);
    CHECK(mainAttack(inv).maxDamage == kPunch.maxDamage);

    // Slots only take what fits.
    CHECK(!moveItem(inv, {Item, 0, 0}, {Equip, SlotHead}));
    CHECK(!moveItem(inv, {Item, 0, 0}, {Bag, 1}));
    CHECK(!moveItem(inv, {Bag, 0}, {Equip, SlotMainHand}));
    CHECK(!moveItem(inv, {Item, 0, 0}, {Item, 99, 0}) && !moveItem(inv, {Item, 5, 0}, {Item, 6, 0}));  // bad slot, empty

    // A bag holding items stays on the bar; an empty one can't go into its own slots.
    CHECK(moveItem(inv, {Item, 0, 0}, {Item, 2, 1}) && inv.containers[1][2] == Shortsword);
    CHECK(!quickMove(inv, {Bag, 0}) && !moveItem(inv, {Bag, 0}, {Item, 4, 0}));
    CHECK(moveItem(inv, {Item, 2, 1}, {Item, 0, 0}));
    CHECK(!moveItem(inv, {Bag, 0}, {Item, 1, 1}));
    CHECK(moveItem(inv, {Bag, 0}, {Item, 4, 0}) && inv.containers[0][4] == Satchel && inv.bags[0] == NoItem);
    CHECK(itemSlots(inv) == 12 && inv.containers[1].empty());

    // Dropping gear on an occupied slot swaps the two.
    CHECK(moveItem(inv, {Item, 0, 0}, {Equip, SlotMainHand}) && giveItem(inv, Shortsword));
    CHECK(moveItem(inv, {Item, 0, 0}, {Item, 4, 0}) && inv.containers[0][0] == Satchel && inv.containers[0][4] == Shortsword);

    // Full bags take nothing more, and right-click can't unequip into them.
    while (giveItem(inv, Shortsword)) {}
    CHECK(freeSlots(inv) == 0 && !giveItem(inv, Satchel));
    CHECK(!quickMove(inv, {Equip, SlotMainHand}) && inv.equipped[SlotMainHand] == Shortsword);

    // Random drags and right-clicks never lose or duplicate an item, and every container matches its bag.
    std::mt19937 rng(3);
    auto count = [](const Inventory& v) {
        std::array<int, ItemCount> n{};
        for (ItemId id : v.equipped) ++n[id];
        for (ItemId id : v.bags) ++n[id];
        for (const auto& c : v.containers)
            for (ItemId id : c) ++n[id];
        n[NoItem] = 0;  // empty slots come and go with bags
        return n;
    };
    auto any = [&](Inventory& v) {
        auto k = SlotRef::Kind(rng() % 3);
        int c = int(rng() % (1 + kBagBarSlots));
        int n = k == Equip ? EquipCount : k == Bag ? kBagBarSlots : std::max(1, int(v.containers[size_t(c)].size()));
        return SlotRef{k, int(rng() % unsigned(n)), c};
    };
    int moved = 0;
    for (int trial = 0; trial < 200; ++trial) {
        Inventory v;
        for (int i = 0; i < 8; ++i) giveItem(v, ItemId(1 + rng() % (ItemCount - 1)));
        auto before = count(v);
        for (int step = 0; step < 200; ++step) {
            moved += rng() % 4 ? moveItem(v, any(v), any(v)) : quickMove(v, any(v));
            CHECK(count(v) == before);
            for (int b = 0; b < kBagBarSlots; ++b)
                CHECK(int(v.containers[size_t(b) + 1].size()) == itemType(v.bags[size_t(b)]).bagSlots);
            for (int s = 0; s < EquipCount; ++s)
                CHECK(v.equipped[size_t(s)] == NoItem || itemType(v.equipped[size_t(s)]).slot == s);
        }
    }
    CHECK(moved > 600);  // most random pairs are two empty slots; this seed makes ~690 real moves

    // The sword is held only when equipped, and it points forward at the slash's impact frame.
    Player p;
    glm::mat4 held(0);
    poseModel(p, ModelPlayer, &held);
    CHECK(held == glm::mat4(0));
    giveItem(p.inv, Shortsword);
    quickMove(p.inv, {Item, 0, 0});
    CHECK(startPunch(p, {0, 1}));
    p.punchTime = kPunchImpact * p.punchLength;
    auto pose = poseModel(p, ModelPlayer, &held);
    glm::vec3 blade = glm::normalize(glm::vec3(held * glm::vec4(0, 1, 0, 0)));
    CHECK(blade.z > 0.95f);
    glm::vec3 grip(held * glm::vec4(2.5f, 2.5f, 0.5f, 1)), fist(pose[HandR] * glm::vec4(1.5f, 1.5f, 1.5f, 1));
    CHECK(glm::distance(grip, fist) < 1e-3f);  // gripped at the center of the right hand
    // Unequipping mid-swing doesn't change the swing: it still slashes and lands as a sword hit.
    p.inv = Inventory{};
    held = glm::mat4(0);
    poseModel(p, ModelPlayer, &held);
    CHECK(held != glm::mat4(0) && weaponAttack(p.swingWeapon).maxDamage == 10);
}

static void protection() {
    // Damage reduction = armor / (armor + K x attacker level).
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
    CHECK(near(armorReduction(37, 2), 37 / 117.0f) && near(armorReduction(37, 2), 0.316f));
    CHECK(near(armorReduction(37, 1), 0.481f) && near(armorReduction(40, 1), 0.5f));  // K armor halves a level 1 hit
    CHECK(armorReduction(0, 1) == 0 && armorReduction(37, 2, 80) < armorReduction(37, 2));  // bigger K, less reduction
    CHECK(mitigate(8, 0, 2) == 8 && mitigate(8, 37, 2) == 5 && mitigate(5, 37, 2) == 3);
    CHECK(mitigate(8, 37, 2, 80) == 6);
    CHECK(mitigate(1, 1000, 1) == 1 && mitigate(3, 1000, 1, 1) == 1);  // a hit always does at least 1
    CHECK(near(armorReduction(1000, 1, 1), 0.8f) && near(armorReduction(160, 1), 0.8f));  // capped at 80%...
    CHECK(near(armorReduction(159, 1), 159 / 199.0f));                                   // ...only above it
    CHECK(mitigate(50, 1000, 1, 1) == 10);
    CHECK(mitigate(0, 37, 1) == 0);

    // Chest and legs always give more armor than any other piece.
    for (int i = 1; i < ItemCount; ++i)
        for (int j = 1; j < ItemCount; ++j) {
            const ItemType &big = itemType(ItemId(i)), &other = itemType(ItemId(j));
            bool bigSlot = big.slot == SlotChest || big.slot == SlotLegs;
            bool otherSlot = other.slot == SlotHead || other.slot == SlotHands || other.slot == SlotFeet ||
                             other.slot == SlotMainHand || other.slot == SlotOffHand;
            if (bigSlot && otherSlot && big.armor <= other.armor)
                std::println(stderr, "{} must have more armor than {}", big.name, other.name);
            CHECK(!(bigSlot && otherSlot) || big.armor > other.armor);
        }

    // Only worn pieces count.
    using enum SlotRef::Kind;
    constexpr ItemId kSet[] = {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons};
    Player player;
    for (ItemId id : kSet) CHECK(giveItem(player.inv, id));
    CHECK(giveItem(player.inv, IronHelm) && armor(player.inv) == 0);  // a spare helm stays in the bags
    for (int i = 0; i < 5; ++i) CHECK(quickMove(player.inv, {Item, i, 0}));
    CHECK(armor(player.inv) == 37 && player.inv.containers[0][5] == IronHelm);
    CHECK(quickMove(player.inv, {Equip, SlotChest}) && armor(player.inv) == 25);
    CHECK(quickMove(player.inv, {Item, 0, 0}) && armor(player.inv) == 37);

    // A bandit (level 2, knife 5-8 unarmored) hits the armored player for 3-5; with K = 1 armor
    // takes almost everything.
    Grid g = meadow();
    player.pos = {20.5f, 4, 20.5f};
    std::vector<Entity> es = spawnEntities(g, player.pos, 1);
    Entity& bandit = es[3];
    player.pos = bandit.body.pos + glm::vec3(-1.5f, 0, 0);
    bandit.angry = true;
    std::mt19937 rng(5);
    auto hits = [&](float k) {
        int lo = 99, hi = 0, n = 0;
        for (int i = 0; i < 20 * 60; ++i) {
            updateEntities(es, g, player, 1 / 60.0f);
            updateCombat(player, es, 1 / 60.0f, rng, k);
            if (player.hitTime == 0) {
                lo = std::min(lo, player.lastDamage), hi = std::max(hi, player.lastDamage), ++n;
                player.health = 100;
            }
        }
        return std::array{lo, hi, n};
    };
    auto [lo, hi, n] = hits(kArmorK);
    CHECK(n > 10 && lo >= 3 && hi <= 5 && hi < 8);
    auto [lo1, hi1, n1] = hits(1);
    CHECK(n1 > 10 && lo1 == 1 && hi1 == 2);  // tiny K hits the 80% cap: 5-8 becomes 1-2, not all 1s

    // Worn pieces are drawn over the parts they cover, at the same sizes; greaves don't show.
    auto parts = partModels(ModelPlayer, player.inv);
    for (Part part : {Head, Torso, HandL, HandR, FootL, FootR}) {
        CHECK(parts[part] == ModelIronArmor);
        Grid plate = buildPart(ModelIronArmor, part);
        CHECK(plate.size == buildPart(ModelPlayer, part).size && std::ranges::count(plate.cells, Plate) > 0);
    }
    Grid helm = buildPart(ModelIronArmor, Head);
    CHECK(std::ranges::count(helm.cells, Eye) == 4);  // the face shows through
    player.inv.equipped[SlotHead] = NoItem;
    CHECK(partModels(ModelPlayer, player.inv)[Head] == ModelPlayer);
    CHECK(partModels(ModelBoar, player.inv)[Torso] == ModelBoar);
}

// World files: a round trip is bit-exact; truncated or corrupted files are rejected cleanly (the
// dev preset's sanitizers check "cleanly"), and unknown sections are skipped.
static void worldFiles() {
    std::optional<std::vector<Marker>> fileMarks;  // the files below have a marker section
    Grid g({64, 96, 32});
    generateTerrain(g, 3);
    g.set({5, 90, 5}, Plate);  // a block generation never places
    std::string path = (std::filesystem::temp_directory_path() / "flycoos_core_test.flyw").string();
    CHECK(saveWorld(path.c_str(), g, 99, {}, {}).empty());
    Grid back({32, 32, 32});
    uint32_t seed = 0;
    std::vector<Instance> none;
    CHECK(loadWorld(path.c_str(), back, seed, none, fileMarks).empty());
    CHECK(back.size == g.size && back.cells == g.cells && seed == 99);

    std::ifstream in(path, std::ios::binary);
    const std::string file((std::istreambuf_iterator<char>(in)), {});
    auto load = [&](const std::string& bytes) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
        Grid h({32, 32, 32});
        uint32_t s = 0;
        std::string error = loadWorld(path.c_str(), h, s, none, fileMarks);
        return error.empty() && h.cells == g.cells ? std::string("same") : error.empty() ? std::string("different") : error;
    };
    auto reseal = [](std::string bytes) {  // recompute the checksum, so the structural checks are what's tested
        uint32_t h = 2166136261u;
        for (size_t i = 0; i + 4 < bytes.size(); ++i) h = (h ^ uint8_t(bytes[i])) * 16777619u;
        for (int i = 0; i < 4; ++i) bytes[bytes.size() - 4 + size_t(i)] = char(h >> (8 * i));
        return bytes;
    };
    CHECK(load(file) == "same");
    for (size_t n : {size_t(0), size_t(3), size_t(24), size_t(28), file.size() / 2, file.size() - 1})
        CHECK(load(file.substr(0, n)) != "same" && load(file.substr(0, n)) != "different");
    for (size_t i = 0; i < file.size(); i += 5) {
        std::string bad = file;
        bad[i] = char(bad[i] ^ 0x5A);
        CHECK(load(bad) != "same" && load(bad) != "different");  // the checksum catches every flip
        load(reseal(bad));  // past the checksum: may load a different world, but must not crash or overrun
    }
    for (size_t cut : {size_t(1), size_t(5), size_t(40)}) {  // a short voxel section, checksum intact
        std::string bad = file.substr(0, file.size() - 4 - cut) + file.substr(file.size() - 4);
        CHECK(load(reseal(bad)) != "same" && load(reseal(bad)) != "different");
    }
    std::string future = file;
    future[4] = 2;
    CHECK(load(reseal(future)).starts_with("unsupported version"));
    std::string extra = file.substr(0, file.size() - 4) + std::string("STRY\x03\0\0\0\0\0\0\0abc", 15) + "....";
    CHECK(load(reseal(extra)) == "same");  // later sections (story markers, prefabs) don't break old readers
    std::filesystem::remove(path);

    std::filesystem::create_directories(path + "/occupied");  // the rename fails: no temp file is left behind
    CHECK(!saveWorld(path.c_str(), g, 99, {}, {}).empty() && !std::filesystem::exists(path + ".tmp"));
    std::filesystem::remove_all(path);
}

// Terrain with hills, trees and water in a 64x96x64 grid.
static Grid editWorld() {
    Grid g({64, 96, 64});
    generateTerrain(g, 11);
    return g;
}

static uint64_t hashCells(const Grid& g) {
    return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(g.cells.data()), g.cells.size()));
}

static EditOp randomOp(std::mt19937& rng, const Grid& g) {
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    constexpr Block kBlocks[] = {Air, Grass, Dirt, Stone, Sand, Snow, Wood, Leaves, Water, Rock};
    EditOp op;
    op.tool = Tool(pick(0, int(Tool::Count) - 1));
    op.center = {pick(-8, g.size.x + 8), pick(-8, g.size.z + 8)};
    op.radius = pick(0, 12);
    op.strength = pick(1, 8);
    op.height = pick(0, g.size.y - 1);
    op.block = kBlocks[pick(op.tool == Tool::Paint ? 1 : 0, op.tool == Tool::Paint ? 7 : 9)];
    op.min = {pick(-4, g.size.x), pick(0, g.size.y), pick(-4, g.size.z)};
    op.max = op.min + glm::ivec3(pick(-6, 6), pick(-6, 6), pick(-6, 6));
    return op;
}

static void editing() {
    std::vector<Marker> marks;  // no marker edits here
    {  // every op changes voxels only inside the dirty box it reports
        Grid g = editWorld();
        EditHistory history;
        std::mt19937 rng(21);
        int changed = 0;
        for (int i = 0; i < 60; ++i) {
            std::vector<Block> before = g.cells;
            EditResult r = applyEdit(g, randomOp(rng, g), history);
            CHECK(!r.error);
            changed += !r.dirty.empty();
            bool outside = false;
            for (int y = 0; y < g.size.y; ++y)
                for (int z = 0; z < g.size.z; ++z)
                    for (int x = 0; x < g.size.x; ++x) {
                        glm::ivec3 p{x, y, z};
                        bool in = !r.dirty.empty() && glm::all(glm::greaterThanEqual(p, r.dirty.lo)) &&
                                  glm::all(glm::lessThanEqual(p, r.dirty.hi));
                        outside |= !in && g.cells[g.index(p)] != before[g.index(p)];
                    }
            CHECK(!outside);
        }
        CHECK(changed > 30);
    }
    {  // seeded fuzz: random ops, undos and redos always match the recorded states bit for bit
        Grid g = editWorld();
        EditHistory history;
        std::vector<Instance> none;
        std::mt19937 rng(7);
        std::vector<uint64_t> states{hashCells(g)};  // timeline; states[at] is the current world
        size_t at = 0;
        int undos = 0, redos = 0;
        for (int step = 0; step < 400; ++step) {
            int action = std::uniform_int_distribution<int>(0, 9)(rng);
            if (action < 3 && at > 0) {
                CHECK(!undoEdit(g, history, none, marks).empty());
                --at, ++undos;
            } else if (action < 5 && at + 1 < states.size()) {
                CHECK(!redoEdit(g, history, none, marks).empty());
                ++at, ++redos;
            } else if (!applyEdit(g, randomOp(rng, g), history).dirty.empty()) {
                states.resize(++at);
                states.push_back(hashCells(g));
            }
            CHECK(hashCells(g) == states[at]);
        }
        while (at > 0) {  // all the way back to the generated world
            CHECK(!undoEdit(g, history, none, marks).empty());
            CHECK(hashCells(g) == states[--at]);
        }
        CHECK(undoEdit(g, history, none, marks).empty() && hashCells(g) == hashCells(editWorld()));
        CHECK(states.size() > 50 && undos > 50 && redos > 20);
    }
    {  // brushes: raise keeps the surface material on top of subsoil; lower under water keeps water
        Grid g({32, 16, 32});
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 32; ++x) {
                for (int y = 0; y < 6; ++y) g.set({x, y, z}, y == 5 ? Grass : Dirt);
                if (x >= 16) g.set({x, 5, z}, Sand), g.set({x, 6, z}, Water), g.set({x, 7, z}, Water);
            }
        EditHistory h;
        EditOp raise{.tool = Tool::Raise, .center = {8, 8}, .radius = 2, .strength = 3};
        CHECK(applyEdit(g, raise, h).dirty.hi.y == 8);
        CHECK(groundTop(g, 8, 8) == 8 && g.get({8, 8, 8}) == Grass && g.get({8, 5, 8}) == Dirt && g.get({8, 7, 8}) == Dirt);
        CHECK(groundTop(g, 8, 11) == 5);  // outside the radius
        EditOp lower{.tool = Tool::Lower, .center = {24, 8}, .radius = 0, .strength = 2};
        CHECK(!applyEdit(g, lower, h).dirty.empty());
        CHECK(groundTop(g, 24, 8) == 3 && g.get({24, 3, 8}) == Sand && g.get({24, 4, 8}) == Water && g.get({24, 5, 8}) == Water);
        EditOp flatten{.tool = Tool::Flatten, .center = {8, 8}, .radius = 4, .strength = 64, .height = 10};
        applyEdit(g, flatten, h);
        for (int dz = -4; dz <= 4; ++dz)
            for (int dx = -4; dx <= 4; ++dx)
                CHECK(dx * dx + dz * dz > 16 || groundTop(g, 8 + dx, 8 + dz) == 10);
        EditOp paint{.tool = Tool::Paint, .center = {8, 8}, .radius = 1, .block = Snow};
        applyEdit(g, paint, h);
        CHECK(g.get({8, 10, 8}) == Snow && g.get({8, 9, 8}) == Dirt && g.get({8, 10, 10}) == Grass);
        EditOp smooth{.tool = Tool::Smooth, .center = {8, 8}, .radius = 8, .strength = 64};
        applyEdit(g, smooth, h);
        CHECK(groundTop(g, 8, 8) == 10 && groundTop(g, 12, 8) < 10 && groundTop(g, 12, 8) > 5);  // the plateau's rim slumps
        EditOp clear{.tool = Tool::Fill, .min = {0, 0, 0}, .max = {3, 15, 3}, .block = Air};
        CHECK(applyEdit(g, clear, h).dirty.size() == glm::ivec3(4, 6, 4));  // only what was solid
        CHECK(groundTop(g, 2, 2) == -1);
        CHECK(applyEdit(g, clear, h).dirty.empty());  // nothing left to change: no undo record
    }
    {  // caps and bad input are rejected without touching the world or the history
        Grid g = editWorld();
        uint64_t before = hashCells(g);
        EditHistory h;
        const EditOp bad[] = {
            {.tool = Tool::Raise, .radius = kMaxBrushRadius + 1},
            {.tool = Tool::Raise, .radius = -1},
            {.tool = Tool::Lower, .strength = 0},
            {.tool = Tool::Lower, .strength = kMaxStrength + 1},
            {.tool = Tool::Flatten, .height = g.size.y},
            {.tool = Tool::Paint, .block = Water},
            {.tool = Tool::Raise, .center = {INT_MAX, 0}},
            {.tool = Tool::Fill, .min = {0, 0, 0}, .max = {128, 127, 127}},  // one voxel over 128^3
            {.tool = Tool::Fill, .min = {INT_MIN, 0, 0}, .max = {INT_MAX, 0, 0}},
            {.tool = Tool::Fill, .block = BlockCount},
            {.tool = Tool::Count},
        };
        for (const EditOp& op : bad) CHECK(applyEdit(g, op, h).error);
        CHECK(hashCells(g) == before && h.undo.empty());
        CHECK(!applyEdit(g, {.tool = Tool::Fill, .min = {0, 0, 0}, .max = {127, 127, 127}, .block = Stone}, h).error);
        CHECK(!applyEdit(g, {.tool = Tool::Raise, .radius = kMaxBrushRadius}, h).error);
        for (int i = 0; i < int(kMaxUndo) + 20; ++i)
            applyEdit(g, {.tool = Tool::Fill, .min = {i % 64, 90, 0}, .max = {i % 64, 90, 0}, .block = i / 64 % 2 ? Stone : Dirt}, h);
        CHECK(h.undo.size() == kMaxUndo);
    }
    {  // undo memory is capped too: the oldest steps are dropped
        Grid g({128, 128, 128});
        EditHistory h;
        size_t fit = kMaxUndoBytes / g.cells.size();  // whole-grid fills that fit
        for (size_t i = 0; i < fit + 3; ++i)
            applyEdit(g, {.tool = Tool::Fill, .min = {0, 0, 0}, .max = {127, 127, 127}, .block = i % 2 ? Air : Stone}, h);
        size_t total = 0;
        for (const EditRecord& r : h.undo) total += r.bytes;
        CHECK(h.undo.size() == fit && total == fit * g.cells.size());
    }
}

// The remote-command dispatcher, without sockets: JSON in, JSON out, the world and history changed.
static void commands() {
    std::vector<Marker> marks;
    Grid g = editWorld();
    const uint64_t original = hashCells(g);
    EditHistory h;
    GameView view;
    std::vector<Instance> instances;
    PrefabLibrary prefabs = loadPrefabs("no-such-folder");
    auto run = [&](std::string_view request) { return runCommand(request, g, instances, marks, h, prefabs, view); };
    auto ok = [&](const Command& c) { return c.response["ok"].get<bool>(); };

    Command c = run(R"({"id": 7, "cmd": "state"})");
    CHECK(ok(c) && c.response["id"] == 7 && c.response["result"]["size"][1] == 96 && c.response["result"]["undo_steps"] == 0);

    int before = groundTop(g, 20, 20);
    c = run(R"({"id": "r", "cmd": "raise", "args": {"x": 20, "z": 20, "radius": 4, "strength": 5}})");
    CHECK(ok(c) && c.response["id"] == "r" && c.response["result"]["changed"] == true && !c.dirty.empty());
    CHECK(groundTop(g, 20, 20) == before + 5 && h.undo.size() == 1);
    c = run(R"({"cmd": "heights", "args": {"x0": 20, "z0": 20, "x1": 20, "z1": 20}})");
    CHECK(ok(c) && c.response["result"]["heights"][0][0] == before + 5);

    // apply and path: many ops, one undo step; undo brings back exactly what was there
    uint64_t raised = hashCells(g);
    c = run(R"({"cmd": "apply", "args": {"ops": [
        {"tool": "flatten", "x": 40, "z": 40, "radius": 6, "height": 50},
        {"tool": "paint", "x": 40, "z": 40, "radius": 3, "material": "snow"},
        {"tool": "fill", "min": [10, 60, 10], "max": [12.0, 62, 12], "material": "Stone"}]}})");
    CHECK(ok(c) && c.response["result"]["ops"] == 3 && h.undo.size() == 2 && h.undo.back().parts.size() == 3);
    CHECK(groundTop(g, 40, 40) == 50 && g.get({40, 50, 40}) == Snow && g.get({11, 61, 11}) == Stone);
    c = run(R"({"cmd": "path", "args": {"points": [[5, 30], [60, 34], [60, 60]], "width": 5, "material": "Dirt"}})");
    CHECK(ok(c) && c.response["result"]["ops"].get<int>() > 20 && h.undo.size() == 3);
    CHECK(g.get({60, groundTop(g, 60, 45), 45}) == Dirt);
    CHECK(ok(run(R"({"cmd": "undo"})")) && ok(run(R"({"cmd": "undo"})")) && hashCells(g) == raised && h.redo.size() == 2);
    CHECK(ok(run(R"({"cmd": "redo"})")) && groundTop(g, 40, 40) == 50 && h.undo.size() == 2);
    c = run(R"({"cmd": "undo"})");
    CHECK(c.response["result"]["changed"] == true && !c.dirty.empty());
    run(R"({"cmd": "undo"})");
    CHECK(hashCells(g) == original && h.undo.empty() && run(R"({"cmd": "undo"})").response["result"]["changed"] == false);

    // bad requests and args are errors that change nothing
    for (std::string_view bad : {
             R"(not json)", R"([1, 2])", R"({"cmd": "explode"})", R"({"cmd": "raise", "args": []})",
             R"({"cmd": "raise", "args": {"x": 20}})",                                // no z
             R"({"cmd": "raise", "args": {"x": 20, "z": 20, "radius": 99}})",         // over the brush cap
             R"({"cmd": "lower", "args": {"x": 2000, "z": 20}})",                     // outside the world
             R"({"cmd": "flatten", "args": {"x": 20, "z": 20, "height": "high"}})",
             R"({"cmd": "paint", "args": {"x": 20, "z": 20, "material": "Water"}})",  // paint needs a solid block
             R"({"cmd": "fill", "args": {"min": [0, 0, 0], "max": [63, 95, 63], "material": "Lava"}})",
             R"({"cmd": "fill", "args": {"min": [0, 0, 0], "max": [1e300, 0, 0], "material": "Air"}})",
             R"({"cmd": "apply", "args": {"ops": [{"tool": "raise", "x": 1, "z": 1}, {"tool": "lower", "x": 1}]}})",
             R"({"cmd": "path", "args": {"points": [[1, 1]]}})",
             R"({"cmd": "inspect", "args": {"min": [0, 0, 0], "max": [1, 1]}})",
             R"({"cmd": "screenshot", "args": {"position": [1, 2, 3], "look_at": [1, 2, 3]}})",
             R"({"cmd": "screenshot", "args": {"position": [1, 2, 3], "yaw": 0, "width": 4000}})"}) {
        c = run(bad);
        CHECK(!ok(c) && c.response["error"].is_string() && c.dirty.empty() && !c.shot);
    }
    CHECK(hashCells(g) == original && h.undo.empty());
    c = run(R"({"id": 3, "cmd": "apply", "args": {"ops": [{"tool": "raise", "x": 1, "z": 1}, {"tool": "lower", "x": 1}]}})");
    CHECK(c.response["id"] == 3 && c.response["error"].get<std::string>().starts_with("op 1:"));

    // caps: a fill bigger than 128^3 (after clamping to the world) is rejected; heights stay under 4096 samples
    Grid big({256, 192, 256});
    EditHistory bh;
    CHECK(!ok(runCommand(R"({"cmd": "fill", "args": {"min": [0, 0, 0], "max": [255, 191, 255], "material": "Air"}})", big, instances, marks, bh, prefabs, view)));
    CHECK(!ok(runCommand(R"({"cmd": "inspect", "args": {"min": [0, 0, 0], "max": [255, 191, 255]}})", big, instances, marks, bh, prefabs, view)));
    c = runCommand(R"({"cmd": "heights", "args": {"x0": -50, "z0": 0, "x1": 9999, "z1": 255}})", big, instances, marks, bh, prefabs, view);
    const nlohmann::json& r = c.response["result"];
    CHECK(ok(c) && r["x0"] == 0 && r["columns"].get<int>() * r["rows"].get<int>() <= 4096 && r["top_blocks"].empty());
    c = run(R"({"cmd": "inspect", "args": {"min": [0, 0, 0], "max": [3, 95, 3]}})");
    int64_t total = 0;
    for (auto& [name, n] : c.response["result"]["blocks"].items()) total += n.get<int64_t>();
    CHECK(ok(c) && total == 4 * 96 * 4 && c.response["result"]["volume"] == total);

    // map writes a PNG; screenshot, goto and save hand their work to the game as data
    std::filesystem::path stale = std::filesystem::temp_directory_path() / "flycoos" / "stale.png";
    std::filesystem::create_directories(stale.parent_path());
    std::ofstream(stale) << "x";
    std::filesystem::last_write_time(stale, std::filesystem::file_time_type::clock::now() - std::chrono::hours(2));
    c = run(R"({"cmd": "map", "args": {"x0": 0, "z0": 0, "x1": 63, "z1": 63, "grid": 16}})");
    std::string png = c.response["result"].value("path", "");
    CHECK(!std::filesystem::exists(stale));  // PNGs older than an hour are cleaned up
    std::ifstream file(png, std::ios::binary);
    char magic[8] = {};
    file.read(magic, 8);
    CHECK(ok(c) && c.response["result"]["width"] == 1024 && std::string_view(magic + 1, 3) == "PNG");
    std::filesystem::remove(png);
    c = run(R"({"cmd": "screenshot", "args": {"position": [10, 80, 10], "look_at": [32, 40, 32], "width": 640, "height": 360}})");
    CHECK(ok(c) && c.shot && c.shot->eye == glm::vec3(10, 80, 10) && c.shot->width == 640 && c.shot->timeOfDay == view.timeOfDay);
    c = run(R"({"cmd": "screenshot", "args": {"position": [10, 80, 10], "yaw": 90, "pitch": 0}})");
    CHECK(ok(c) && glm::length(c.shot->target - c.shot->eye - glm::vec3(-1, 0, 0)) < 1e-5f);  // yaw 90 looks toward -X
    c = run(R"({"cmd": "goto", "args": {"position": [30, 70, 30], "look_at": [40, 40, 40]}})");
    CHECK(ok(c) && c.go && c.go->lookAt && c.go->pos == glm::vec3(30, 70, 30));
    CHECK(hashCells(g) == original);

    // a copy only goes to a .flyw file directly in worlds/ or next to the world file
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "flycoos_save_test", outside = fs::temp_directory_path() / "flycoos_outside.flyw";
    fs::create_directories(dir);
    std::ofstream(outside) << "x";
    std::error_code ec;
    fs::create_symlink(outside, dir / "link.flyw", ec);
    view.path = (dir / "w.flyw").string();
    auto save = [&](const std::string& path) { return run(nlohmann::json{{"cmd", "save"}, {"args", {{"path", path}}}}.dump()); };
    CHECK(run(R"({"cmd": "save"})").save == "");
    CHECK(save((dir / "copy.flyw").string()).save == fs::weakly_canonical(dir / "copy.flyw").string());
    CHECK(save("worlds/copy.flyw").save == fs::weakly_canonical("worlds/copy.flyw").string());  // from the working folder
    for (const std::string& bad : {(dir / "copy.txt").string(), (dir / ".." / "x.flyw").string(), (dir / "sub" / "x.flyw").string(),
                                   (dir / "link.flyw").string(), std::string("/etc/x.flyw"), std::string("worlds/../x.flyw"),
                                   std::string("worlds/x\0.flyw", 14)}) {
        c = save(bad);
        CHECK(!ok(c) && !c.save);
    }
    fs::remove_all(dir);
    fs::remove(outside);
    view.path.clear();

    // hostile input: invalid UTF-8 still gives a response that serializes; deep nesting is refused
    for (std::string_view bad : {"\xc3", "{\"cmd\": \"state\", \xff}", "{\"cmd\": \"x\xff\"}"}) {
        c = run(bad);
        std::string line;
        try {
            line = c.response.dump();
        } catch (const std::exception&) {
        }
        CHECK(!ok(c) && line.contains("\"ok\":false"));
    }
    std::string deep = R"({"id": 1, "cmd": "state", "args": {"a": )" + std::string(100000, '[') + std::string(100000, ']') + "}}";
    c = run(deep);
    CHECK(!ok(c) && c.response["error"].get<std::string>().contains("nested"));
    CHECK(!ok(run(deep.substr(0, 100000))));  // unclosed
    CHECK(ok(run(R"({"cmd": "state", "args": {"a": )" + std::string(32, '[') + std::string(32, ']') + "}}")));

    // work budget: too many big brushes, or edits spread too wide, are refused before anything changes
    std::string raises;
    for (int i = 0; i < 3; ++i) raises += std::format(R"({}{{"tool": "raise", "x": {}, "z": 100, "radius": 32}})", i ? "," : "", 40 + 20 * i);
    uint64_t bigBefore = hashCells(big);
    c = runCommand(R"({"cmd": "apply", "args": {"ops": [)" + raises + "]}}", big, instances, marks, bh, prefabs, view);
    CHECK(!ok(c) && c.response["error"].get<std::string>().contains("budget") && hashCells(big) == bigBefore && bh.undo.empty());
    CHECK(!ok(runCommand(R"({"cmd": "path", "args": {"points": [[0, 0], [255, 255]], "width": 65}})", big, instances, marks, bh, prefabs, view)));
    CHECK(ok(runCommand(R"({"cmd": "apply", "args": {"ops": [)" + raises.substr(0, raises.rfind(",{")) + "]}}", big, instances, marks, bh, prefabs, view)));
    Grid wide({256, 32, 256});
    EditHistory wh;
    auto corners = [&](int far) {
        return runCommand(std::format(R"({{"cmd": "apply", "args": {{"ops": [{{"tool": "fill", "min": [0, 0, 0], "max": [0, 0, 0], "material": "Stone"}},
            {{"tool": "fill", "min": [{0}, 0, {0}], "max": [{0}, 0, {0}], "material": "Stone"}}]}}}})", far), wide, instances, marks, wh, prefabs, view);
    };
    CHECK(ok(corners(223)) && !ok(corners(224)));  // 7x7 chunk columns to remesh are fine, 8x8 are too many

    // prefabs: list (rescanning the folder), place, move, remove, instances; each edit is one undo step
    g = editWorld();
    h = {};
    instances.clear();
    prefabs.dir = std::filesystem::path(FLYCOOS_SOURCE_DIR) / "prefabs";
    c = run(R"({"cmd": "prefabs"})");
    // Every shipped file loads without errors, after the 5 generated prefabs.
    auto shipped = std::ranges::count_if(std::filesystem::directory_iterator(prefabs.dir), [](const auto& f) { return f.path().extension() == ".txt"; });
    CHECK(ok(c) && c.response["result"]["prefabs"].size() == size_t(5 + shipped) && shipped >= 6 && c.response["result"]["errors"].empty());
    for (size_t i = 0; ok(c) && i < c.response["result"]["prefabs"].size(); ++i)
        CHECK(c.response["result"]["prefabs"][i].contains(i < 5 ? "params" : "size"));
    CHECK(c.response["result"]["prefabs"][0]["params"][0]["name"] == "trunk" && c.response["result"]["prefabs"][5].contains("size"));
    c = run(R"({"cmd": "place", "args": {"prefab": "cottage", "x": 30, "z": 30, "rotation": 90}})");
    CHECK(ok(c) && c.response["result"]["instance"]["id"] == 1 && c.response["result"]["instance"]["rotation"] == 90 && !c.dirty.empty());
    c = run(R"({"cmd": "place", "args": {"prefab": "tree", "x": 12, "z": 50, "y": 60, "seed": 4000000000, "params": {"trunk": 13.4, "radius": 3.5}}})");
    CHECK(ok(c) && c.response["result"]["instance"]["params"]["trunk"] == 13 && c.response["result"]["instance"]["seed"] == 4000000000u &&
          c.response["result"]["instance"]["position"][1] == 60 && h.undo.size() == 2);
    c = run(R"({"cmd": "move", "args": {"id": 1, "x": 45, "z": 20}})");
    CHECK(ok(c) && c.response["result"]["instance"]["position"][0] == 45 && c.response["result"]["instance"]["rotation"] == 90 && h.undo.size() == 3);
    c = run(R"({"cmd": "instances", "args": {"x0": 40, "z0": 10, "x1": 50, "z1": 30}})");
    CHECK(ok(c) && c.response["result"]["count"] == 1 && c.response["result"]["instances"][0]["prefab"] == "cottage");
    CHECK(run(R"({"cmd": "instances"})").response["result"]["count"] == 2 && run(R"({"cmd": "state"})").response["result"]["instances"] == 2);
    c = run(R"({"cmd": "remove", "args": {"id": 2}})");
    CHECK(ok(c) && c.response["result"]["removed"] == 2 && instances.size() == 1 && !c.dirty.empty());
    CHECK(ok(run(R"({"cmd": "undo"})")) && instances.size() == 2);
    uint64_t placedAll = hashCells(g);
    for (std::string_view bad : {
             R"({"cmd": "place", "args": {"prefab": "castle", "x": 10, "z": 10}})",
             R"({"cmd": "place", "args": {"x": 10, "z": 10}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 64, "z": 10}})",                              // outside the world
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": -1}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "y": 96}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "rotation": 45}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "rotation": 360}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "params": {"trunk": 99}}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "params": {"height": 9}}})",   // not a tree parameter
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "params": [10]}})",
             R"({"cmd": "place", "args": {"prefab": "cottage", "x": 10, "z": 10, "params": {"trunk": 9}}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "seed": -1}})",
             R"({"cmd": "place", "args": {"prefab": "bush", "x": 10, "z": 10, "y": 2}})",                    // buried: changes nothing
             R"({"cmd": "remove", "args": {"id": 7}})",
             R"({"cmd": "remove", "args": {}})",
             R"({"cmd": "move", "args": {"id": 7, "x": 10, "z": 10}})",
             R"({"cmd": "move", "args": {"id": 1, "x": 10, "z": 1000}})",
             R"({"cmd": "move", "args": {"id": 1, "x": 10, "z": 10, "rotation": -90}})",
             R"({"cmd": "instances", "args": {"x0": 1}})"}) {
        c = run(bad);
        CHECK(!ok(c) && c.response["error"].is_string() && c.dirty.empty());
    }
    CHECK(hashCells(g) == placedAll && instances.size() == 2 && h.undo.size() == 3);
    Grid far({1024, 64, 1024});
    std::vector<Instance> farList;
    EditHistory fh;
    CHECK(ok(runCommand(R"({"cmd": "place", "args": {"prefab": "boulder", "x": 10, "z": 10, "y": 5}})", far, farList, marks, fh, prefabs, view)));
    c = runCommand(R"({"cmd": "move", "args": {"id": 1, "x": 1000, "z": 1000, "y": 5}})", far, farList, marks, fh, prefabs, view);
    CHECK(!ok(c) && c.response["error"].get<std::string>().contains("budget") && farList[0].pos.x == 10);
}

static const Prefab& prefabNamed(const PrefabLibrary& lib, std::string_view name) {
    const Prefab* p = lib.find(name);
    CHECK(p);
    return *p;
}

// Grass plain, 64 x 32 x 64, ground top at y = 3.
static Grid plain() {
    Grid g({64, 32, 64});
    for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x)
            for (int y = 0; y <= 3; ++y) g.set({x, y, z}, y == 3 ? Grass : Stone);
    return g;
}

static Instance at(const Prefab& p, glm::ivec3 pos, int rotation = 0, uint32_t seed = 5) {
    Instance in{.prefab = p.name, .pos = pos, .rotation = rotation, .seed = seed};
    for (const PrefabParam& q : p.params) in.params.push_back(q.fallback);
    return in;
}

static void prefabs() {
    std::vector<Marker> marks;
    std::optional<std::vector<Marker>> fileMarks;
    namespace fs = std::filesystem;
    {  // a good file: header, legend (any case), comments, short rows padded with keep, trailing blank lines ignored
        Prefab p{.name = "hut"};
        CHECK(parsePrefab("# a hut\ndescription: A hut: small\nfoundation: cobble\ny: -1\nS = stone\nw = Wood\n\nlayer\nSSS\nS.S\nlayer\n w\n\n#c\n w \n\n", p).empty());
        CHECK(p.name == "hut" && p.description == "A hut: small" && p.foundation == Cobble && p.y == -1 && p.size == glm::ivec3(3, 2, 3));
        CHECK(p.cells[0] == Stone && p.cells[4] == Air && p.cells[6] == kKeep && p.cells[9] == kKeep && p.cells[10] == Wood && p.cells[12] == kKeep &&
              p.cells[13] == kKeep && p.cells[16] == Wood);
        CHECK(!p.generated());
    }
    {  // every kind of error names its line
        std::string wide = "S = Stone\nlayer\n" + std::string(65, 'S') + "\n", tall = "S = Stone\n";
        for (int i = 0; i < 65; ++i) tall += "layer\nS\n";
        std::pair<std::string, std::string> bad[] = {
            {"description: x\nS = Lava\nlayer\nS\n", "line 2: unknown block 'Lava'"},
            {"S = Stone\nlayer\nSX\n", "line 3: 'X' is not in the legend"},
            {"S = Stone\nlayer\nS\tS\n", "line 3: tab"},
            {"S = Stone\nS = Dirt\n", "line 2: 'S' is already in the legend"},
            {". = Stone\n", "line 1: '.' can't be a legend character"},
            {"colour: red\n", "line 1: unknown key 'colour'"},
            {"what is this\n", "line 1: expected"},
            {"foundation: Water\n", "line 1: foundation must be a solid block"},
            {"y: 1.5\n", "line 1: y must be an integer"},
            {"S = Stone\n\n", "line 2: no layers"},
            {"S = Stone\nlayer\nlayer\nS\n", "line 2: empty layer"},
            {"S = Stone\nlayer\n   \nlayer\n.\n", "line 2: empty layer"},
            {"layer\n  \n \nlayer\n   \n x", "line 6: 'x' is not in the legend"},
            {"S = Stone\nlayer\n  \n \n", "line 2: empty layer"},
            {wide, "line 3: row longer than 64"},
            {tall, "line 130: more than 64 layers"},
        };
        for (const auto& [text, error] : bad) {
            Prefab p{.name = "x"};
            std::string got = parsePrefab(text, p);
            CHECK(got.starts_with(error));
            if (!got.starts_with(error)) std::println(stderr, "  parse error: got '{}', want '{}'", got, error);
        }
        Prefab p{.name = "x"};
        CHECK(parsePrefab("layer\n   \n", p) == "line 1: empty layer");
        CHECK(parsePrefab("y: -2147483648\n", p).starts_with("line 1: y must be"));  // no abs(INT_MIN)
        CHECK(parsePrefab("description: crlf\r\nS = Stone\r\nlayer\r\nSS\r\n", p).empty() && p.size == glm::ivec3(2, 1, 1) &&
              p.description == "crlf");
    }
    {  // the library: generated prefabs, then regular .txt files directly in the folder; broken files are reported
        fs::path dir = fs::temp_directory_path() / "flycoos_prefab_test";
        fs::remove_all(dir);
        fs::create_directories(dir / "sub");
        std::ofstream(dir / "box.txt") << "S = Stone\nlayer\nSS\n";
        std::ofstream(dir / "broken.txt") << "S = Stone\nlayer\nSQ\n";
        std::ofstream(dir / "tree.txt") << "S = Stone\nlayer\nS\n";
        std::ofstream(dir / "notes.md") << "S = Stone\nlayer\nS\n";
        std::ofstream(dir / "sub" / "deep.txt") << "S = Stone\nlayer\nS\n";
        std::ofstream(dir / "huge.txt") << std::string(kMaxPrefabFileBytes + 1, '#');
        std::error_code ec;
        fs::create_symlink(dir / "box.txt", dir / "link.txt", ec);
        PrefabLibrary lib = loadPrefabs(dir);
        std::vector<std::string> names;
        for (const Prefab& p : lib.prefabs) names.push_back(p.name);
        CHECK((names == std::vector<std::string>{"tree", "pine", "bush", "boulder", "spire", "box"}));
        CHECK(lib.errors.size() == 3 && lib.errors[0] == "broken.txt: line 3: 'Q' is not in the legend" &&
              lib.errors[1].starts_with("huge.txt") && lib.errors[2].starts_with("tree.txt"));
        fs::remove_all(dir);
        CHECK(loadPrefabs(dir).errors.size() == 1);  // a missing folder is reported, the generated prefabs remain
        fs::create_directories(dir);
        for (int i = 0; i < 8; ++i)  // 8 x 260 KB: the last one is over the 2 MB total
            std::ofstream(dir / std::format("big{}.txt", i)) << "#" << std::string(260 << 10, 'x') << "\nS = Stone\nlayer\nS\n";
        lib = loadPrefabs(dir);
        CHECK(lib.prefabs.size() == 5 + 7 && lib.errors.size() == 1 && lib.errors[0].starts_with("big7.txt: skipped"));
        fs::remove_all(dir);
        PrefabLibrary shipped = loadPrefabs(fs::path(FLYCOOS_SOURCE_DIR) / "prefabs");
        for (const std::string& e : shipped.errors) std::println(stderr, "  shipped prefab: {}", e);
        CHECK(shipped.errors.empty());
        for (const char* name : {"cottage", "tent", "camp", "well", "fence", "ruin"}) CHECK(shipped.find(name));
    }

    PrefabLibrary lib = loadPrefabs(fs::path(FLYCOOS_SOURCE_DIR) / "prefabs");
    {  // rotation: an asymmetric prefab lands as four quarter turns counterclockwise seen from above (+x toward -z)
        Prefab p{.name = "l"};
        CHECK(parsePrefab("A = Stone\nB = Sand\nC = Snow\nD = Planks\nlayer\nABC\nD\n", p).empty());
        const glm::ivec3 expected[4][4] = {  // where A, B, C, D land with the anchor column at (8, 8), ground y 3
            {{7, 4, 7}, {8, 4, 7}, {9, 4, 7}, {7, 4, 8}},
            {{7, 4, 9}, {7, 4, 8}, {7, 4, 7}, {8, 4, 9}},
            {{9, 4, 8}, {8, 4, 8}, {7, 4, 8}, {9, 4, 7}},
            {{8, 4, 7}, {8, 4, 8}, {8, 4, 9}, {7, 4, 7}},
        };
        for (int r = 0; r < 4; ++r) {
            Grid g = plain();
            std::vector<Instance> list;
            EditHistory h;
            CHECK(!placePrefab(g, p, at(p, {8, 0, 8}, r), std::nullopt, list, h).error);
            constexpr Block kBlocks[] = {Stone, Sand, Snow, Planks};
            for (int i = 0; i < 4; ++i) CHECK(g.get(expected[r][i]) == kBlocks[i]);
            CHECK(list.size() == 1 && list[0].cells.size() == 4 && list[0].pos == glm::ivec3(8, 3, 8));
        }
    }
    {  // every prefab: placing then removing restores the exact world; generators are deterministic per seed
        Grid g = editWorld();
        const uint64_t original = hashCells(g);
        std::vector<Instance> list;
        EditHistory h;
        glm::ivec3 dry(findSpawn(g, {32, 30}));  // on grass, so the bush has air to grow into
        for (const Prefab& p : lib.prefabs)
            for (int r = 0; r < 4; ++r) {
                EditResult placed = placePrefab(g, p, at(p, dry, r), std::nullopt, list, h);
                CHECK(!placed.error && !placed.dirty.empty() && list.size() == 1);
                if (placed.error) {
                    std::println(stderr, "  {} rotation {}: {}", p.name, r, placed.error);
                    continue;
                }
                Box box = prefabBox(g, p, list[0]), written = instanceBox(list[0]);
                CHECK(glm::all(glm::greaterThanEqual(written.lo, box.lo)) && glm::all(glm::lessThanEqual(written.hi, box.hi)));
                CHECK(!removePrefab(g, placed.id, list, h).error && list.empty());
                if (hashCells(g) != original) std::println(stderr, "  {} rotation {} left changes behind", p.name, r);
                CHECK(hashCells(g) == original);
            }
        const Prefab& tree = prefabNamed(lib, "tree");
        Grid a = editWorld(), b = editWorld(), c = editWorld();
        std::vector<Instance> la, lb, lc;
        placePrefab(a, tree, at(tree, {20, 0, 20}), std::nullopt, la, h);
        placePrefab(b, tree, at(tree, {20, 0, 20}), std::nullopt, lb, h);
        placePrefab(c, tree, at(tree, {20, 0, 20}, 0, 6), std::nullopt, lc, h);
        CHECK(a.cells == b.cells && a.cells != c.cells);
    }
    {  // remove keeps what was sculpted over the prefab; move; undo/redo restore voxels and the instance list together
        Grid g = plain();
        const uint64_t original = hashCells(g);
        std::vector<Instance> list;
        EditHistory h;
        const Prefab& cottage = prefabNamed(lib, "cottage");
        uint32_t id = placePrefab(g, cottage, at(cottage, {10, 0, 10}), std::nullopt, list, h).id;
        CHECK(id == 1 && g.get({10, 5, 10}) == Air && g.get({10, 5, 7}) == Planks && g.get({10, 4, 10}) == Planks);  // room, back wall, floor
        applyEdit(g, {.tool = Tool::Fill, .min = {8, 5, 7}, .max = {12, 5, 7}, .block = Stone}, h);  // sculpt over its wall
        applyEdit(g, {.tool = Tool::Fill, .min = {10, 6, 10}, .max = {10, 6, 10}, .block = Snow}, h);  // and into its room
        CHECK(!removePrefab(g, id, list, h).error && list.empty());
        CHECK(g.get({10, 5, 7}) == Stone && g.get({10, 6, 10}) == Snow && g.get({10, 7, 7}) == Air && g.get({10, 4, 10}) == Air);
        undoEdit(g, h, list, marks), undoEdit(g, h, list, marks), undoEdit(g, h, list, marks);
        CHECK(list.size() == 1 && g.get({10, 5, 7}) == Planks && g.get({10, 6, 10}) == Air);

        // move: one undo step, the old spot back as it was, the same id at the new spot
        uint64_t placed = hashCells(g);
        size_t steps = h.undo.size();
        EditResult moved = movePrefab(g, cottage, id, {40, 40}, std::nullopt, 1, list, h);
        CHECK(!moved.error && moved.id == id && h.undo.size() == steps + 1 && list.size() == 1 && list[0].rotation == 1);
        CHECK(g.get({10, 5, 7}) == Air && g.get({40, 5, 40}) == Air && g.get({37, 5, 40}) == Planks);  // the back wall turned to -x
        uint64_t afterMove = hashCells(g);
        CHECK(!undoEdit(g, h, list, marks).empty() && hashCells(g) == placed && list.size() == 1 && list[0].pos.x == 10 && list[0].rotation == 0);
        CHECK(!redoEdit(g, h, list, marks).empty() && hashCells(g) == afterMove && list[0].pos.x == 40);
        CHECK(movePrefab(g, cottage, 99, {40, 40}, std::nullopt, 0, list, h).error && hashCells(g) == afterMove);
        CHECK(movePrefab(g, cottage, id, {40, 40}, std::nullopt, 4, list, h).error && hashCells(g) == afterMove);
        CHECK(!removePrefab(g, id, list, h).error && list.empty());
        CHECK(!undoEdit(g, h, list, marks).empty() && list.size() == 1 && hashCells(g) == afterMove);
        while (!undoEdit(g, h, list, marks).empty()) {}
        CHECK(hashCells(g) == original && list.empty());
        while (!redoEdit(g, h, list, marks).empty()) {}  // place, move, remove
        CHECK(hashCells(g) == original && list.empty() && h.redo.empty() && h.undo.size() == 3);
        CHECK(placePrefab(g, cottage, at(cottage, {60, 0, 60}), std::nullopt, list, h).id == 2);  // ids are never reused

        const Prefab& bush = prefabNamed(lib, "bush");  // a move that would change nothing leaves everything as it was
        uint32_t b = placePrefab(g, bush, at(bush, {20, 0, 20}), std::nullopt, list, h).id;
        applyEdit(g, {.tool = Tool::Fill, .min = {1, 9, 1}, .max = {1, 9, 1}, .block = Stone}, h);
        undoEdit(g, h, list, marks);
        uint64_t before = hashCells(g);
        size_t undos = h.undo.size();
        CHECK(movePrefab(g, bush, b, {25, 25}, 0, 0, list, h).error && hashCells(g) == before && h.undo.size() == undos &&
              h.redo.size() == 1 && list.back().pos == glm::ivec3(20, 3, 20));
    }
    {  // placement checks
        Grid g = plain();
        std::vector<Instance> list;
        EditHistory h;
        const Prefab& tree = prefabNamed(lib, "tree");
        Instance in = at(tree, {8, 0, 8});
        in.params[0] = 30;
        CHECK(placePrefab(g, tree, in, std::nullopt, list, h).error);
        CHECK(placePrefab(g, tree, at(tree, {-1, 0, 8}), std::nullopt, list, h).error);
        CHECK(placePrefab(g, tree, at(tree, {8, 0, 8}, 4), std::nullopt, list, h).error);
        CHECK(placePrefab(g, prefabNamed(lib, "bush"), at(prefabNamed(lib, "bush"), {8, 0, 8}), 0, list, h).error);  // buried: changes nothing
        CHECK(list.empty() && h.undo.empty());
    }
    {  // review regressions: caps, id overflow, foundation under gaps, untrusted instances from files
        Grid g = plain();
        const uint64_t original = hashCells(g);
        EditHistory h;
        const Prefab& fence = prefabNamed(lib, "fence");
        std::vector<Instance> full(kMaxInstances, Instance{.cells = {{1, 1, 1, Air, Stone}}});
        CHECK(placePrefab(g, fence, at(fence, {20, 0, 20}), std::nullopt, full, h).error && hashCells(g) == original);
        std::vector<Instance> heavy{Instance{.id = 1, .cells = std::vector<Instance::Cell>(kMaxInstanceCells - 2, {1, 1, 1, Air, Stone})}};
        CHECK(placePrefab(g, fence, at(fence, {20, 0, 20}), std::nullopt, heavy, h).error && hashCells(g) == original && heavy.size() == 1);
        std::vector<Instance> last{Instance{.id = UINT32_MAX, .cells = {{1, 1, 1, Air, Stone}}}};
        CHECK(placePrefab(g, fence, at(fence, {20, 0, 20}), std::nullopt, last, h).error && hashCells(g) == original && h.undo.empty());

        // the fence's foundation fills under its posts only, not under the gaps between them
        for (int x = 17; x <= 18; ++x) g.set({x, 3, 20}, Air), g.set({x, 2, 20}, Air);
        std::vector<Instance> list;
        CHECK(!placePrefab(g, fence, at(fence, {20, 0, 20}), 3, list, h).error);
        CHECK(g.get({17, 4, 20}) == Wood && g.get({17, 3, 20}) == Wood && g.get({17, 2, 20}) == Wood && g.get({18, 3, 20}) == Air);

        // a crafted file: tree instances with missing or absurd params load (the loader doesn't know prefabs), but
        // move refuses them instead of reading past their params; remove still works
        auto tree = [](uint32_t id, std::vector<float> params) {
            return Instance{.id = id, .prefab = "tree", .pos = {10, 3, 10}, .params = std::move(params), .cells = {{10, 5, 10, Air, Wood}}};
        };
        std::string path = (fs::temp_directory_path() / "flycoos_crafted.flyw").string();
        CHECK(saveWorld(path.c_str(), g, 1, {tree(1, {}), tree(2, {10}), tree(3, {1e30f, 1e30f})}, {}).empty());
        std::vector<Instance> loaded;
        uint32_t seed;
        CHECK(loadWorld(path.c_str(), g, seed, loaded, fileMarks).empty() && loaded.size() == 3);
        CHECK(prefabBox(g, prefabNamed(lib, "tree"), loaded[0]).empty() && prefabBox(g, prefabNamed(lib, "tree"), loaded[2]).empty());
        GameView view;
        for (int id = 1; id <= 3; ++id) {
            Command c = runCommand(std::format(R"({{"cmd": "move", "args": {{"id": {}, "x": 30, "z": 30}}}})", id), g, loaded, marks, h, lib, view);
            CHECK(!c.response["ok"].get<bool>() && c.dirty.empty());
        }
        CHECK(runCommand(R"({"cmd": "remove", "args": {"id": 3}})", g, loaded, marks, h, lib, view).response["ok"] == true && loaded.size() == 2);

        // the loader rejects what placing can't produce: ids 0 and 2^32-1, x or z of -1, too many params, cells spread too far
        Grid wide({128, 32, 128});
        std::vector<Instance> bad[] = {
            {Instance{.id = UINT32_MAX, .cells = {{1, 1, 1, Air, Stone}}}},
            {Instance{.id = 1, .pos = {-1, 3, 5}, .cells = {{1, 1, 1, Air, Stone}}}},
            {Instance{.id = 1, .params = std::vector<float>(kMaxInstanceParams + 1, 1), .cells = {{1, 1, 1, Air, Stone}}}},
            {Instance{.id = 1, .cells = {{0, 0, 0, Air, Stone}, {127, 0, 0, Air, Stone}}}},
        };
        for (const std::vector<Instance>& b : bad) {
            CHECK(saveWorld(path.c_str(), wide, 1, b, {}).empty());
            CHECK(loadWorld(path.c_str(), wide, seed, loaded, fileMarks).starts_with("bad instance"));
        }
        fs::remove(path);
    }
    {  // world files keep the instances; files without them load with none; corrupt instance data is rejected
        Grid g = plain();
        std::vector<Instance> list;
        EditHistory h;
        placePrefab(g, prefabNamed(lib, "well"), at(prefabNamed(lib, "well"), {8, 0, 8}, 1, 77), std::nullopt, list, h);
        placePrefab(g, prefabNamed(lib, "spire"), at(prefabNamed(lib, "spire"), {20, 0, 20}), std::nullopt, list, h);
        removePrefab(g, 1, list, h);
        placePrefab(g, prefabNamed(lib, "fence"), at(prefabNamed(lib, "fence"), {30, 0, 8}), std::nullopt, list, h);
        std::string path = (fs::temp_directory_path() / "flycoos_instances.flyw").string();
        CHECK(saveWorld(path.c_str(), g, 5, list, {}).empty());
        Grid back({32, 32, 32});
        uint32_t seed = 0;
        std::vector<Instance> loaded{Instance{}};
        CHECK(loadWorld(path.c_str(), back, seed, loaded, fileMarks).empty() && back.cells == g.cells && loaded.size() == 2);
        for (size_t i = 0; i < loaded.size(); ++i) {
            const Instance &a = loaded[i], &b = list[i];
            CHECK(a.id == b.id && a.prefab == b.prefab && a.pos == b.pos && a.rotation == b.rotation && a.seed == b.seed &&
                  a.params == b.params && a.cells.size() == b.cells.size() &&
                  std::equal(a.cells.begin(), a.cells.end(), b.cells.begin(), [](auto& p, auto& q) {
                      return p.x == q.x && p.y == q.y && p.z == q.z && p.before == q.before && p.after == q.after;
                  }));
        }
        CHECK(!removePrefab(back, 2, loaded, h).error && loaded.size() == 1);  // a loaded instance still removes cleanly

        std::ifstream in(path, std::ios::binary);
        const std::string file((std::istreambuf_iterator<char>(in)), {});
        auto reseal = [](std::string bytes) {
            uint32_t hash = 2166136261u;
            for (size_t i = 0; i + 4 < bytes.size(); ++i) hash = (hash ^ uint8_t(bytes[i])) * 16777619u;
            for (int i = 0; i < 4; ++i) bytes[bytes.size() - 4 + size_t(i)] = char(hash >> (8 * i));
            return bytes;
        };
        size_t inst = file.find("INST");
        CHECK(inst != std::string::npos);
        int rejected = 0;
        for (size_t i = inst + 12; i + 4 < file.size(); i += 3) {  // flip bytes past the checksum: never crash or overrun
            std::string bad = file;
            bad[i] = char(bad[i] ^ 0xA5);
            std::ofstream(path, std::ios::binary | std::ios::trunc) << reseal(bad);
            rejected += !loadWorld(path.c_str(), back, seed, loaded, fileMarks).empty();
        }
        CHECK(rejected > 10);
        for (size_t cut : {size_t(1), size_t(9), size_t(30)}) {
            std::string bad = file.substr(0, file.size() - 4 - cut) + file.substr(file.size() - 4);
            std::ofstream(path, std::ios::binary | std::ios::trunc) << reseal(bad);
            CHECK(!loadWorld(path.c_str(), back, seed, loaded, fileMarks).empty());
        }
        CHECK(saveWorld(path.c_str(), g, 5, {}, {}).empty());  // no instances: no section, and loading clears the list
        loaded = list;
        CHECK(loadWorld(path.c_str(), back, seed, loaded, fileMarks).empty() && loaded.empty());
        std::ifstream noInst(path, std::ios::binary);
        CHECK(std::string((std::istreambuf_iterator<char>(noInst)), {}).find("INST") == std::string::npos);
        fs::remove(path);
    }
}

// Story markers: validation, one undo step per edit (markers and NPC spawns stay consistent), files
// (round trip, older files, hostile data), commands, regions and spawns.
static void markers() {
    Grid g = meadow();  // grass top at y = 3 (feet at 4), pond at x >= 32
    auto marker = [](MarkerKind kind, const char* name, glm::vec3 pos, float radius) {
        return Marker{.kind = kind, .name = name, .pos = pos, .radius = radius};
    };
    Marker poi = marker(MarkerKind::Poi, "Old Well", {10.5f, 4, 10.5f}, 16);
    CHECK(!checkMarker(g, poi));
    CHECK(!checkMarker(g, marker(MarkerKind::Region, "Gr\xC3\xBCnwald \xE2\x9B\xB0 \xF0\x9F\x8C\xB2", {0, 0, 63.9f}, 512)));  // UTF-8, edges
    CHECK(!checkMarker(g, marker(MarkerKind::Poi, std::string(48, 'a').c_str(), {1, 16, 1}, 0)));
    for (auto [field, bad] : std::initializer_list<std::pair<int, float>>{
             {0, NAN}, {0, -0.1f}, {0, 64}, {1, -1}, {1, 16.5f}, {2, INFINITY}, {3, NAN}, {3, -1}, {3, 513}}) {
        Marker m = poi;
        (field < 3 ? m.pos[field] : m.radius) = bad;
        CHECK(checkMarker(g, m));
    }
    for (const char* name : {"", "\xC3", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "a\nb", "a\x7F", "\xC2\x85", "\xFF"}) {
        Marker m = poi;
        m.name = name;
        CHECK(checkMarker(g, m));
    }
    Marker bad = poi;
    bad.name = std::string(49, 'a');
    CHECK(checkMarker(g, bad));
    bad = poi, bad.kind = MarkerKind(9);
    CHECK(checkMarker(g, bad));
    bad = poi, bad.entity = EntityKind::Count;
    CHECK(checkMarker(g, bad));
    bad = poi, bad.count = 0;
    CHECK(checkMarker(g, bad));
    bad = poi, bad.count = kMaxSpawnCount + 1;
    CHECK(checkMarker(g, bad));
    Marker anon = marker(MarkerKind::Spawn, "", {5.5f, 4, 5.5f}, 6);  // spawns and the player spawn may be unnamed
    CHECK(!checkMarker(g, anon));

    // Regions: the smallest one containing the point wins.
    std::vector<Marker> list = {marker(MarkerKind::Region, "Vale", {20, 4, 20}, 30), marker(MarkerKind::Region, "Hamlet", {25, 4, 20}, 8),
                                marker(MarkerKind::Poi, "Stone", {20, 4, 20}, 2)};
    CHECK(regionAt(list, {26, 4, 21}) == &list[1] && regionAt(list, {10, 4, 20}) == &list[0]);
    CHECK(regionAt(list, {60, 4, 60}) == nullptr && regionAt({}, {1, 1, 1}) == nullptr);

    // Edits and undo: markers and the NPCs of spawn markers move together.
    list.clear();
    EditHistory h;
    std::vector<Instance> none;
    std::vector<Entity> npcs;
    auto sync = [&] { syncSpawns(npcs, g, list, 1, {16, 4, 16}); };
    Marker boars = marker(MarkerKind::Spawn, "Boar Den", {10.5f, 4, 20.5f}, 6);
    boars.entity = EntityKind::Boar;
    boars.count = 3;
    EditResult r = putMarker(g, list, h, boars);
    CHECK(!r.error && r.id == 1 && r.dirty.lo == glm::ivec3(10, 4, 20) && list.size() == 1 && h.undo.size() == 1);
    sync();
    CHECK(npcs.size() == 3);
    for (const Entity& e : npcs) {
        CHECK(e.kind == EntityKind::Boar && e.spawn.id == 1 && e.wanderRadius == 6 && e.home == e.body.pos);
        CHECK(glm::distance(e.home, boars.pos) < 4 && e.home.y == 4 && !overlapsSolid(g, e.home));
    }
    CHECK(npcs[0].home == boars.pos);
    r = putMarker(g, list, h, poi);
    CHECK(r.id == 2);
    damage(npcs[1], 5);
    sync();  // the boars' marker didn't change: same entities
    CHECK(npcs.size() == 3 && npcs[1].body.health == 95);
    Marker moved = list[0];
    moved.pos = {20.5f, 4, 40.5f};
    moved.count = 5;
    CHECK(!putMarker(g, list, h, moved).error && h.undo.size() == 3);
    CHECK(!putMarker(g, list, h, moved).error && h.undo.size() == 3);  // no change: no undo step
    sync();
    CHECK(npcs.size() == 5 && npcs[0].home == moved.pos && npcs[1].body.health == npcs[1].type().stats.stamina * 10);
    CHECK(!undoEdit(g, h, none, list).empty() && list[0].pos == boars.pos && list[0].count == 3);
    sync();
    CHECK(npcs.size() == 3 && npcs[0].home == boars.pos);
    CHECK(!redoEdit(g, h, none, list).empty());
    sync();
    CHECK(npcs.size() == 5 && npcs[0].home == moved.pos);
    r = removeMarker(g, list, h, 1);
    CHECK(!r.error && list.size() == 1 && list[0].id == 2);
    sync();
    CHECK(npcs.empty());
    undoEdit(g, h, none, list);
    sync();
    CHECK(list.size() == 2 && list[0].id == 1 && npcs.size() == 5);
    CHECK(removeMarker(g, list, h, 77).error && putMarker(g, list, h, Marker{.id = 77, .name = "x", .pos = {1, 4, 1}}).error);
    CHECK(putMarker(g, list, h, bad).error && list.size() == 2);
    // Ids come after the highest one an undo or redo could bring back; the player spawn is unique.
    undoEdit(g, h, none, list), undoEdit(g, h, none, list);  // the move and the poi: id 2 is only in the redo stack now
    Marker start = marker(MarkerKind::PlayerSpawn, "", {40.5f, 4, 8.5f}, 0);
    CHECK(playerSpawn(g, list) == findSpawn(g));
    r = putMarker(g, list, h, start);
    CHECK(!r.error && r.id == 3 && playerSpawn(g, list) == start.pos);
    CHECK(putMarker(g, list, h, start).error);  // a second one
    start.id = r.id, start.pos.x = 5.5f;
    CHECK(!putMarker(g, list, h, start).error && playerSpawn(g, list).x == 5.5f);  // moving it is fine
    std::vector<Marker> full(kMaxMarkers, poi);
    for (size_t i = 0; i < full.size(); ++i) full[i].id = uint32_t(i + 1);
    EditHistory fh;
    CHECK(putMarker(g, full, fh, poi).error && full.size() == kMaxMarkers);
    std::vector<Marker> last = {poi};
    last[0].id = UINT32_MAX - 1;
    CHECK(putMarker(g, last, fh, poi).error && last.size() == 1);  // ids never reach 2^32-1 or wrap to 0

    // Respawning: a spawn marker's dead NPC comes back at its home by the marker; the player respawns
    // at the player spawn (main.cpp uses playerSpawn).
    sync();
    Entity& boar = npcs[0];
    std::mt19937 rng(1);
    Player far;
    far.pos = {60, 4, 60};
    damage(boar, 1000);
    boar.body.pos = {20, 4, 12};  // died elsewhere
    for (int i = 0; i < 31 * 60; ++i) updateCombat(far, npcs, 1 / 60.0f, rng);
    CHECK(boar.body.alive() && boar.body.pos == boars.pos);

    // Files: a round trip keeps every field; older files have no marker section; hostile sections fail cleanly.
    std::string path = (std::filesystem::temp_directory_path() / "flycoos_markers_test.flyw").string();
    std::optional<std::vector<Marker>> loaded;
    std::vector<Instance> instances;
    uint32_t seed = 0;
    Grid fg({64, 32, 64});  // world files need whole chunks
    fg.set({1, 2, 3}, Stone);
    CHECK(saveWorld(path.c_str(), fg, 5, {}, list).empty());
    Grid back({32, 32, 32});
    CHECK(loadWorld(path.c_str(), back, seed, instances, loaded).empty() && loaded && *loaded == list);
    CHECK(saveWorld(path.c_str(), fg, 5, {}, {}).empty() && loadWorld(path.c_str(), back, seed, instances, loaded).empty() && loaded &&
          loaded->empty());  // an empty section: the world has no markers (not "an older file")
    std::ifstream in(path, std::ios::binary);
    const std::string file((std::istreambuf_iterator<char>(in)), {});
    in.close();
    size_t at = file.rfind("MARK");
    auto seal = [&](std::string payload) {  // this file with the given MARK payload (empty: no section)
        std::string bytes = file.substr(0, at);
        if (!payload.empty()) {
            bytes += "MARK";
            for (int i = 0; i < 8; ++i) bytes += char(uint64_t(payload.size()) >> (8 * i));
            bytes += payload;
        }
        uint32_t hash = 2166136261u;
        for (char c : bytes) hash = (hash ^ uint8_t(c)) * 16777619u;
        for (int i = 0; i < 4; ++i) bytes += char(hash >> (8 * i));
        std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
        return loadWorld(path.c_str(), back, seed, instances, loaded);
    };
    CHECK(seal("").empty() && !loaded && back.cells == fg.cells);  // an older file
    auto u32 = [](uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
    auto f32 = [&](float v) { return u32(std::bit_cast<uint32_t>(v)); };
    // One marker: id, kind, name, x, y, z, radius, entity, count.
    auto one = [&](uint32_t id, uint8_t kind, std::string name, glm::vec3 p, float radius, uint8_t entity, uint8_t count) {
        return u32(id) + char(kind) + char(name.size()) + name + f32(p.x) + f32(p.y) + f32(p.z) + f32(radius) + char(entity) + char(count);
    };
    std::string good = one(4, 2, "Well", {10.5f, 4, 10.5f}, 8, 1, 1);
    CHECK(seal(u32(1) + good).empty() && loaded && loaded->size() == 1 && (*loaded)[0].name == "Well");
    for (const std::string& hostile : {
             u32(1) + one(4, 7, "Well", {10, 4, 10}, 8, 1, 1),            // unknown kind
             u32(1) + one(4, 1, "Den", {10, 4, 10}, 8, 9, 1),             // unknown entity
             u32(1) + one(4, 1, "Den", {10, 4, 10}, 8, 3, 200),           // huge count
             u32(1) + one(4, 1, "Den", {10, 4, 10}, 8, 3, 0),             // no count
             u32(1) + one(4, 2, "Well", {NAN, 4, 10}, 8, 1, 1),           // NaN
             u32(1) + one(4, 2, "Well", {10, 4, 10}, INFINITY, 1, 1),     // infinite radius
             u32(1) + one(4, 2, "Well", {10, 4, 9000}, 8, 1, 1),          // outside the world
             u32(1) + one(0, 2, "Well", {10, 4, 10}, 8, 1, 1),            // id 0
             u32(1) + one(UINT32_MAX, 2, "Well", {10, 4, 10}, 8, 1, 1),   // id 2^32-1
             u32(1) + one(4, 2, std::string(200, 'w'), {10, 4, 10}, 8, 1, 1),  // name too long
             u32(1) + one(4, 2, "W\xC3\x28ll", {10, 4, 10}, 8, 1, 1),     // invalid UTF-8
             u32(2) + good + good,                                         // ids not ascending
             u32(2) + one(1, 0, "", {1, 4, 1}, 0, 1, 1) + one(2, 0, "", {2, 4, 2}, 0, 1, 1),  // two player spawns
             u32(5000) + good,                                             // too many
             u32(2) + good,                                                // truncated
             u32(1) + good + "x",                                          // trailing bytes
             u32(1) + good.substr(0, 9),                                   // cut inside the name
         }) {
        std::string error = seal(hostile);
        CHECK(!error.empty() && (error.contains("marker") || error.contains("truncated")));
    }

    // Older worlds: the default markers spawn the NPCs spawnEntities always did, names included.
    Grid w = editWorld();
    std::vector<Marker> defaults = defaultMarkers(w, 9);
    std::vector<Entity> before = spawnEntities(w, findSpawn(w), 9), after;
    syncSpawns(after, w, defaults, 9, findSpawn(w));
    CHECK(defaults.size() == 4 && after.size() == before.size());
    for (size_t i = 0; i < std::min(before.size(), after.size()); ++i)
        CHECK(after[i].kind == before[i].kind && after[i].home == before[i].home && after[i].name == before[i].name &&
              after[i].body.yaw == before[i].body.yaw && after[i].wanderRadius == before[i].type().wanderRadius);
    for (const Marker& m : defaults) CHECK(!checkMarker(w, m) && m.kind == MarkerKind::Spawn && m.count == 1);

    // Commands: one undo step each, validated, listed, drawn on the map.
    Grid cg = meadow();
    std::vector<Marker> marks;
    EditHistory ch;
    PrefabLibrary lib = loadPrefabs("no-such-folder");
    GameView view;
    auto run = [&](std::string_view request) { return runCommand(request, cg, instances, marks, ch, lib, view); };
    auto ok = [&](const Command& c) { return c.response["ok"].get<bool>(); };
    Command c = run(R"({"cmd": "mark", "args": {"kind": "poi", "name": "Old Well", "x": 10, "z": 12}})");
    CHECK(ok(c) && c.response["result"]["marker"]["id"] == 1 && c.response["result"]["marker"]["y"] == 3 && marks.size() == 1);
    CHECK(marks[0].pos == glm::vec3(10.5f, 4, 12.5f) && marks[0].radius == 16 && !c.dirty.empty() && ch.undo.size() == 1);
    c = run(R"({"cmd": "mark", "args": {"kind": "spawn", "name": "Bandit Camp", "x": 20, "z": 20, "y": 5, "entity": "bandit", "count": 4, "radius": 7}})");
    CHECK(ok(c) && marks.size() == 2 && marks[1].entity == EntityKind::Bandit && marks[1].count == 4 && marks[1].pos.y == 6);
    c = run(R"({"cmd": "mark", "args": {"kind": "region", "name": "Meadow", "x": 16, "z": 16, "radius": 30}})");
    CHECK(ok(c) && run(R"({"cmd": "mark", "args": {"kind": "player_spawn", "x": 5, "z": 5}})").response["ok"] == true);
    CHECK(run(R"({"cmd": "state"})").response["result"]["markers"] == 4);
    c = run(R"({"cmd": "edit_marker", "args": {"id": 2, "x": 24, "count": 2}})");
    CHECK(ok(c) && marks[1].pos == glm::vec3(24.5f, 4, 20.5f) && marks[1].count == 2 && marks[1].name == "Bandit Camp");
    CHECK(ok(run(R"({"cmd": "edit_marker", "args": {"id": 1, "name": "Dry Well"}})")) && marks[0].name == "Dry Well");
    CHECK(ch.undo.size() == 6);
    c = run(R"({"cmd": "markers", "args": {"kind": "spawn"}})");
    CHECK(ok(c) && c.response["result"]["count"] == 1 && c.response["result"]["markers"][0]["entity"] == "bandit");
    c = run(R"({"cmd": "markers", "args": {"x0": 0, "z0": 0, "x1": 9, "z1": 9}})");  // the well is outside, the region reaches in
    CHECK(ok(c) && c.response["result"]["count"] == 2);
    c = run(R"({"cmd": "map", "args": {"x0": 0, "z0": 0, "x1": 63, "z1": 63}})");
    CHECK(ok(c) && c.response["result"]["markers"].size() == 4);
    if (ok(c)) std::filesystem::remove(c.response["result"]["path"].get<std::string>());
    for (const char* hostile : {
             R"({"cmd": "mark", "args": {"kind": "castle", "name": "x", "x": 1, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "spawn", "x": 1, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "spawn", "x": 1, "z": 1, "entity": "dragon"}})",
             R"({"cmd": "mark", "args": {"kind": "spawn", "x": 1, "z": 1, "entity": "boar", "count": 9}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": "x", "x": 1, "z": 1, "entity": "boar"}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "x": 1, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": "x", "x": 64, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": "x", "x": 1, "z": 1, "y": 99}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": "x", "x": 1, "z": 1, "radius": 1e300}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": "x", "x": 1e400, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": "a\u0000b", "x": 1, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": "0123456789012345678901234567890123456789012345678", "x": 1, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "poi", "name": 7, "x": 1, "z": 1}})",
             R"({"cmd": "mark", "args": {"kind": "player_spawn", "x": 9, "z": 9}})",
             R"({"cmd": "edit_marker", "args": {"id": 0, "name": "x"}})",
             R"({"cmd": "edit_marker", "args": {"id": 99, "name": "x"}})",
             R"({"cmd": "edit_marker", "args": {"id": 4294967296, "name": "x"}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "name": ""}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "count": 2}})",
             R"({"cmd": "unmark", "args": {"id": 99}})",
             R"({"cmd": "markers", "args": {"kind": "boss"}})",
         }) {
        c = run(hostile);
        CHECK(!ok(c) && c.dirty.empty());
    }
    CHECK(marks.size() == 4 && ch.undo.size() == 6);
    c = run(R"({"cmd": "unmark", "args": {"id": 2}})");
    CHECK(ok(c) && marks.size() == 3 && ch.undo.size() == 7);
    CHECK(ok(run(R"({"cmd": "undo"})")) && marks.size() == 4 && marks[1].count == 2);
    CHECK(ok(run(R"({"cmd": "undo"})")) && marks[0].name == "Old Well");
    CHECK(!ok(run(R"({"cmd": "edit_marker", "args": {"id": 1.5, "name": "x"}})")) && !ok(run(R"({"cmd": "unmark", "args": {"id": 1.9}})")));
    CHECK(marks.size() == 4 && marks[0].name == "Old Well");  // whole ids only: 1.9 isn't 1

    // Review fixes. Names: no bidi overrides or zero-width characters.
    for (const char* name : {"a\xE2\x80\xAE" "b", "a\xE2\x80\x8B" "b", "a\xE2\x81\xA6" "b", "\xEF\xBB\xBF" "a", "a\xE2\x80\x8F" "b"}) {
        Marker m = poi;
        m.name = name;
        CHECK(checkMarker(g, m));
    }
    // All spawn markers together make at most kMaxSpawnedNpcs NPCs; replacing a marker's count counts it once.
    {
        std::vector<Marker> spawns;
        EditHistory sh;
        Marker den = anon;
        den.entity = EntityKind::Boar, den.count = kMaxSpawnCount;
        for (int i = 0; i < kMaxSpawnedNpcs / kMaxSpawnCount; ++i) CHECK(!putMarker(g, spawns, sh, den).error);
        CHECK(putMarker(g, spawns, sh, den).error && spawns.size() == size_t(kMaxSpawnedNpcs / kMaxSpawnCount));
        Marker again = spawns[0];
        again.pos.x = 7.5f;
        CHECK(!putMarker(g, spawns, sh, again).error);  // same count, moved: fine at the cap
        again.count = 1;
        CHECK(!putMarker(g, spawns, sh, again).error && !putMarker(g, spawns, sh, Marker{.kind = MarkerKind::Spawn, .pos = {3.5f, 4, 3.5f}, .count = 7}).error);
        std::string payload = u32(kMaxSpawnedNpcs / kMaxSpawnCount + 1);  // a file over the cap
        for (uint32_t id = 1; id <= uint32_t(kMaxSpawnedNpcs / kMaxSpawnCount + 1); ++id) payload += one(id, 1, "", {10.5f, 4, 10.5f}, 6, 2, 8);
        CHECK(seal(payload).contains("NPCs"));
        size_t last = one(1, 1, "", {}, 0, 0, 0).size();  // one marker fewer: exactly at the cap
        CHECK(seal(payload.substr(0, payload.size() - last).replace(0, 4, u32(kMaxSpawnedNpcs / kMaxSpawnCount))).empty());
    }
    std::filesystem::remove(path);
    // Spawns inside terrain: the player and NPCs come out on top instead of being stuck.
    {
        Grid t = meadow();
        for (int y = 4; y < 9; ++y) t.set({10, y, 10}, Stone);  // a pillar over the marker
        std::vector<Marker> ms = {marker(MarkerKind::PlayerSpawn, "", {10.5f, 4, 10.5f}, 0), marker(MarkerKind::Spawn, "", {10.5f, 4, 10.5f}, 3)};
        ms[0].id = 1, ms[1].id = 2;
        glm::vec3 p = playerSpawn(t, ms);
        CHECK(p.y == 9 && !overlapsSolid(t, p));
        std::vector<Entity> es;
        syncSpawns(es, t, ms, 1, p);
        CHECK(es.size() == 1 && es[0].body.pos.y == 9 && !overlapsSolid(t, es[0].body.pos));
        for (int y = 9; y < 12; ++y) t.set({10, y, 10}, Stone);  // built over since: freed on the next update
        Player far;
        far.pos = {60, 4, 60};
        updateEntities(es, t, far, 1 / 60.0f);
        CHECK(!overlapsSolid(t, es[0].body.pos));
        ms[1].name = "Renamed";
        syncSpawns(es, t, ms, 1, p);
        CHECK(es[0].spawn.name == "Renamed" && es[0].body.pos.y >= 12);  // a rename doesn't respawn
    }
    // mark's default ground is under a tree's canopy, not on it.
    cg.set({5, 4, 5}, Wood), cg.set({5, 5, 5}, Wood), cg.set({5, 6, 5}, Leaves), cg.set({5, 8, 5}, Leaves);
    c = run(R"({"cmd": "mark", "args": {"kind": "poi", "name": "Shade", "x": 5, "z": 5}})");
    CHECK(ok(c) && c.response["result"]["marker"]["y"] == 3);
    // The map lists at most 500 markers.
    for (int i = 0; i < 520; ++i) putMarker(cg, marks, ch, marker(MarkerKind::Poi, "p", {float(i % 64) + 0.5f, 4, float(i / 64) + 0.5f}, 4));
    c = run(R"({"cmd": "map", "args": {"x0": 0, "z0": 0, "x1": 63, "z1": 63}})");
    CHECK(ok(c) && marks.size() > 500 && c.response["result"]["markers"].size() == 500);
    if (ok(c)) std::filesystem::remove(c.response["result"]["path"].get<std::string>());
}

// The story NPC kinds: table sanity, models, fights against an armored player, loot.
static void storyNpcs() {
    struct Row { EntityKind kind; int level; Temper temper; bool talks; Model model; ItemId weapon, loot; };
    constexpr Row kRows[] = {
        {EntityKind::Wolf, 1, Temper::Aggressive, false, ModelWolf, NoItem, WolfPelt},
        {EntityKind::Guard, 4, Temper::Passive, true, ModelGuard, Shortsword, NoItem},
        {EntityKind::BanditChief, 3, Temper::Aggressive, false, ModelChief, Shortsword, IronHelm},
        {EntityKind::Ghoul, 3, Temper::Aggressive, false, ModelGhoul, NoItem, BoneCharm},
    };
    for (const Row& r : kRows) {
        const EntityType& t = typeOf(r.kind);
        CHECK(t.name && t.level == r.level && t.temper == r.temper && t.talks == r.talks && t.model == r.model);
        CHECK(t.weapon == r.weapon && t.loot == r.loot && t.attackable && t.wanders && t.respawnSeconds > 0);
        CHECK((t.loot == NoItem) == (t.lootChance == 0) && t.lootChance <= 1);
        if (r.temper == Temper::Aggressive) CHECK(t.aggroRange > 0 && t.aggroRange < kLeashRadius && t.chaseSpeed > 0);
    }
    CHECK(typeOf(EntityKind::Wolf).chaseSpeed > 5 && typeOf(EntityKind::Ghoul).chaseSpeed < 5);  // the player walks 5, runs 9
    CHECK(typeOf(EntityKind::BanditChief).lootChance == 1);
    CHECK(std::string_view(kEntityKindNames[int(EntityKind::BanditChief)]) == "bandit_chief");
    CHECK(itemType(WolfPelt).slot == SlotNone && itemType(BoneCharm).slot == SlotNone && std::string_view(itemType(BoneCharm).name) == "Bone Charm");
    // Models: distinct, every part has voxels, sized like the rig they reuse.
    for (Model m : {ModelWolf, ModelGuard, ModelChief, ModelGhoul})
        for (int p = 0; p < PartCount; ++p) {
            Grid part = buildPart(m, Part(p));
            CHECK(std::ranges::count(part.cells, Air) < std::ssize(part.cells));
            for (Model o : {ModelWolf, ModelGuard, ModelChief, ModelGhoul, ModelVillager, ModelBandit, ModelIronArmor, ModelBoar})
                if (o != m && p == Torso) CHECK(buildPart(o, Torso).cells != part.cells);
            if (m != ModelWolf) CHECK(part.size == buildPart(ModelPlayer, Part(p)).size);
        }
    Grid wolfHead = buildPart(ModelWolf, Head);
    CHECK(wolfHead.size.z > buildPart(ModelBoar, Head).size.z && std::ranges::count(wolfHead.cells, DarkHair) >= 4);  // snout, ears
    CHECK(buildPart(ModelWolf, HandL).size.y > buildPart(ModelBoar, HandL).size.y);  // longer legs
    CHECK(std::ranges::count(buildPart(ModelWolf, Torso).cells, DarkHair) > 0);      // tail tip
    CHECK(partModels(ModelWolf, Inventory{})[Head] == ModelWolf);
    Player chiefBody, guardBody;
    chiefBody.inv.equipped[SlotMainHand] = guardBody.inv.equipped[SlotMainHand] = Shortsword;
    glm::mat4 held(0);
    poseModel(chiefBody, ModelChief, &held);
    CHECK(held != glm::mat4(0));
    CHECK(poseModel(chiefBody, ModelChief)[Head][3].y > poseModel(guardBody, ModelGuard)[Head][3].y + 0.1f);  // bigger
    held = glm::mat4(0);
    poseModel(Player{}, ModelWolf, &held);
    CHECK(held == glm::mat4(0));  // no hands

    // Fights: a level-1 player in the whole Iron set (armor 37) with the sword stands still and
    // swings at the nearest foe; foes come from a spawn marker 6 blocks away. Returns the player's
    // health left, or minus the foes' health left when the player goes down.
    Grid g = meadow();
    auto fight = [&](EntityKind kind, int count, bool geared, uint32_t seed) {
        Player player;
        player.pos = {20.5f, 4, 32.5f};
        if (geared)
            for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, Shortsword}) {
                giveItem(player.inv, id);
                quickMove(player.inv, {SlotRef::Item, 0, 0});
            }
        CHECK(armor(player.inv) == (geared ? 37 : 0) && player.inv.equipped[SlotMainHand] == (geared ? Shortsword : NoItem));
        Marker m{.id = 1, .kind = MarkerKind::Spawn, .pos = {26.5f, 4, 32.5f}, .radius = 3, .entity = kind, .count = count};
        std::vector<Entity> es;
        syncSpawns(es, g, {m}, seed, player.pos);
        std::mt19937 rng(seed);
        for (int i = 0; i < 120 * 60; ++i) {
            updateEntities(es, g, player, 1 / 60.0f);
            separate(player, g, es);
            auto alive = es | std::views::filter([](const Entity& e) { return e.body.alive(); });
            auto flat = [&](const Entity& e) { return glm::vec2(e.body.pos.x - player.pos.x, e.body.pos.z - player.pos.z); };
            auto foe = std::ranges::min_element(alive, {}, [&](const Entity& e) { return glm::length(flat(e)); });
            if (foe == alive.end()) return player.health;
            if (glm::length(flat(*foe)) < kPunchReach - 0.1f) startPunch(player, glm::normalize(flat(*foe)));
            updateCombat(player, es, 1 / 60.0f, rng);
            if (!player.alive()) return -std::ranges::fold_left(es, 0.0f, [](float n, const Entity& e) { return n + e.body.health; });
        }
        return 999.0f;  // nobody won in two minutes
    };
    // Twelve seeds: every fight goes the same way; health left varies a little per seed, so its mean
    // is what's tuned.
    constexpr int kSeeds = 12;
    float chiefSum = 0, wolvesSum = 0, ghoulsSum = 0;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
        float chief = fight(EntityKind::BanditChief, 1, true, seed), wolves = fight(EntityKind::Wolf, 2, true, seed);
        float ghouls = fight(EntityKind::Ghoul, 2, true, seed), bare = fight(EntityKind::BanditChief, 1, false, seed);
        if (!(chief > 0 && chief < 50 && wolves > 0 && wolves < 60 && ghouls > 0 && ghouls < 60 && bare < 0))
            std::println(stderr, "seed {}: chief {} wolves {} ghouls {} bare-handed {}", seed, chief, wolves, ghouls, bare);
        CHECK(chief > 0 && chief < 50);    // the chief alone: won, never easily
        CHECK(wolves > 0 && wolves < 60);  // a pair of wolves
        CHECK(ghouls > 0 && ghouls < 60);  // a pair of ghouls
        CHECK(bare < 0);                   // bare-handed and unarmored, the chief wins
        chiefSum += chief, wolvesSum += wolves, ghoulsSum += ghouls;
    }
    float chiefMean = chiefSum / kSeeds, wolvesMean = wolvesSum / kSeeds, ghoulsMean = ghoulsSum / kSeeds;
    std::println("story fights, health left on average: chief {:.0f}, wolves {:.0f}, ghouls {:.0f}", chiefMean, wolvesMean, ghoulsMean);
    CHECK(chiefMean >= 20 && chiefMean <= 30);                           // about a quarter
    CHECK(wolvesMean >= 25 && wolvesMean <= 45 && ghoulsMean >= 20 && ghoulsMean <= 40);  // a quarter to two fifths
    CHECK(fight(EntityKind::Wolf, 3, true, 1) < 0);  // a pack of three is too much alone
    // Their hits on that player: punch or sword + strength x speed, less armor / (armor + 40 x level).
    std::mt19937 rng(3);
    Player armored;
    for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) armored.inv.equipped[itemType(id).slot] = id;
    for (auto [kind, lo, hi] : {std::tuple{EntityKind::Wolf, 2, 3}, std::tuple{EntityKind::Ghoul, 2, 5}, std::tuple{EntityKind::BanditChief, 5, 8}}) {
        const EntityType& t = typeOf(kind);
        int least = 99, most = 0;
        for (int i = 0; i < 500; ++i) {
            int d = mitigate(rollDamage(t.stats, weaponAttack(t.weapon), rng), armor(armored.inv), t.level);
            least = std::min(least, d), most = std::max(most, d);
        }
        CHECK(least == lo && most == hi);
    }

    // The chief of a story camp: named by its marker, taunts when it turns on the player, always drops the helm.
    Marker camp{.id = 3, .kind = MarkerKind::Spawn, .name = "Hakon the Red", .pos = {12.5f, 4, 12.5f}, .radius = 6,
                .entity = EntityKind::BanditChief, .count = 1, .lines = {"You walked into the wrong camp.", "Is that all?"}};
    CHECK(!checkMarker(g, camp));
    std::vector<Entity> es;
    syncSpawns(es, g, {camp}, 1, {20, 4, 12});
    Entity& hakon = es[0];
    CHECK(hakon.displayName() == "Hakon the Red" && hakon.speech() == nullptr);
    Player player;
    player.pos = hakon.body.pos + glm::vec3(5, 0, 0);
    updateEntities(es, g, player, 1 / 60.0f);
    CHECK(hakon.angry && hakon.speech() && std::string_view(hakon.speech()) == "You walked into the wrong camp.");
    CHECK(damage(hakon, 10000) && hakon.loot == IronHelm);
    hakon.speechTimer = 0;
    for (int i = 0; i < 3; ++i) updateEntities(es, g, player, 1 / 60.0f);
    CHECK(hakon.speech() == nullptr);  // the dead don't taunt

    // Guards: talk while calm (E), shout their next line when attacked, never talk once dead.
    Marker post{.id = 4, .kind = MarkerKind::Spawn, .name = "Gate", .pos = {30.5f, 4, 30.5f}, .radius = 4,
                .entity = EntityKind::Guard, .count = 1, .lines = {"Halt.", "Have at you!", "Move along."}};
    std::vector<Entity> gs;
    syncSpawns(gs, g, {post}, 1, {34, 4, 30});
    Entity& guard = gs[0];
    player.pos = guard.body.pos + glm::vec3(2, 0, 0);
    CHECK(talkTarget(gs, player) == &guard);
    talk(guard);
    CHECK(std::string_view(guard.speech()) == "Halt.");
    CHECK(damage(guard, 5) && guard.angry && std::string_view(guard.speech()) == "Have at you!");
    // Hit from outside its territory it can't stay angry: one shout, not one per hit, and it doesn't
    // stand frozen under the balloon.
    Player far;
    far.pos = guard.home + glm::vec3(kLeashRadius + 2, 0, 0);
    guard.angry = false, guard.shoutTimer = 0, guard.speechTimer = 0;
    int shouts = 0;
    for (int i = 0; i < 4 * 60; ++i) {  // within the balloon's 5 s
        int before = guard.phrase;
        if (i % 30 == 0) damage(guard, 1);
        float waited = guard.wanderTimer;
        updateEntities(gs, g, far, 1 / 60.0f);
        shouts += guard.phrase != before;
        CHECK(!guard.angry && (guard.wanderTimer < waited || guard.wanderTimer > waited + 1));  // it keeps wandering
    }
    CHECK(shouts == 1 && guard.speech());
    guard.body.health = 0;
    CHECK(talkTarget(gs, player) == nullptr);  // the dead don't talk (E loots instead)
    player.pos = hakon.body.pos + glm::vec3(1, 0, 0);
    CHECK(lootTarget(es, player) == &hakon && takeLoot(hakon, player.inv) && player.inv.containers[0][0] == IronHelm);
}

// Spawn marker lines: validation, talking, editing without respawning, undo, files, commands.
static void markerLines() {
    Grid g = meadow();
    Marker town{.kind = MarkerKind::Spawn, .name = "Maren Holt", .pos = {10.5f, 4, 10.5f}, .radius = 4,
                .entity = EntityKind::Villager, .count = 1, .lines = {"Welcome to the ford.", "Mind the wolves.", "Gr\xC3\xBC\xC3\x9F dich!"}};
    CHECK(!checkMarker(g, town));
    Marker m = town;
    m.lines.assign(kMaxMarkerLines, std::string(kMaxMarkerLine, 'a'));
    CHECK(!checkMarker(g, m));  // 8 lines of 160 bytes is the cap
    for (auto bad : std::initializer_list<std::vector<std::string>>{
             std::vector<std::string>(kMaxMarkerLines + 1, "a"), {std::string(kMaxMarkerLine + 1, 'a')}, {""}, {"ok", ""},
             {"\xC3"}, {"\xC0\x80"}, {"a\nb"}, {"a\x7F"}, {"a\xE2\x80\xAE" "b"}, {"a\xE2\x80\x8B" "b"}, {"\xEF\xBB\xBF" "a"}}) {
        m = town;
        m.lines = bad;
        CHECK(checkMarker(g, m));
    }
    m = town, m.entity = EntityKind::TrainingDummy;  // the dummy neither talks nor fights
    CHECK(checkMarker(g, m));
    m = town, m.kind = MarkerKind::Poi;
    CHECK(checkMarker(g, m));
    for (EntityKind k : {EntityKind::Guard, EntityKind::Boar, EntityKind::Wolf, EntityKind::BanditChief, EntityKind::Ghoul}) {
        m = town, m.entity = k;
        CHECK(!checkMarker(g, m));
    }

    // Talking: the lines in order and round again; the lone NPC takes the marker's name.
    std::vector<Marker> list;
    EditHistory h;
    std::vector<Instance> none;
    std::vector<Entity> npcs;
    auto sync = [&] { syncSpawns(npcs, g, list, 1, {16, 4, 16}); };
    CHECK(!putMarker(g, list, h, town).error);
    sync();
    CHECK(npcs.size() == 1 && npcs[0].displayName() == "Maren Holt" && npcs[0].name != "Maren Holt");
    std::vector<std::string> said;
    for (int i = 0; i < 4; ++i) {
        talk(npcs[0]);
        said.push_back(npcs[0].speech());
    }
    CHECK(said == std::vector<std::string>({town.lines[0], town.lines[1], town.lines[2], town.lines[0]}));
    Player player;
    player.pos = npcs[0].body.pos + glm::vec3(1.5f, 0, 0);
    CHECK(talkTarget(npcs, player) == &npcs[0]);

    // A guard: edit its lines and name; it isn't respawned (it keeps its damage) and starts over.
    Marker guard = town;
    guard.name = "Gate";
    guard.entity = EntityKind::Guard;
    guard.pos = {20.5f, 4, 10.5f};
    guard.lines = {"Halt."};
    EditResult r = putMarker(g, list, h, guard);
    CHECK(!r.error && r.id == 2);
    sync();
    Entity* g1 = &npcs[1];
    CHECK(g1->kind == EntityKind::Guard && g1->displayName() == "Gate" && damage(*g1, 7) && g1->body.health == 153);
    g1->angry = false;
    talk(*g1);
    CHECK(std::string_view(g1->speech()) == "Halt.");
    guard = list[1];
    guard.name = "Sergeant Brask";
    guard.lines = {"Move along.", "The road east is closed."};
    size_t steps = h.undo.size();
    CHECK(!putMarker(g, list, h, guard).error && h.undo.size() == steps + 1);  // one undo step
    sync();
    g1 = &npcs[1];
    CHECK(npcs.size() == 2 && g1->body.health == 153 && g1->displayName() == "Sergeant Brask" && g1->speech() == nullptr);
    talk(*g1);
    CHECK(std::string_view(g1->speech()) == "Move along.");
    CHECK(!undoEdit(g, h, none, list).empty() && list[1].lines == std::vector<std::string>{"Halt."});
    sync();
    g1 = &npcs[1];
    CHECK(npcs.size() == 2 && g1->body.health == 153 && g1->displayName() == "Gate");
    talk(*g1);
    CHECK(std::string_view(g1->speech()) == "Halt.");
    CHECK(!redoEdit(g, h, none, list).empty());
    sync();
    CHECK(npcs[1].body.health == 153 && npcs[1].displayName() == "Sergeant Brask");
    // Two of them keep their own names; without lines they say random phrases again.
    guard = list[1];
    guard.count = 2;
    CHECK(!putMarker(g, list, h, guard).error);
    sync();
    CHECK(npcs.size() == 3 && npcs[1].displayName() == "Guard" && npcs[2].displayName() == "Guard");
    Marker plain = list[0];
    plain.lines.clear();
    CHECK(!putMarker(g, list, h, plain).error);
    sync();
    CHECK(npcs[0].displayName() == npcs[0].name);
    talk(npcs[0]);
    CHECK(npcs[0].speech() && std::ranges::find(town.lines, std::string(npcs[0].speech())) == town.lines.end());
    undoEdit(g, h, none, list);
    sync();

    // Files: lines round-trip in their own section; files without it load without lines; hostile ones fail.
    std::string path = (std::filesystem::temp_directory_path() / "flycoos_lines_test.flyw").string();
    std::optional<std::vector<Marker>> loaded;
    std::vector<Instance> instances;
    uint32_t seed = 0;
    Grid fg({64, 32, 64});
    fg.set({1, 2, 3}, Stone);
    CHECK(saveWorld(path.c_str(), fg, 5, {}, list).empty());
    Grid back({32, 32, 32});
    CHECK(loadWorld(path.c_str(), back, seed, instances, loaded).empty() && loaded && *loaded == list && !(*loaded)[0].lines.empty());
    std::ifstream in(path, std::ios::binary);
    const std::string file((std::istreambuf_iterator<char>(in)), {});
    in.close();
    CHECK(file.contains("LINE"));
    size_t at = file.rfind("LINE");
    auto u32 = [](uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
    auto seal = [&](std::string head, std::string payload) {  // the file up to LINE, then `payload` as LINE (none: no section)
        std::string bytes = head;
        if (!payload.empty()) {
            bytes += "LINE";
            for (int i = 0; i < 8; ++i) bytes += char(uint64_t(payload.size()) >> (8 * i));
            bytes += payload;
        }
        uint32_t hash = 2166136261u;
        for (char c : bytes) hash = (hash ^ uint8_t(c)) * 16777619u;
        for (int i = 0; i < 4; ++i) bytes += char(hash >> (8 * i));
        std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
        return loadWorld(path.c_str(), back, seed, instances, loaded);
    };
    std::string head = file.substr(0, at);
    CHECK(seal(head, "").empty() && loaded && loaded->size() == list.size() &&
          std::ranges::all_of(*loaded, [](const Marker& k) { return k.lines.empty(); }));  // an older file
    auto lines = [&](uint32_t id, std::vector<std::string> ls) {
        std::string out = u32(id) + char(ls.size());
        for (const std::string& l : ls) out += char(l.size()) + l;
        return out;
    };
    CHECK(seal(head, u32(1) + lines(2, {"Halt."})).empty() && (*loaded)[1].lines == std::vector<std::string>{"Halt."});
    std::string noMarks = file.substr(0, file.rfind("MARK"));  // no MARK section before it
    CHECK(seal(noMarks, u32(1) + lines(1, {"Hi"})).contains("without markers"));
    for (const std::string& hostile : {
             u32(1) + lines(9, {"x"}),                                      // no such marker
             u32(2) + lines(2, {"x"}) + lines(1, {"y"}),                    // ids not ascending
             u32(2) + lines(1, {"x"}) + lines(1, {"y"}),                    // twice
             u32(3) + lines(1, {"x"}) + lines(2, {"y"}) + lines(3, {"z"}),  // more than the markers
             u32(1) + lines(1, {}),                                         // no lines
             u32(1) + lines(1, std::vector<std::string>(9, "x")),           // too many
             u32(1) + lines(1, {""}),                                       // an empty line
             u32(1) + lines(1, {std::string(161, 'x')}),                    // too long
             u32(1) + lines(1, {"a\xE2\x80\xAE" "b"}),                      // bidi override
             u32(1) + lines(1, {"\xC3\x28"}),                               // invalid UTF-8
             u32(2) + lines(1, {"x"}),                                      // truncated
             u32(1) + lines(1, {"x"}) + "z",                                // trailing bytes
             u32(1) + lines(1, {"xyz"}).substr(0, 7),                       // cut inside a line
         }) {
        std::string error = seal(head, hostile);
        CHECK(!error.empty() && (error.contains("lines") || error.contains("truncated")));
    }
    {  // two LINE sections: rejected, not one overwriting the other
        std::string first = u32(1) + lines(1, {"Hi"}), section = "LINE";
        for (int i = 0; i < 8; ++i) section += char(uint64_t(first.size()) >> (8 * i));
        CHECK(seal(head + section + first, u32(1) + lines(2, {"Ho"})).contains("duplicate"));
    }
    // A dummy's marker can't carry lines, even from a file.
    std::vector<Marker> dummies = {Marker{.id = 1, .kind = MarkerKind::Spawn, .pos = {5.5f, 4, 5.5f}, .entity = EntityKind::TrainingDummy}};
    CHECK(saveWorld(path.c_str(), fg, 5, {}, dummies).empty());
    std::ifstream in2(path, std::ios::binary);
    const std::string dummyFile((std::istreambuf_iterator<char>(in2)), {});
    in2.close();
    CHECK(!dummyFile.contains("LINE") && seal(dummyFile.substr(0, dummyFile.size() - 4), u32(1) + lines(1, {"Hi"})).contains("lines"));
    std::filesystem::remove(path);

    // Commands: mark and edit_marker take lines, markers lists them, hostile ones change nothing.
    std::vector<Marker> marks;
    EditHistory ch;
    PrefabLibrary lib = loadPrefabs("no-such-folder");
    GameView view;
    auto run = [&](std::string_view request) { return runCommand(request, g, instances, marks, ch, lib, view); };
    auto ok = [&](const Command& c) { return c.response["ok"].get<bool>(); };
    Command c = run(R"({"cmd": "mark", "args": {"kind": "spawn", "name": "Torvald", "x": 10, "z": 10, "entity": "villager", "lines": ["My iron is gone.", "Bandits took it."]}})");
    CHECK(ok(c) && marks.size() == 1 && marks[0].lines.size() == 2 && c.response["result"]["marker"]["lines"][1] == "Bandits took it.");
    c = run(R"({"cmd": "markers", "args": {}})");
    CHECK(ok(c) && c.response["result"]["markers"][0]["lines"].size() == 2);
    CHECK(ok(run(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": ["Thank you."]}})")) && marks[0].lines == std::vector<std::string>{"Thank you."});
    CHECK(ok(run(R"({"cmd": "mark", "args": {"kind": "spawn", "x": 20, "z": 10, "entity": "wolf", "count": 3}})")));
    CHECK(!run(R"({"cmd": "markers", "args": {"kind": "spawn"}})").response["result"]["markers"][1].contains("lines"));
    CHECK(ch.undo.size() == 3);
    std::string tooLong = std::format(R"({{"cmd": "edit_marker", "args": {{"id": 1, "lines": ["{}"]}}}})", std::string(161, 'x'));
    for (const std::string& hostile : {
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": "Hello"}})"),
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": [1]}})"),
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": [["a"]]}})"),
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": ["1","2","3","4","5","6","7","8","9"]}})"),
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": [""]}})"),
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": ["a‮b"]}})"),
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": ["a\u0007b"]}})"),
             std::string(R"({"cmd": "edit_marker", "args": {"id": 1, "entity": "dummy"}})"),  // it has lines
             std::string(R"({"cmd": "mark", "args": {"kind": "poi", "name": "Well", "x": 5, "z": 5, "lines": ["Hi"]}})"),
             std::string(R"({"cmd": "mark", "args": {"kind": "spawn", "x": 5, "z": 5, "entity": "dummy", "lines": ["Hi"]}})"),
             tooLong,
         }) {
        c = run(hostile);
        CHECK(!ok(c) && c.dirty.empty());
    }
    CHECK(marks.size() == 2 && ch.undo.size() == 3 && marks[0].lines == std::vector<std::string>{"Thank you."});
    CHECK(ok(run(R"({"cmd": "edit_marker", "args": {"id": 1, "lines": [], "entity": "dummy"}})")) && marks[0].lines.empty());
    CHECK(ok(run(R"({"cmd": "undo"})")) && marks[0].lines.size() == 1 && marks[0].entity == EntityKind::Villager);
}

static void raycasting() {
    Grid g = flatWorld(3);  // ground top y = 3, wall at x = 10 up to y = 6
    auto hit = raycast(g, {8.5f, 10, 8.5f}, {0, -1, 0}, 100);
    CHECK(hit && hit->cell == glm::ivec3(8, 3, 8) && hit->normal == glm::ivec3(0, 1, 0) && std::abs(hit->distance - 6) < 1e-4f);
    hit = raycast(g, {2.5f, 5.5f, 8.5f}, {1, 0, 0}, 100);
    CHECK(hit && hit->cell == glm::ivec3(10, 5, 8) && hit->normal == glm::ivec3(-1, 0, 0) && std::abs(hit->distance - 7.5f) < 1e-4f);
    hit = raycast(g, {14.5f, 5.5f, 8.5f}, {-1, 0, 0}, 100);
    CHECK(hit && hit->cell == glm::ivec3(10, 5, 8) && hit->normal == glm::ivec3(1, 0, 0));
    CHECK(!raycast(g, {2.5f, 5.5f, 8.5f}, {1, 0, 0}, 7.4f));  // out of range
    CHECK(!raycast(g, {8.5f, 10, 8.5f}, {0, 1, 0}, 100));     // sky
    hit = raycast(g, {8.5f, 2.5f, 8.5f}, {0, 1, 0}, 100);     // starts inside the ground
    CHECK(hit && hit->cell == glm::ivec3(8, 2, 8) && hit->normal == glm::ivec3(0) && hit->distance == 0);
    Grid m = meadow();  // water is passed through
    hit = raycast(m, {40.5f, 8, 40.5f}, {0, -1, 0}, 100);
    CHECK(hit && hit->cell == glm::ivec3(40, 2, 40) && hit->normal == glm::ivec3(0, 1, 0));

    // Random rays agree with a fine-stepped march, and land on the face they report.
    Grid w = editWorld();
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(0, 1);
    int hits = 0;
    for (int i = 0; i < 150; ++i) {
        glm::vec3 from{u(rng) * 64, 60 + u(rng) * 30, u(rng) * 64};
        if (isOpaque(w.get(glm::ivec3(glm::floor(from))))) continue;
        glm::vec3 dir = glm::normalize(glm::vec3(u(rng) - 0.5f, -u(rng), u(rng) - 0.5f));
        auto r = raycast(w, from, dir, 120);
        float t = 0;
        while (t < 120 && !isOpaque(w.get(glm::ivec3(glm::floor(from + dir * t))))) t += 0.002f;
        CHECK(bool(r) == (t < 120));
        if (!r) continue;
        ++hits;
        CHECK(r->cell == glm::ivec3(glm::floor(from + dir * t)) || std::abs(r->distance - t) < 0.01f);  // grazing an edge
        glm::vec3 p = from + dir * r->distance;
        int a = r->normal.x ? 0 : r->normal.y ? 1 : 2;
        CHECK(std::abs(p[a] - float(r->cell[a] + (r->normal[a] > 0))) < 2e-3f);
        CHECK(!isOpaque(w.get(r->cell + r->normal)));
    }
    CHECK(hits > 50);
}

// One field of a pinned table row: on a mismatch says which row and field, and both values.
template <class T>
static void same(std::string_view row, std::string_view field, const T& got, const T& want) {
    if (got == want) return;
    if constexpr (std::is_enum_v<T>) std::println(stderr, "FAIL {}.{}: got {}, want {}", row, field, int(got), int(want));
    else std::println(stderr, "FAIL {}.{}: got {}, want {}", row, field, got, want);
    ++failures;
}
static std::string_view str(const char* s) { return s ? s : "<null>"; }

// Characterization: every field of the entity and item tables, and the ids saved in world files and
// inventories. Moving the tables to data files must keep every value.
static void tables() {
    // Saved as u8 in spawn markers and inventories: append-only, never renumbered.
    same("EntityKind", "Count", int(EntityKind::Count), 8);
    for (auto [kind, id, name] : {std::tuple{EntityKind::TrainingDummy, 0, "dummy"}, {EntityKind::Villager, 1, "villager"},
                                  {EntityKind::Boar, 2, "boar"}, {EntityKind::Bandit, 3, "bandit"}, {EntityKind::Wolf, 4, "wolf"},
                                  {EntityKind::Guard, 5, "guard"}, {EntityKind::BanditChief, 6, "bandit_chief"},
                                  {EntityKind::Ghoul, 7, "ghoul"}}) {
        same(name, "id", int(kind), id);
        same(name, "kEntityKindNames", std::string_view(kEntityKindNames[id]), std::string_view(name));
    }
    same("ItemId", "ItemCount", int(ItemCount), 12);
    for (auto [item, id] : {std::pair{NoItem, 0}, {Shortsword, 1}, {Satchel, 2}, {Knife, 3}, {BoarSkin, 4}, {IronHelm, 5},
                            {IronCuirass, 6}, {IronGauntlets, 7}, {IronGreaves, 8}, {IronSabatons, 9}, {WolfPelt, 10},
                            {BoneCharm, 11}})
        same(itemType(item).name, "id", int(item), id);
    same("EquipSlot", "EquipCount", int(EquipCount), 7);
    for (auto [slot, id, name] : {std::tuple{SlotHead, 0, "Head"}, {SlotChest, 1, "Chest"}, {SlotHands, 2, "Hands"},
                                  {SlotLegs, 3, "Legs"}, {SlotFeet, 4, "Feet"}, {SlotMainHand, 5, "Main Hand"},
                                  {SlotOffHand, 6, "Off Hand"}}) {
        same(name, "id", int(slot), id);
        same(name, "kSlotNames", std::string_view(kSlotNames[id]), std::string_view(name));
    }
    same("SlotBag", "id", int(SlotBag), 7);
    same("SlotNone", "id", int(SlotNone), 8);

    struct WantType {
        const char* name; int level; Stats stats; bool attackable, wanders, talks; Model model; Temper temper;
        float aggroRange, wanderRadius, chaseSpeed; ItemId weapon, loot; float lootChance, respawnSeconds;
    };
    constexpr WantType kWantTypes[] = {
        // name            lvl  sta str agi int spi   attackable wanders talks model
        //                 temper              aggro wander chase weapon      loot       chance respawn
        {"Training Dummy", 1, {10, 0, 0, 0, 10}, true,  false, false, ModelDummy,
                           Temper::Harmless,   0,    0,     0,    NoItem,     NoItem,    0,     5},
        {nullptr,          5, {25, 5, 5, 5, 5},  false, true,  true,  ModelVillager,
                           Temper::Harmless,   0,    12,    0,    NoItem,     NoItem,    0,     30},
        {"Boar",           1, {10, 2, 2, 2, 2},  true,  true,  false, ModelBoar,
                           Temper::Passive,    0,    10,    6,    NoItem,     BoarSkin,  0.75f, 30},
        {"Bandit",         2, {12, 3, 2, 2, 2},  true,  true,  false, ModelBandit,
                           Temper::Aggressive, 10,   5,     5.5f, Knife,      Knife,     0.4f,  30},
        {"Wolf",           1, {8, 2, 4, 2, 2},   true,  true,  false, ModelWolf,
                           Temper::Aggressive, 12,   10,    7.5f, NoItem,     WolfPelt,  0.6f,  30},
        {"Guard",          4, {16, 5, 3, 2, 4},  true,  true,  true,  ModelGuard,
                           Temper::Passive,    0,    6,     6,    Shortsword, NoItem,    0,     60},
        {"Bandit Chief",   3, {11, 1, 2, 2, 2},  true,  true,  false, ModelChief,
                           Temper::Aggressive, 10,   6,     5.5f, Shortsword, IronHelm,  1,     120},
        {"Barrow Ghoul",   3, {7, 2, 2, 2, 2},   true,  true,  false, ModelGhoul,
                           Temper::Aggressive, 8,    4,     4,    NoItem,     BoneCharm, 0.3f,  45},
    };
    static_assert(std::size(kWantTypes) == size_t(EntityKind::Count));
    for (int k = 0; k < int(EntityKind::Count); ++k) {
        const EntityType& t = typeOf(EntityKind(k));
        const WantType& w = kWantTypes[k];
        std::string_view row = kEntityKindNames[k];
#define SAME(field) same(row, #field, t.field, w.field)
        same(row, "name", str(t.name), str(w.name));
        SAME(level), SAME(stats.stamina), SAME(stats.strength), SAME(stats.agility), SAME(stats.intellect), SAME(stats.spirit);
        SAME(attackable), SAME(wanders), SAME(talks), SAME(model), SAME(temper);
        SAME(aggroRange), SAME(wanderRadius), SAME(chaseSpeed), SAME(weapon), SAME(loot), SAME(lootChance), SAME(respawnSeconds);
#undef SAME
    }

    struct WantItem { const char* name; EquipSlot slot; Attack attack; int bagSlots, armor; };
    constexpr WantItem kWantItems[] = {
        // name             slot          attack         bag slots  armor
        {"",                SlotNone,     {0, 0, 0},     0,         0},
        {"Iron Shortsword", SlotMainHand, {6, 10, 1.0f}, 0,         0},
        {"Canvas Satchel",  SlotBag,      {0, 0, 0},     6,         0},
        {"Rusty Knife",     SlotMainHand, {3, 6, 0.75f}, 0,         0},
        {"Boar Skin",       SlotNone,     {0, 0, 0},     0,         0},
        {"Iron Helm",       SlotHead,     {0, 0, 0},     0,         6},
        {"Iron Cuirass",    SlotChest,    {0, 0, 0},     0,         12},
        {"Iron Gauntlets",  SlotHands,    {0, 0, 0},     0,         4},
        {"Iron Greaves",    SlotLegs,     {0, 0, 0},     0,         10},
        {"Iron Sabatons",   SlotFeet,     {0, 0, 0},     0,         5},
        {"Wolf Pelt",       SlotNone,     {0, 0, 0},     0,         0},
        {"Bone Charm",      SlotNone,     {0, 0, 0},     0,         0},
    };
    static_assert(std::size(kWantItems) == ItemCount);
    for (int i = 0; i < ItemCount; ++i) {
        const ItemType& t = itemType(ItemId(i));
        const WantItem& w = kWantItems[i];
        std::string row = std::format("item {} ({})", i, w.name);
#define SAME(field) same(row, #field, t.field, w.field)
        same(row, "name", str(t.name), str(w.name));
        SAME(slot), SAME(attack.minDamage), SAME(attack.maxDamage), SAME(attack.speed), SAME(bagSlots), SAME(armor);
#undef SAME
    }
    same("kPunch", "attack", std::tuple{kPunch.minDamage, kPunch.maxDamage, kPunch.speed}, std::tuple{1, 4, 1.0f});
    same("itemType", "out of range", str(itemType(ItemId(200)).name), str(itemType(NoItem).name));
    CHECK(&itemType(ItemId(200)) == &itemType(NoItem));
    same("inventory", "kBackpackSlots", kBackpackSlots, 12);
    same("inventory", "kBagBarSlots", kBagBarSlots, 4);
    same("stats", "kArmorK", kArmorK, 40.0f);
    same("stats", "kMaxArmorReduction", kMaxArmorReduction, 0.8f);
    same("stats", "kHealthPerStamina", kHealthPerStamina, 10);
    same("stats", "kHastePerAgility", kHastePerAgility, 0.01f);
    same("stats", "kRegenPerSpirit", kRegenPerSpirit, 0.5f);
    same("stats", "kCombatSeconds", kCombatSeconds, 5.0f);
    Stats s;
    same("Stats{}", "attributes", std::tuple{s.stamina, s.strength, s.agility, s.intellect, s.spirit}, std::tuple{10, 2, 2, 2, 2});
}

// Characterization: a seeded fight replayed frame by frame at a fixed 60 fps. A level-1 player in the
// whole Iron set with the shortsword stands and swings at a bandit that spawns 6 blocks away and
// charges. Every hit (frame, who was hit, how hard), the bandit's death frame, its loot and the
// player's health are pinned: any change to combat, stats, AI, physics or the RNG draws shows here.
// The numbers also depend on libc++'s std::uniform_*_distribution (the project only builds on macOS):
// another standard library draws different rolls from the same mt19937 stream.
static void fightReplay() {
    Grid g = meadow();
    Player player;
    player.pos = {20.5f, 4, 32.5f};
    for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, Shortsword}) {
        giveItem(player.inv, id);
        quickMove(player.inv, {SlotRef::Item, 0, 0});
    }
    CHECK(armor(player.inv) == 37 && player.inv.equipped[SlotMainHand] == Shortsword);
    Marker m{.id = 1, .kind = MarkerKind::Spawn, .pos = {26.5f, 4, 32.5f}, .radius = 3, .entity = EntityKind::Bandit};
    std::vector<Entity> es;
    syncSpawns(es, g, {m}, 1, player.pos);
    CHECK(es.size() == 1);
    Entity& bandit = es[0];
    std::mt19937 rng(1);
    std::vector<std::tuple<int, char, int>> hits;  // frame, 'p'layer or 'b'andit hit, damage
    int deathFrame = -1;
    for (int frame = 0; frame < 60 * 60 && deathFrame < 0 && player.alive(); ++frame) {
        float playerHealth = player.health, banditHealth = bandit.body.health;
        updateEntities(es, g, player, 1 / 60.0f);
        separate(player, g, es);
        glm::vec2 toBandit{bandit.body.pos.x - player.pos.x, bandit.body.pos.z - player.pos.z};
        if (glm::length(toBandit) < kPunchReach - 0.1f) startPunch(player, glm::normalize(toBandit));
        updateCombat(player, es, 1 / 60.0f, rng);
        if (player.health < playerHealth) hits.emplace_back(frame, 'p', player.lastDamage);
        if (bandit.body.health < banditHealth) hits.emplace_back(frame, 'b', bandit.body.lastDamage);
        if (!bandit.body.alive()) deathFrame = frame;
    }
    // The bandit hits for 3-5 (knife 3-6 + round(3 x 0.75) strength = 5-8, less 37 armor vs level 2: 32%),
    // the player for 8-12 (sword 6-10 + 2 strength); the bandit has 120 health.
    const std::vector<std::tuple<int, char, int>> want{
        {57, 'b', 11}, {60, 'p', 3},   {104, 'p', 3}, {115, 'b', 9},  {148, 'p', 5}, {174, 'b', 8},  {192, 'p', 3},
        {233, 'b', 9}, {236, 'p', 3},  {280, 'p', 5}, {292, 'b', 12}, {324, 'p', 5}, {351, 'b', 9},  {368, 'p', 5},
        {409, 'b', 12}, {412, 'p', 5}, {457, 'p', 4}, {468, 'b', 10}, {501, 'p', 3}, {527, 'b', 11}, {545, 'p', 3},
        {586, 'b', 10}, {589, 'p', 3}, {633, 'p', 4}, {645, 'b', 10}, {677, 'p', 3}, {704, 'b', 9},
    };
    CHECK(hits == want);
    if (hits != want)
        for (auto [frame, who, amount] : hits) std::println(stderr, "  got {{{}, '{}', {}}}", frame, who, amount);
    same("fightReplay", "deathFrame", deathFrame, 704);
    if (std::abs(player.health - 43) >= 0.01f) same("fightReplay", "player.health", player.health, 43.0f);
    same("fightReplay", "bandit.loot", bandit.loot, NoItem);  // the 40% knife roll fails for this seed
    CHECK(!takeLoot(bandit, player.inv) && freeSlots(player.inv) == kBackpackSlots);

    // The bandit chief always drops his iron helm, whatever the seed.
    for (uint32_t seed : {1u, 2u, 3u}) {
        std::vector<Entity> chief;
        syncSpawns(chief, g, {{.id = 1, .kind = MarkerKind::Spawn, .pos = {26.5f, 4, 32.5f}, .entity = EntityKind::BanditChief}}, seed, player.pos);
        CHECK(chief.size() == 1 && damage(chief[0], 1000) && !chief[0].body.alive());
        same(std::format("chief seed {}", seed), "loot", chief[0].loot, IronHelm);
    }
}

// Characterization of the passive temper and the leash, before the AI becomes a state machine. A
// bare-handed player stands by a boar (seed 2, fixed 60 fps): it ignores them until punched once, then
// chases and fights back; the player doesn't swing again. Then the player steps beyond the leash: the
// boar calms down and walks home.
static void boarReplay() {
    Grid g = meadow();
    Player player;
    player.pos = {14.5f, 4, 20.5f};
    Marker m{.id = 1, .kind = MarkerKind::Spawn, .pos = {10.5f, 4, 20.5f}, .entity = EntityKind::Boar};
    std::vector<Entity> es;
    syncSpawns(es, g, {m}, 2, player.pos);
    CHECK(es.size() == 1);
    Entity& boar = es[0];
    std::mt19937 rng(2);
    std::vector<std::tuple<int, char, int>> hits;  // frame, 'p'layer or 'b'oar hit, damage
    int frame = 0;
    auto run = [&](int frames, bool punch) {
        for (int end = frame + frames; frame < end; ++frame) {
            float playerHealth = player.health, boarHealth = boar.body.health;
            updateEntities(es, g, player, 1 / 60.0f);
            separate(player, g, es);
            if (punch) startPunch(player, glm::normalize(glm::vec2(boar.body.pos.x - player.pos.x, boar.body.pos.z - player.pos.z)));
            punch = false;
            updateCombat(player, es, 1 / 60.0f, rng);
            if (player.health < playerHealth) hits.emplace_back(frame, 'p', player.lastDamage);
            if (boar.body.health < boarHealth) hits.emplace_back(frame, 'b', boar.body.lastDamage);
        }
    };
    run(120, false);  // two seconds side by side: no fight
    CHECK(hits.empty() && !boar.angry);
    player.pos = boar.body.pos + glm::vec3(1.2f, 0, 0);
    run(60, true);  // one punch
    CHECK(boar.angry);
    run(5 * 60, false);  // it fights back
    CHECK(boar.angry);
    same("boarReplay", "boar health", boar.body.health, 97.0f);  // one punch, no regen in combat
    same("boarReplay", "player health", player.health, 71.0f);
    player.pos = boar.home + glm::vec3(0, 0, 15);  // backs off, still in its territory
    run(3 * 60, false);  // it runs after them and keeps biting
    CHECK(boar.angry);
    auto near = [](float got, float want) { return std::abs(got - want) < 0.01f; };
    if (!near(glm::distance(boar.body.pos, boar.home), 13.75f))
        same("boarReplay", "chase distance from home", glm::distance(boar.body.pos, boar.home), 13.75f);
    player.pos = boar.home + glm::vec3(0, 0, kLeashRadius + 5);  // out of its territory: calms at once
    run(1, false);
    CHECK(!boar.angry);
    run(10 * 60, false);  // walks back home, both heal out of combat
    CHECK(!boar.angry);
    if (!near(glm::distance(boar.body.pos, boar.home), 0.067f))
        same("boarReplay", "distance from home", glm::distance(boar.body.pos, boar.home), 0.067f);
    same("boarReplay", "boar health", boar.body.health, 100.0f);
    if (!near(player.health, 70.62f)) same("boarReplay", "player health", player.health, 70.62f);
    const std::vector<std::tuple<int, char, int>> want{
        {128, 'b', 3}, {137, 'p', 6}, {195, 'p', 4}, {254, 'p', 3}, {313, 'p', 5}, {372, 'p', 6}, {431, 'p', 5}, {624, 'p', 6},
    };
    CHECK(hits == want);
    if (hits != want)
        for (auto [f, who, amount] : hits) std::println(stderr, "  got {{{}, '{}', {}}}", f, who, amount);
}

// Session and step(): what main.cpp's loop did inline before, now drivable headless.
static void session() {
    auto make = [](uint32_t seed) {
        Session s{.world = meadow(), .seed = seed};
        s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}},
                     {.id = 2, .kind = MarkerKind::Region, .name = "Pond", .pos = {48.5f, 4, 48.5f}, .radius = 6},
                     {.id = 3, .kind = MarkerKind::Spawn, .pos = {16.5f, 4, 10.5f}, .radius = 3, .entity = EntityKind::Bandit}};
        startSession(s);
        return s;
    };
    constexpr float dt = 1 / 60.0f;

    // Starting gear: a shortsword and a satchel in the backpack, nothing equipped.
    Session s = make(1);
    CHECK(s.player.pos == glm::vec3(10.5f, 4, 10.5f));
    CHECK(s.player.inv.containers[0][0] == Shortsword && s.player.inv.containers[0][1] == Satchel);
    CHECK(freeSlots(s.player.inv) == kBackpackSlots - 2);
    CHECK(std::ranges::all_of(s.player.inv.equipped, [](ItemId id) { return id == NoItem; }));
    CHECK(s.entities.size() == 1 && s.entities[0].kind == EntityKind::Bandit);
    CHECK(s.rng() == std::mt19937(1)());  // the sim RNG is seeded from the world seed

    // Editing: NPCs and combat pause, and no swing starts.
    {
        glm::vec3 banditAt = s.entities[0].body.pos;
        SimInput edit;
        edit.editing = edit.attack = true;
        edit.aim = {1, 0};
        for (int i = 0; i < 60; ++i) CHECK(!step(s, edit, dt).attacked);
        CHECK(s.entities[0].body.pos == banditAt && !s.player.punching());
    }

    // Interact: next to a corpse that holds loot, it moves into the bags.
    {
        Entity& bandit = s.entities[0];
        CHECK(damage(bandit, 1000) && !bandit.body.alive());
        bandit.loot = Knife;  // whatever the drop roll said
        s.player.pos = bandit.body.pos + glm::vec3(1.5f, 0, 0);
        SimInput loot;
        loot.interact = true;
        step(s, loot, dt);
        CHECK(bandit.loot == NoItem && s.player.inv.containers[0][2] == Knife);
    }

    // Time of day: speeds up with timeRate and wraps past midnight.
    {
        Session t{.world = Grid({4, 4, 4})};
        t.timeOfDay = 0.9999f;
        SimInput fast;
        fast.timeRate = 60;
        step(t, fast, 1);
        CHECK(t.timeOfDay < 0.1f && std::abs(t.timeOfDay - (0.9999f + 60 / kDayLength - 1)) < 1e-4f);
    }

    // Region enter: fires once on entering (regionSince stays put while inside), clears on leaving.
    s.entities.clear();  // no bandit in the way
    step(s, {}, dt);
    CHECK(s.regionId == 0);
    s.player.pos = {47.5f, 4, 47.5f};
    step(s, {}, dt);
    CHECK(s.regionId == 2 && s.regionSince == s.elapsed);
    double entered = s.regionSince;
    for (int i = 0; i < 30; ++i) step(s, {}, dt);
    CHECK(s.regionId == 2 && s.regionSince == entered);
    s.player.pos = {20.5f, 4, 20.5f};
    step(s, {}, dt);
    CHECK(s.regionId == 0 && s.regionSince == s.elapsed);

    // Knocked out: input is ignored, and after kPlayerRespawnSeconds the player is back at the
    // player spawn with full health, once.
    CHECK(hurt(s.player, 1000) && !s.player.alive());
    glm::vec3 down = s.player.pos;
    SimInput walk;
    walk.move.move = {1, 0};
    walk.attack = true;
    int respawns = 0, frames = 0;
    for (; frames < 6 * 60 && !respawns; ++frames) {
        StepResult r = step(s, walk, dt);
        respawns += r.respawned;
        CHECK(r.attacked);  // a knocked-out player's click is used up
        if (!r.respawned) CHECK(s.player.pos == down);
    }
    CHECK(respawns == 1 && frames > int(kPlayerRespawnSeconds * 60) - 2 && frames < int(kPlayerRespawnSeconds * 60) + 3);
    CHECK(s.player.alive() && s.player.health == float(maxHealth(s.player.stats)) && s.player.deadTime == 0);
    CHECK(glm::distance(glm::vec2(s.player.pos.x, s.player.pos.z), glm::vec2(10.5f, 10.5f)) < 0.2f);  // walked one step since
    CHECK(s.player.combatTimer == 0);

    // Determinism: the same seed and scripted input give the same fight, step for step.
    auto fight = [&](uint32_t seed) {
        Session f = make(seed);
        std::vector<std::tuple<int, float, float, float>> trace;  // frame, player health, bandit health, player x
        for (int frame = 0; frame < 4 * 60; ++frame) {
            const Entity& bandit = f.entities[0];
            glm::vec2 to{bandit.body.pos.x - f.player.pos.x, bandit.body.pos.z - f.player.pos.z};
            SimInput in;
            in.aim = glm::length(to) > 0 ? glm::normalize(to) : glm::vec2(1, 0);
            in.move.move = glm::length(to) > 1.5f ? in.aim : glm::vec2(0);
            in.attack = glm::length(to) < kPunchReach - 0.1f;
            in.interact = frame % 60 == 0;
            step(f, in, dt);
            trace.emplace_back(frame, f.player.health, bandit.body.health, f.player.pos.x);
        }
        return std::pair{trace, f.rng()};
    };
    auto [a, aNext] = fight(7);
    auto [b, bNext] = fight(7);
    CHECK(a == b && aNext == bNext);
    CHECK(fight(8) != fight(7));  // another seed rolls other damage
    CHECK(std::get<2>(a.back()) < 120 && std::get<1>(a.back()) < 100);  // blows were traded: the rolls ran
}

// The fixed tick: N ticks of a scripted input end in exactly the same state, and bodies are drawn
// between their last two ticks (but never slid across a teleport).
static void ticks() {
    Session s{.world = meadow(), .seed = 3};
    s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}},
                 {.id = 2, .kind = MarkerKind::Spawn, .pos = {20.5f, 4, 24.5f}, .radius = 4, .entity = EntityKind::Villager}};
    startSession(s);
    CHECK(s.player.prevPos == s.player.pos && s.entities.size() == 1 && s.entities[0].body.prevPos == s.entities[0].body.pos);
    SimInput in;
    in.move.move = glm::normalize(glm::vec2(1, 0.5f));
    in.move.run = true;
    for (int tick = 0; tick < 120; ++tick) {
        in.move.jump = tick == 30;
        glm::vec3 before = s.player.pos;
        step(s, in, kTick);
        CHECK(s.player.prevPos == before);
    }
    // 2 s running along (1, 0.5) with one jump (accelerating, then 9 b/s), facing the way it runs; the villager strolls.
    same("ticks", "player.x", s.player.pos.x, 25.891537f);
    same("ticks", "player.y", s.player.pos.y, 4.001f);
    same("ticks", "player.z", s.player.pos.z, 18.195745f);
    same("ticks", "player.yaw", s.player.yaw, 1.1071485f);
    same("ticks", "elapsed", s.elapsed, 120 * double(kTick));
    same("ticks", "villager.x", s.entities[0].body.pos.x, 20.693283f);
    same("ticks", "villager.z", s.entities[0].body.pos.z, 20.679798f);

    // Drawing: alpha 0 is the previous tick, 1 the current; yaw turns the short way past ±pi.
    Player b;
    b.prevPos = {0, 0, 0};
    b.pos = {2, 0, 0};
    CHECK(drawPos(b, 0) == b.prevPos && drawPos(b, 1) == b.pos && drawPos(b, 0.25f) == glm::vec3(0.5f, 0, 0));
    b.prevYaw = 3.0f;
    b.yaw = -3.0f;
    CHECK(std::abs(drawYaw(b, 0.5f)) > 3.1f);  // through pi, not through 0
    b.teleport({9, 0, 9});
    CHECK(drawPos(b, 0.3f) == glm::vec3(9, 0, 9) && drawYaw(b, 0.3f) == b.yaw);
    // A respawn is a teleport too: the next frame draws it at the spawn, whatever alpha.
    s.player.pos = {40.5f, 4, 40.5f};
    CHECK(hurt(s.player, 1000));
    s.player.deadTime = kPlayerRespawnSeconds;
    CHECK(step(s, {}, kTick).respawned);
    CHECK(glm::distance(drawPos(s.player, 0), glm::vec3(10.5f, 4, 10.5f)) < 0.05f);
}

int main() {
    meshing();
    packing();
    physics();
    entities();
    combat();
    fighters();
    items();
    protection();
    generation();
    regions();
    worldFiles();
    editing();
    commands();
    prefabs();
    markers();
    storyNpcs();
    markerLines();
    raycasting();
    tables();
    fightReplay();
    boarReplay();
    session();
    ticks();
    if (failures) std::println(stderr, "{} check(s) failed", failures);
    else std::println("all checks passed");
    return failures ? 1 : 0;
}
