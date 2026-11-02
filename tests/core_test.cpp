// Assert-style checks for the engine core. Not using <cassert>: release builds define NDEBUG.
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
    CHECK(player.health == 100 && player.maxHealth == 100);
    player.pos = {28.5f, 4, 20.5f};
    std::vector<Entity> es = spawnEntities(g, player.pos, 1);
    CHECK(es.size() == 2);
    Entity &dummy = es[0], &villager = es[1];

    CHECK(dummy.kind == EntityKind::TrainingDummy && dummy.name == "Training Dummy" && dummy.level == 1);
    CHECK(dummy.body.health == 100 && dummy.body.maxHealth == 100);
    CHECK(dummy.type().attackable && !dummy.type().wanders && !dummy.type().talks);
    CHECK(villager.kind == EntityKind::Villager && villager.level == 5 && !villager.name.empty());
    CHECK(villager.body.health == 250 && villager.body.maxHealth == 250);
    CHECK(!villager.type().attackable && villager.type().wanders && villager.type().talks);
    CHECK(spawnEntities(g, player.pos, 1)[1].name == villager.name);  // names are seeded
    CHECK(glm::distance(dummy.body.pos, villager.body.pos) >= 1.5f);  // nobody spawns inside anybody
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
    CHECK(maxFromHome < kWanderRadius + 2);
    CHECK(!wet);
    CHECK(!stuck);
    CHECK(dummy.body.pos == dummyPos);
    CHECK(villager.speech() == nullptr);  // the balloon timed out
}

int main() {
    meshing();
    packing();
    physics();
    entities();
    generation();
    regions();
    if (failures) std::println(stderr, "{} check(s) failed", failures);
    else std::println("all checks passed");
    return failures ? 1 : 0;
}
