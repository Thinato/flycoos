// Assert-style checks for the engine core. Not using <cassert>: release builds define NDEBUG.
#include <algorithm>
#include <numbers>
#include <print>

#include "entity.h"
#include "mesher.h"

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

static void generation() {
    Grid a({64, 128, 64}), b({64, 128, 64}), c({64, 128, 64});
    generateTerrain(a, 7);
    generateTerrain(b, 7);
    generateTerrain(c, 8);
    CHECK(a.cells == b.cells);
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
    CHECK(moved > 700);  // most random pairs are two empty slots; this seed makes ~870 real moves

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

int main() {
    meshing();
    packing();
    physics();
    entities();
    combat();
    fighters();
    items();
    generation();
    regions();
    if (failures) std::println(stderr, "{} check(s) failed", failures);
    else std::println("all checks passed");
    return failures ? 1 : 0;
}
