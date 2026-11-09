// Assert-style checks for the engine core. Not using <cassert>: release builds define NDEBUG.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <unistd.h>  // mkdtemp
#include <filesystem>
#include <format>
#include <limits>
#include <fstream>
#include <numbers>
#include <print>
#include <ranges>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>

#include <glm/gtc/matrix_transform.hpp>

#include "audio.h"
#include "commands.h"
#include "defs.h"
#include "edit.h"
#include "entity.h"
#include "light.h"
#include "lockon.h"
#include "melee.h"
#include "mesher.h"
#include "notes.h"
#include "plants.h"
#include "prefab.h"
#include "save.h"
#include "trees.h"
#include "session.h"
#include "stream.h"

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

// Colour patch tones (ADR-028, docs/ART.md): plain blocks have none, man-made ones are fainter
// than every natural one, and every patched block has a patch size. Every block is in one class.
static void tones() {
    constexpr Block plain[] = {Air, Water, Ember, Lantern, Skin, Tunic, Belt, Boots, Hair, Eye, Robe, DarkHair, Straw,
                               Burlap, Steel, Iron, Hide, Tusk, Snout, Hood, Leather, Plate,
                               ShortGrass, TallGrass, FlowerWhite, FlowerYellow, FlowerRed, FlowerViolet,  // drawn as models
                               PetalWhite, PetalYellow, PetalRed, PetalViolet, Stem, FlowerHeart};
    constexpr Block made[] = {Planks, DarkPlanks, Brick, Cobble, MossyCobble, Thatch, Hay, Clay, Slate};
    constexpr Block natural[] = {Grass, Dirt, Sand, Snow, Stone, Rock, Wood, BirchBark, PineBark, AraucariaBark, Blade, TallBlade};
    std::array<int, BlockCount> seen{};
    for (int k = 0; k < int(Leaf::Count); ++k)  // leaves: natural, and the 4 shades get strictly lighter
        for (int s = 0; s < kLeafShades; ++s) {
            Block b = leafBlock(Leaf(k), s);
            ++seen[b];
            CHECK(kLeafOf[b].shade == s && kLeafOf[b].kind == Leaf(k) && isLeaf(b) && isTreeBlock(b) && kTones[b].strength >= 0.6f);
            auto sum = [](Block x) { return kPalette[x][0] + kPalette[x][1] + kPalette[x][2]; };
            if (s > 0) CHECK(sum(b) > sum(leafBlock(Leaf(k), s - 1)));
        }
    CHECK(leafBlock(Leaf::Oak, 2) == Leaves && BlockCount <= 128);
    for (Block b : plain) {
        ++seen[b];
        CHECK(kTones[b].strength == 0);
    }
    for (Block m : made) {
        ++seen[m];
        for (Block n : natural) CHECK(kTones[m].strength > 0 && kTones[m].strength < kTones[n].strength);
    }
    for (Block n : natural) ++seen[n];
    CHECK(std::ranges::all_of(seen, [](int n) { return n == 1; }));  // a new block must pick a class
    for (const BlockTone& t : kTones) CHECK(t.strength >= 0 && t.strength <= 1 && (t.strength == 0 || t.size > 0));
}

// The world's chunk meshes come from core (meshChunks/remeshChunks); the renderer only uploads them.
static void chunkMeshes() {
    Grid g({64, 128, 64});
    generateTerrain(g, 7);  // the world `generation` pins
    std::vector<Mesh> meshes = meshChunks(g);
    CHECK(meshes.size() == 16);
    size_t opaque = 0, water = 0;
    for (size_t i = 0; i < meshes.size(); ++i) {
        Mesh one = meshRegion(g, g.chunkOrigin(i), glm::ivec3(kChunk));
        CHECK(meshes[i].opaque == one.opaque && meshes[i].water == one.water);
        opaque += meshes[i].opaque.size(), water += meshes[i].water.size();
    }
    CHECK(opaque == 7673 && water == 27);  // coasts, rivers and lakes (8201 before them)

    // A voxel edited on a chunk corner remeshes the 8 chunks its faces and AO reach, and they match a
    // full remesh.
    glm::ivec3 corner(32, 64, 32);
    g.set(corner, g.get(corner) == Air ? Stone : Air);
    std::vector<size_t> ids = remeshChunks(g, corner, corner, meshes);
    CHECK(ids.size() == 8);
    std::vector<Mesh> fresh = meshChunks(g);
    bool same = true;
    for (size_t i = 0; i < fresh.size(); ++i) same &= fresh[i].opaque == meshes[i].opaque && fresh[i].water == meshes[i].water;
    CHECK(same);
    CHECK(remeshChunks(g, {0, 0, 0}, {0, 0, 0}, meshes) == std::vector<size_t>{0});  // a corner voxel: its own chunk
}

static void packing() {
    QuadInfo cases[] = {
        {{0, 0, 0}, 1, 1, 0, {0, 0, 0, 0}, Grass},
        {{31, 31, 31}, 32, 32, 5, {3, 2, 1, 0}, Eye},
        {{7, 19, 2}, 13, 4, 3, {1, 3, 0, 2}, Water},
        {{1, 2, 3}, 4, 5, 2, {2, 1, 0, 3}, Block(63), 7, 11},  // the highest block id the shader's 6 bits hold
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
    {  // a roll goes the way it moves, about 4 blocks, on the ground only; standing still it's a
       // backstep (ADR-038): a hop back under 2 blocks, still facing ahead
        auto rolled = [](glm::vec2 move, float yaw, bool airborne) {
            Grid g = flatWorld(0);
            Player p;
            p.pos = {7.5f, airborne ? 6.0f : 4.001f, 7.5f};
            p.yaw = yaw;
            updatePlayer(p, g, {}, 1 / 120.0f);  // settles (or starts falling)
            Input in;
            in.move = move, in.roll = true;
            updatePlayer(p, g, in, 1 / 120.0f);
            bool started = p.rolling(), punch = startPunch(p, {1, 0});
            in.roll = false;
            float yawAfter = p.yaw;
            for (int i = 0; i < 60; ++i) updatePlayer(p, g, {}, 1 / 120.0f);  // past the roll's 0.45 s
            return std::tuple{started, punch, glm::vec2(p.pos.x - 7.5f, p.pos.z - 7.5f), yawAfter, p.rolling()};
        };
        auto [started, punch, by, yaw, still] = rolled({0, 1}, 0, false);
        CHECK(started && !punch && !still);  // no swing mid-roll; over after its length
        CHECK(by.y > 4.4f && by.y < 5.4f && std::abs(by.x) < 0.01f);  // straight ahead (yaw 0 faces +Z), light: unarmored
        auto [startedB, punchB, byB, yawB, stillB] = rolled({}, 0, false);
        CHECK(startedB && !punchB && !stillB && byB.y < -1.4f && byB.y > -2.3f && std::abs(byB.x) < 0.01f && yawB == 0);
        Grid flat = flatWorld(0);  // and it still faces ahead once it has slid to a stop
        Player b;
        b.pos = {7.5f, 4.001f, 7.5f};
        updatePlayer(b, flat, {}, 1 / 120.0f);
        Input back;
        back.roll = true;
        updatePlayer(b, flat, back, 1 / 120.0f);
        for (int i = 0; i < 120; ++i) updatePlayer(b, flat, {}, 1 / 120.0f);
        CHECK(b.backstep && !b.rolling() && b.yaw == 0 && glm::length(glm::vec2(b.vel.x, b.vel.z)) < 0.1f);
        auto [started2, punch2, by2, yaw2, still2] = rolled({-1, 0}, 0, false);
        CHECK(started2 && by2.x < -3.5f && std::abs(by2.y) < 0.01f);  // toward -X, turned to face it
        CHECK(std::abs(yaw2 + std::numbers::pi_v<float> / 2) < 1e-4f);
        auto [started3, punch3, by3, yaw3, still3] = rolled({1, 0}, 0, true);
        CHECK(!started3 && punch3);  // in the air: no roll
    }
    {  // a roll just after a jump goes through the air with it; one later in the jump doesn't start
        auto airRoll = [](int ticksAfterJump) {
            Grid g = flatWorld(0);
            Player p;
            p.pos = {7.5f, 4.001f, 4.5f};
            updatePlayer(p, g, {}, 1 / 120.0f);
            Input in;
            in.jump = true;
            updatePlayer(p, g, in, 1 / 120.0f);
            in.jump = false;
            for (int i = 0; i < ticksAfterJump; ++i) updatePlayer(p, g, in, 1 / 120.0f);
            in.move = {0, 1}, in.roll = true;
            updatePlayer(p, g, in, 1 / 120.0f);
            return p.rolling() && !p.onGround;
        };
        CHECK(airRoll(12));   // 0.1 s after take-off
        CHECK(!airRoll(60));  // 0.5 s: past the window, still in the air
    }
    {  // a jump still takes off for a few frames after stepping off an edge, once
        auto jumpAfterEdge = [](int ticks) {
            Grid g = flatWorld(0);
            Player p;
            p.pos = {7.5f, 4.001f, 7.5f};
            updatePlayer(p, g, {}, 1 / 60.0f);
            p.pos.y = 6;  // as if it just walked off a ledge: falling from here
            for (int i = 0; i < ticks; ++i) updatePlayer(p, g, {}, 1 / 60.0f);
            Input in;
            in.jump = true;
            updatePlayer(p, g, in, 1 / 60.0f);
            float up = p.vel.y;
            updatePlayer(p, g, in, 1 / 60.0f);  // held: no second jump in the air
            return std::pair{up > 5, p.vel.y < up};
        };
        auto [late4, once] = jumpAfterEdge(4);
        CHECK(late4 && once);
        CHECK(!jumpAfterEdge(10).first);  // 1/6 s: too late
    }
    {  // jumping while rolling cuts the roll short: the turn carries on from where it was, quickly
        Grid g = flatWorld(0);
        Player p;
        p.pos = {7.5f, 4.001f, 4.5f};
        updatePlayer(p, g, {}, 1 / 120.0f);
        Input in;
        in.roll = true;
        updatePlayer(p, g, in, 1 / 120.0f);
        in.roll = false;
        for (int i = 0; i < 12; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        float u = p.rollTime / p.rollLength;
        in.jump = true;
        updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.vel.y > 5 && p.rolling());
        CHECK(std::abs(p.rollTime / p.rollLength - u) < 0.05f);  // one tick on, not a jump in the pose
        in.jump = false;
        for (int i = 0; i < 15; ++i) updatePlayer(p, g, in, 1 / 120.0f);  // 0.125 s later
        CHECK(!p.rolling() && !p.onGround);
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
        for (int i = 0; i < 120; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.inWater && p.swimAmount > 0.99f);  // swim pose in the pool: leaning into a crawl, hands off the walk swing
        Player walker = p;
        walker.swimAmount = 0;
        CHECK(poseModel(p, ModelPlayer)[HandL] != poseModel(walker, ModelPlayer)[HandL]);
        for (int i = 120; i < 1200; ++i) updatePlayer(p, g, in, 1 / 120.0f);
        CHECK(p.pos.x > 10.5f);
        CHECK(p.swimAmount < 0.01f);  // out on the shore
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
    CHECK(fnv(a) == 0x22c3a6e7u);  // coasts, rivers and lakes; 0x209bcf65 before them
    CHECK(a.cells != c.cells);
    glm::vec3 spawn = findSpawn(a);
    CHECK(a.get(glm::ivec3(glm::floor(spawn)) - glm::ivec3(0, 1, 0)) == Grass);

    // The endless world: each chunk column is the same whichever grid holds it and wherever its
    // window is, so a streamed window and a fixed grid elsewhere agree where they overlap.
    Grid s = Grid::streamed({128, 128, 128}, {-96, 0, -32}), f({64, 128, 64}, {-64, 0, -64});
    generateTerrain(s, 7);
    generateTerrain(f, 7);
    bool same = true, trees = false;
    for (int y = 0; y < 128; ++y)
        for (int z = -32; z < 0; ++z)
            for (int x = -64; x < 0; ++x) {
                same &= s.get({x, y, z}) == f.get({x, y, z});
                trees |= isTreeBlock(f.get({x, y, z}));
            }
    CHECK(same && trees);
    for (int y = 0; y < 128; ++y)  // and the window at 0 holds the fixed grid at 0
        for (int z = 0; z < 64; ++z)
            for (int x = 0; x < 32; ++x) same &= s.get({x, y, z}) == a.get({x, y, z});
    CHECK(same);
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
                tree |= isTreeBlock(b);
                rock |= b == Rock;
            }
            forest += tree;
            rocky += rock;
            sum[size_t(z + 1) * (n + 1) + x + 1] = (tree || rock) + sum[size_t(z) * (n + 1) + x + 1] +
                                                   sum[size_t(z + 1) * (n + 1) + x] - sum[size_t(z) * (n + 1) + x];
        }
    CHECK(fnv(g) == 0x73170551u);  // coasts, rivers and lakes; 0xd779f799 before them
    // Lakes lie above the sea at their own levels, and no water is left open to the air at its
    // sides or under it (a lake's rim holds it in).
    int lake = 0, exposed = 0;
    for (int y = 1; y < g.size.y; ++y)
        for (int z = 1; z < n - 1; ++z)
            for (int x = 1; x < n - 1; ++x) {
                if (g.get({x, y, z}) != Water) continue;
                lake += y > kSeaLevel;
                for (glm::ivec3 d : {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1), glm::ivec3(0, -1, 0)})
                    exposed += g.get(glm::ivec3(x, y, z) + d) == Air;
            }
    CHECK(lake > 10000 && exposed == 0);
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

    CHECK(dummy.kind == EntityKind::TrainingDummy && dummy.name == "Training Dummy" && dummy.body.level == 1);
    CHECK(dummy.body.health == 100 && maxHealth(dummy.body.stats) == 100);
    CHECK(dummy.type().attackable && !dummy.type().wanders && !dummy.type().talks);
    CHECK(villager.kind == EntityKind::Villager && villager.body.level == 5 && !villager.name.empty());
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
    CHECK(boar.kind == EntityKind::Boar && boar.body.level == 1 && boar.type().temper == Temper::Passive);
    CHECK(boar.body.health == player.health && maxHealth(boar.body.stats) == maxHealth(player.stats));
    CHECK(strengthBonus(boar.body.stats, kPunch) == strengthBonus(player.stats, kPunch));
    CHECK(mainAttack(boar.body.inv).minDamage == kPunch.minDamage && mainAttack(boar.body.inv).maxDamage == kPunch.maxDamage);
    CHECK(bandit.kind == EntityKind::Bandit && bandit.body.level == 2 && bandit.type().temper == Temper::Aggressive);
    CHECK(maxHealth(bandit.body.stats) > maxHealth(player.stats) && bandit.body.stats.strength > player.stats.strength);
    CHECK(bandit.body.inv.equipped[SlotMainHand] == Knife);
    const Attack &knife = itemType(Knife).attack, &sword = itemType(Shortsword).attack;
    CHECK(knife.speed < sword.speed && knife.maxDamage < sword.maxDamage && knife.minDamage < sword.minDamage);
    // Its camp is out of sight of the spawn: even at the edge of its wandering it can't see it,
    // and the spawn is outside its territory.
    float campDist = glm::distance(flat(bandit.home), flat(player.pos));
    CHECK(campDist - bandit.type().wanderRadius > bandit.type().aggroRange && campDist > bandit.type().leashRadius);

    // A minute at the spawn: nobody starts a fight.
    sim(60);
    CHECK(player.health == 100 && !boar.angry() && !bandit.angry());

    // Passive: standing right next to the boar is fine, until it gets hit. Then it gores back.
    player.pos = boar.body.pos + glm::vec3(1.5f, 0, 0);
    sim(5);
    CHECK(player.health == 100 && !boar.angry());
    CHECK(damage(boar, 1) && !boar.angry());  // it reads the hit on its next update
    updateEntities(es, g, player, 1 / 60.0f);
    CHECK(boar.angry());
    sim(5);
    CHECK(player.health < 100 && player.lastDamage >= 3 && player.lastDamage <= 6);  // a punch, like the player's
    CHECK(!bandit.angry());
    setState(boar, AiState::Wander, "test");

    // Aggressive: the bandit charges a player inside its aggro range and hits with the knife.
    player.health = 100;
    player.pos = bandit.body.pos + glm::vec3(-8, 0, 0);
    sim(0.1f);
    CHECK(bandit.angry());
    sim(6);
    CHECK(player.health < 100 && player.lastDamage >= 5 && player.lastDamage <= 8);  // 3-6 + strength 3 x speed 0.75
    CHECK(glm::distance(flat(bandit.body.pos), flat(player.pos)) < kPunchReach);

    // Leash: once the player is out of its territory it gives up (after the swing under way) and walks home.
    player.pos = bandit.home + glm::vec3(-bandit.type().leashRadius - 1, 0, 0);
    sim(1);
    CHECK(!bandit.angry());
    sim(30);
    CHECK(!bandit.angry() && glm::distance(flat(bandit.body.pos), flat(bandit.home)) < bandit.type().wanderRadius + 1);

    // Knocked out: fighters stop, and the player can't swing until back up.
    player.health = 1;
    player.pos = bandit.body.pos + glm::vec3(-3, 0, 0);
    sim(6);
    CHECK(!player.alive() && player.deadTime > 0 && !bandit.angry() && !startPunch(player, {1, 0}));
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
    bandit.events = 0;  // forget those hits
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
    boar.body.vel = {};  // (killed mid-stride it would slide to a stop first)
    glm::vec3 fell = boar.body.pos;
    boar.body.deadTime = 0;
    setState(boar, AiState::Chase, "test");
    sim(boar.type().respawnSeconds - 1);
    CHECK(!boar.body.alive() && !boar.angry() && flat(boar.body.pos) == flat(fell));
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
    b.moveTime = impactTime(b);
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
    startMove(p, Move::Light4, {0, 1});  // the thrust: at its hit the blade points straight ahead
    p.moveTime = impactTime(p);
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
    for (int i = 1; i < int(defs().items.size()); ++i)
        for (int j = 1; j < int(defs().items.size()); ++j) {
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
    CHECK(giveItem(player.inv, IronHelm) && sheet(player)[Stat::Armor] == 0);  // a spare helm stays in the bags
    for (int i = 0; i < 5; ++i) CHECK(quickMove(player.inv, {Item, i, 0}));
    CHECK(sheet(player)[Stat::Armor] == 37 && player.inv.containers[0][5] == IronHelm);
    CHECK(quickMove(player.inv, {Equip, SlotChest}) && sheet(player)[Stat::Armor] == 25);
    CHECK(quickMove(player.inv, {Item, 0, 0}) && sheet(player)[Stat::Armor] == 37);

    // A bandit (level 2, knife 5-8 unarmored) hits the armored player for 3-5; with K = 1 armor
    // takes almost everything.
    Grid g = meadow();
    player.pos = {20.5f, 4, 20.5f};
    std::vector<Entity> es = spawnEntities(g, player.pos, 1);
    Entity& bandit = es[3];
    player.pos = bandit.body.pos + glm::vec3(-1.5f, 0, 0);
    setState(bandit, AiState::Chase, "test");
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

// The shipped items.json cut to the items code names (ItemId), open for appending records: tests
// add theirs from save id ItemCount on, whatever the data adds after those.
static std::string codeNamedItems() {
    std::ostringstream all;
    all << std::ifstream(std::filesystem::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json").rdbuf();
    std::string text = all.str();
    size_t at = text.find(std::format("\"save_id\": {},", int(ItemCount)));
    text.erase(at == std::string::npos ? text.rfind(']') : text.rfind(',', text.rfind('{', at)));
    return text;
}

// The shipped creatures.json for codeNamedItems: less the shields they hold (code doesn't name those),
// and with them the rest of their record from there (the tactics a shield makes valid).
static std::string codeItemCreatures() {
    std::ostringstream all;
    all << std::ifstream(std::filesystem::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string text = all.str();
    for (size_t at; (at = text.find(R"("shield": ")")) != std::string::npos;) {
        size_t from = text.rfind(',', at);
        text.erase(from, text.find('}', at) - from);
    }
    return text;
}

// The stat pipeline (ADR-022): compute's arithmetic, collect from worn gear, the trinket slot, the
// sheet behind regen, max health and swing time, and resolveHit's order.
static void statSheets() {
    namespace fs = std::filesystem;
    auto near = [](float a, float b) { return std::abs(a - b) < 1e-3f; };
    using enum ModKind;
    Stats base;  // the player's: 10 2 2 2 2
    // Increased modifiers add up, more ones multiply, and flat ones come before both.
    Modifier inc50{Stat::Strength, Increased, 0.5f}, more50{Stat::Strength, More, 0.5f};
    CHECK(compute(base, std::array{inc50, inc50})[Stat::Strength] == 4.0f);    // 2 x 2.0
    CHECK(compute(base, std::array{more50, more50})[Stat::Strength] == 4.5f);  // 2 x 2.25
    CHECK(compute(base, std::array{inc50, Modifier{Stat::Strength, Flat, 3}})[Stat::Strength] == 7.5f);  // (2 + 3) x 1.5
    std::array mix{Modifier{Stat::Stamina, Flat, 5}, Modifier{Stat::Stamina, Increased, 0.1f},
                   Modifier{Stat::Stamina, Increased, 0.2f}, Modifier{Stat::Stamina, More, 0.5f}};
    CHECK(near(compute(base, mix)[Stat::Stamina], 29.25f));  // (10 + 5) x 1.3 x 1.5
    // Derived stats start from the final attributes, then take their own modifiers.
    CHECK(maxHealth(compute(base, std::array{Modifier{Stat::Stamina, Flat, 5}, Modifier{Stat::MaxHealth, Increased, 0.1f}})) == 165);
    // No modifiers: the formulas of ADR-007 exactly.
    constexpr StatSheet plain = compute(Stats{});
    static_assert(maxHealth(plain) == 100 && plain[Stat::Damage] == 1 && plain[Stat::Armor] == 0 && plain[Stat::DamageTaken] == 1);
    CHECK(plain[Stat::Haste] == 2 * kHastePerAgility && healthRegen(plain) == 2 * kRegenPerSpirit);
    // Nothing goes below 0, and max health not below 1.
    StatSheet cursed = compute(base, std::array{Modifier{Stat::Stamina, Flat, -50}, Modifier{Stat::Agility, Increased, -3}});
    CHECK(cursed[Stat::Stamina] == 0 && maxHealth(cursed) == 1 && cursed[Stat::Agility] == 0 && swingTime(cursed, kPunch) == kPunch.speed);
    Modifier nothing{Stat::Strength, More, -2};  // -200% more counts as -100%: two of them don't cancel out
    CHECK(compute(base, std::array{nothing, nothing})[Stat::Strength] == 0);
    // Haste may go negative, a slow, down to -90% (10x the swing time).
    CHECK(near(swingTime(compute(base, std::array{Modifier{Stat::Haste, Flat, -0.5f}}), kPunch), 1 / 0.52f));
    CHECK(near(swingTime(compute(base, std::array{Modifier{Stat::Haste, Flat, -5}}), kPunch), 10));
    // Every stat stops at kMaxStat, so the integer consumers can't overflow.
    Modifier huge{Stat::Stamina, More, 10};
    StatSheet giant = compute(base, std::array{huge, huge, huge, huge, huge, huge});
    CHECK(giant[Stat::Stamina] == kMaxStat && maxHealth(giant) == int(kMaxStat));

    // Worn gear adds its armor and mods, each with its source; taking it all off restores the base exactly.
    using enum SlotRef::Kind;
    Player p;
    CHECK(collect(p).empty() && sheet(p).v == compute(p.stats).v && p.level == 1);
    for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, Shortsword, BoneCharm}) {
        CHECK(giveItem(p.inv, id));
        CHECK(quickMove(p.inv, {Item, 0, 0}));
    }
    CHECK(p.inv.equipped[SlotTrinket] == BoneCharm);  // right-click puts the charm in the trinket slot
    std::vector<Modifier> mods = collect(p);
    CHECK(mods.size() == 6 && std::string_view(mods[0].source) == "Iron Helm" && mods[0].stat == Stat::Armor && mods[0].value == 6);
    CHECK(std::string_view(mods[5].source) == "Bone Charm" && mods[5].stat == Stat::Spirit && mods[5].kind == Flat);
    StatSheet worn = sheet(p);
    CHECK(worn[Stat::Armor] == 37 && worn[Stat::Spirit] == 4 && healthRegen(worn) == 4 * kRegenPerSpirit && maxHealth(worn) == 100);
    for (int s = 0; s < EquipCount; ++s) quickMove(p.inv, {Equip, s});
    CHECK(collect(p).empty() && sheet(p).v == compute(p.stats).v && freeSlots(p.inv) == kBackpackSlots - 7);
    // The trinket slot takes trinkets only, and a trinket goes nowhere else.
    Player q;
    CHECK(giveItem(q.inv, BoneCharm) && giveItem(q.inv, IronHelm));
    CHECK(!moveItem(q.inv, {Item, 0, 0}, {Equip, SlotHead}) && !moveItem(q.inv, {Item, 1, 0}, {Equip, SlotTrinket}));
    CHECK(moveItem(q.inv, {Item, 0, 0}, {Equip, SlotTrinket}) && sheet(q)[Stat::Spirit] == 4);

    // Out-of-combat regen reads the sheet: the charm doubles it.
    std::mt19937 rng(1);
    std::vector<Entity> nobody;
    auto regen = [&](Player& b) {
        b.health = 50;
        for (int i = 0; i < 60; ++i) updateCombat(b, nobody, 1 / 60.0f, rng);
        return b.health - 50;
    };
    Player bare;
    CHECK(near(regen(bare), 1) && near(regen(q), 2));

    // Test items with every kind of modifier (the shipped ones have none but the charm's).
    fs::path dir = fs::temp_directory_path() / "flycoos_stats_test";
    fs::create_directories(dir);
    std::ofstream(dir / "creatures.json") << codeItemCreatures();
    std::string items = codeNamedItems();
    std::ofstream(dir / "items.json") << items << R"(,
        {"id": "amulet", "save_id": 12, "name": "Amulet", "slot": "trinket",
         "mods": [{"stat": "stamina", "flat": 5}, {"stat": "max_health", "increased": 0.1}, {"stat": "agility", "flat": 50}]},
        {"id": "fury_gloves", "save_id": 13, "name": "Fury Gloves", "slot": "hands", "armor": 4, "mods": [{"stat": "damage", "more": 0.5}]},
        {"id": "ward", "save_id": 14, "name": "Ward", "slot": "off_hand", "mods": [{"stat": "damage_taken", "increased": -0.5}]},
        {"id": "plain_ward", "save_id": 15, "copy_from": "ward", "name": "Plain Ward", "mods": null},
        {"id": "ward_copy", "save_id": 16, "copy_from": "ward", "name": "Ward Copy"}])";
    Defs test, shipped;
    std::string error;
    CHECK(loadDefs(dir, test, error) && loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
    if (!error.empty()) std::println(stderr, "FAIL stats test data: {}", error);
    fs::remove_all(dir);
    if (test.items.size() != 17) return;
    CHECK(test.items[15].mods.empty() && test.items[16].mods.size() == 1 && test.items[16].mods[0].stat == Stat::DamageTaken &&
          test.items[16].mods[0].kind == Increased && test.items[16].mods[0].value == -0.5f);
    setDefs(std::move(test));
    constexpr auto kAmulet = ItemId(12), kGloves = ItemId(13), kWard = ItemId(14);
    // Max health and swing time read the sheet; unequipping caps health at the base maximum again.
    Player hero;
    hero.inv.equipped[SlotTrinket] = kAmulet;
    CHECK(maxHealth(sheet(hero)) == 165);  // (10 + 5) x 10 x 1.1
    hero.health = 200;
    updateCombat(hero, nobody, 1 / 60.0f, rng);
    CHECK(hero.health == 165);
    hero.swingTimer = 0;
    CHECK(startPunch(hero, {0, 1}) && near(hero.swingTimer, 1 / 1.52f));  // agility 52: 52% faster
    hero.inv.equipped[SlotTrinket] = NoItem;
    updateCombat(hero, nobody, 1 / 60.0f, rng);
    CHECK(hero.health == 100 && sheet(hero).v == compute(hero.stats).v);
    // resolveHit: the roll plus strength, times the attacker's damage, less armor by the attacker's
    // level, times the defender's damage taken, at least 1.
    Player attacker, defender;
    attacker.stats.strength = 0;  // the roll alone
    constexpr Attack kEight{8, 8, 1};
    CHECK(resolveHit(attacker, defender, kEight, {rng}) == 8);
    attacker.inv.equipped[SlotHands] = kGloves;
    CHECK(resolveHit(attacker, defender, kEight, {rng}) == 12);  // x 1.5; the attacker's own armor doesn't matter
    for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) defender.inv.equipped[itemType(id).slot] = id;
    attacker.level = 2;
    CHECK(resolveHit(attacker, defender, kEight, {rng}) == 8);  // 12 x (1 - 37 / 117) = 8.2
    CHECK(resolveHit(attacker, defender, kEight, {rng, 80}) == 10);  // a bigger K: 12 x (1 - 37 / 197) = 9.7
    defender.inv.equipped[SlotOffHand] = kWard;
    CHECK(resolveHit(attacker, defender, kEight, {rng}) == 4);  // 8.2 x 0.5
    attacker.level = 1;
    CHECK(resolveHit(attacker, defender, kEight, {rng}) == 3);  // 12 x (1 - 37 / 77) x 0.5 = 3.1: a lower level gets through less
    CHECK(resolveHit(attacker, defender, Attack{1, 1, 1}, {rng}) == 1);  // 0.4, but a hit does at least 1
    CHECK(resolveHit(attacker, defender, Attack{0, 0, 1}, {rng}) == 0);  // no damage rolled, no hit
    setDefs(std::move(shipped));
}

// World files: a round trip is bit-exact; truncated or corrupted files are rejected cleanly (the
// dev preset's sanitizers check "cleanly"), and unknown sections are skipped.
static void worldFiles() {
    std::optional<std::vector<Marker>> fileMarks;  // the files below have a marker section
    Grid g({64, 96, 32});
    generateTerrain(g, 3);
    g.set({5, 90, 5}, Plate);  // blocks generation never places, ids past 32 too
    g.set({6, 90, 5}, Lantern);
    g.set({7, 90, 5}, Slate);
    std::string path = (std::filesystem::temp_directory_path() / "flycoos_core_test.flyw").string();
    CHECK(saveWorld(path.c_str(), g, 99, {}, {}).empty());
    Grid back({32, 32, 32});
    uint32_t seed = 0;
    std::vector<Instance> none;
    CHECK(loadWorld(path.c_str(), back, seed, none, fileMarks).empty());
    CHECK(back.size == g.size && back.cells == g.cells && seed == 99 && back.get({6, 90, 5}) == Lantern);

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

// Trees (ADR-029): each species, at both ends of its full-grown height range and 5 seeds, has its
// trunk width, height and crown width, its bark and leaves, no stray leaves, one leaf shade per row
// that never gets darker going up, and draws only inside shapeBox.
static void trees() {
    struct Want { Species s; int lo, hi, trunkLo, trunkHi, crownLo, crownHi, limbs; Block bark; Leaf leaf; };
    constexpr Want kWant[] = {
        {Species::Oak, 20, 28, 4, 5, 20, 26, 3, Wood, Leaf::Oak},
        {Species::Maple, 20, 28, 4, 5, 20, 26, 3, Wood, Leaf::MapleRed},
        {Species::Birch, 18, 24, 2, 3, 12, 16, 0, BirchBark, Leaf::Birch},
        {Species::Conifer, 26, 38, 3, 3, 12, 16, 0, PineBark, Leaf::Pine},
        {Species::Araucaria, 32, 44, 3, 4, 22, 28, 10, AraucariaBark, Leaf::Araucaria},
    };
    const glm::ivec3 at{48, 10, 48};
    Grid ground({96, 80, 96});
    for (int y = 0; y <= at.y; ++y)
        for (int z = 0; z < 96; ++z)
            for (int x = 0; x < 96; ++x) ground.set({x, y, z}, y == at.y ? Grass : Stone);
    for (const Want& w : kWant)
        for (int height : {w.lo, w.hi})
            for (uint32_t seed = 1; seed <= 5; ++seed) {
                TreeShape t = planTree(w.s, height, 0, 0, seed);
                Grid g = ground;
                growShape(g, at, t, seed);
                Box box = shapeBox(t, at);
                int top = 0;
                std::array<int, 13> slice{};  // wood per z row of the trunk, 5 above the ground: the widest is its width
                glm::ivec3 lo(INT_MAX), hi(INT_MIN);
                std::vector<int> rowShade(80, -1);
                bool inBox = true, kinds = true, strays = false, rows = true;
                for (int y = 0; y < 80; ++y)
                    for (int z = 0; z < 96; ++z)
                        for (int x = 0; x < 96; ++x) {
                            glm::ivec3 p{x, y, z};
                            Block b = g.get(p);
                            if (b == ground.get(p)) continue;
                            inBox &= glm::all(glm::greaterThanEqual(p, box.lo)) && glm::all(glm::lessThanEqual(p, box.hi));
                            if (!isLeaf(b)) {
                                kinds &= b == w.bark;
                                if (y == at.y + 5 && std::abs(x - at.x) <= 6 && std::abs(z - at.z) <= 6) ++slice[size_t(z - at.z + 6)];
                                continue;
                            }
                            kinds &= kLeafOf[b].kind == w.leaf;
                            top = std::max(top, y - at.y);
                            lo = glm::min(lo, p), hi = glm::max(hi, p);
                            strays |= std::ranges::none_of(std::array{glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0),
                                                                      glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)},
                                                           [&](glm::ivec3 f) { return isOpaque(g.get(p + f)); });
                            int shade = kLeafOf[b].shade;
                            rows &= rowShade[size_t(y)] < 0 || rowShade[size_t(y)] == shade;
                            rowShade[size_t(y)] = shade;
                        }
                int last = 0;
                for (int shade : rowShade) rows &= shade < 0 || shade >= last, last = std::max(last, shade);
                int crown = std::max(hi.x - lo.x, hi.z - lo.z) + 1, trunk = std::ranges::max(slice);
                bool ok = inBox && kinds && !strays && rows && std::abs(top - height) <= 2 && trunk >= w.trunkLo && trunk <= w.trunkHi &&
                          crown >= int(float(w.crownLo) * 0.85f) && crown <= int(float(w.crownHi) * 1.15f) + 1 && t.limbs >= w.limbs;
                if (!ok)
                    std::println("tree {} h{} seed {}: box {} kinds {} strays {} rows {} top {} trunk {} crown {} limbs {}", int(w.s), height, seed,
                                 inBox, kinds, strays, rows, top, trunk, crown, t.limbs);
                CHECK(ok);
            }
    for (float radius : {1.0f, 2.5f, 4.0f}) {  // bushes: oak leaves, no strays, inside their box
        TreeShape t = planBush(radius, 9);
        Grid g = ground;
        growShape(g, at, t, 9);
        Box box = shapeBox(t, at);
        int leaves = 0;
        for (int y = at.y + 1; y < 30; ++y)
            for (int z = 30; z < 66; ++z)
                for (int x = 30; x < 66; ++x)
                    if (Block b = g.get({x, y, z}); b != Air) {
                        ++leaves;
                        CHECK(isLeaf(b) && kLeafOf[b].kind == Leaf::Oak && x >= box.lo.x && x <= box.hi.x && y <= box.hi.y && z >= box.lo.z && z <= box.hi.z);
                    }
        CHECK(leaves > 0);
    }
}

// Instance cells: INS2 run-length codes cells in box order (prefab placements) and keeps any other
// list as is; both round-trip exactly, and old INST files still load.
static void instanceCells() {
    Grid g({32, 32, 32});
    std::vector<Instance> list(2);
    list[0] = {.id = 3, .prefab = "bush", .pos = {9, 4, 9}, .seed = 7, .params = {2}};
    for (int y = 5; y < 12; ++y)  // a blob in box order, as placePrefab records it
        for (int z = 4; z < 15; ++z)
            for (int x = 4; x < 15; ++x)
                if ((x - 9) * (x - 9) + (y - 8) * (y - 8) * 2 + (z - 9) * (z - 9) <= 25)
                    list[0].cells.push_back({uint16_t(x), uint16_t(y), uint16_t(z), Air, y > 9 ? leafBlock(Leaf::Oak, 3) : Leaves});
    list[0].cells.push_back({20, 20, 20, Stone, Wood});  // a far cell in the same box
    list[1] = {.id = 9, .prefab = "tree", .pos = {2, 2, 2}, .params = {10, 4.5f},
               .cells = {{5, 5, 5, Air, Wood}, {1, 1, 1, Dirt, Wood}, {6, 5, 5, Grass, Grass}}};  // out of order, a no-op
    std::string path = (std::filesystem::temp_directory_path() / "flycoos_cells_test.flyw").string();
    auto roundTrip = [&](const std::vector<Instance>& in) {
        std::vector<Instance> back;
        std::vector<Section> extra;
        uint32_t seed = 0;
        Grid h({32, 32, 32});
        std::string error = readWorldFile(path.c_str(), h, seed, back, extra);
        CHECK(error.empty());
        CHECK(back.size() == in.size());
        for (size_t i = 0; i < std::min(back.size(), in.size()); ++i)
            CHECK(back[i].id == in[i].id && back[i].prefab == in[i].prefab && back[i].params == in[i].params &&
                  std::ranges::equal(back[i].cells, in[i].cells, [](auto& a, auto& b) {
                      return a.x == b.x && a.y == b.y && a.z == b.z && a.before == b.before && a.after == b.after;
                  }));
    };
    CHECK(writeWorldFile(path.c_str(), g, 1, list, {}).empty());
    roundTrip(list);
    std::ifstream in(path, std::ios::binary);
    std::string file((std::istreambuf_iterator<char>(in)), {});
    in.close();
    size_t at = file.find("INS2");
    CHECK(at != std::string::npos && file.find("INST") == std::string::npos);
    uint64_t payload = 0;
    for (int i = 0; i < 8; ++i) payload |= uint64_t(uint8_t(file[at + 4 + size_t(i)])) << (8 * i);
    CHECK(list[0].cells.size() > 300 && payload * 5 < list[0].cells.size() * 8);  // INST: 8 bytes a cell

    // The same instances in the old INST form (form bytes dropped), checksum recomputed.
    std::vector<Instance> raw = {list[1]};
    CHECK(writeWorldFile(path.c_str(), g, 1, raw, {}).empty());
    std::ifstream again(path, std::ios::binary);
    file.assign((std::istreambuf_iterator<char>(again)), {});
    again.close();
    at = file.find("INS2");
    size_t form = at + 12 + 4 + 4 + 1 + 4 + 12 + 1 + 4 + 1 + 4 * 2;  // header, count, id, name length, name, pos, rotation, seed, params
    CHECK(file[form] == 0);
    file.erase(form, 1);
    file.replace(at, 4, "INST");
    file[at + 4] = char(uint8_t(file[at + 4]) - 1);
    uint32_t h = fnv1a(reinterpret_cast<const uint8_t*>(file.data()), file.size() - 4);
    for (int i = 0; i < 4; ++i) file[file.size() - 4 + size_t(i)] = char(h >> (8 * i));
    std::ofstream(path, std::ios::binary | std::ios::trunc) << file;
    roundTrip(raw);
    std::filesystem::remove(path);
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
    if (op.tool == Tool::Plant) op.block = std::array{Air, ShortGrass, TallGrass, FlowerRed}[size_t(pick(0, 3))];
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
    {  // under trees and structures the brushes keep the natural ground's surface: no spread leaves or roofs
        Grid g({32, 32, 32});
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 32; ++x)
                for (int y = 0; y <= 5; ++y) g.set({x, y, z}, y == 5 ? Grass : Dirt);
        growShape(g, {16, 5, 16}, planTree(Species::Oak, 10, 4.5f, 0, 3), 3);
        for (int y = 6; y <= 8; ++y) g.set({10, y, 16}, y == 8 ? Thatch : Planks);  // a hut's wall and roof
        CHECK(isLeaf(g.get({16, groundTop(g, 16, 16), 16})) && groundTop(g, 16, 16) > 12);
        EditHistory h;
        Grid lowered = g;  // lowering into a canopy leaves it a canopy (no grass up in the tree)
        CHECK(!applyEdit(lowered, {.tool = Tool::Lower, .center = {16, 16}, .radius = 0, .strength = 2}, h).dirty.empty());
        CHECK(isLeaf(lowered.get({16, groundTop(lowered, 16, 16), 16})));
        Grid raised = g;  // raising under a tree raises the canopy (no ground stacked on the leaves)
        int canopy = groundTop(g, 16, 16);
        CHECK(!applyEdit(raised, {.tool = Tool::Raise, .center = {16, 16}, .radius = 0, .strength = 3}, h).dirty.empty());
        CHECK(groundTop(raised, 16, 16) == canopy + 3);
        for (int y = 6; y <= canopy + 3; ++y) CHECK(raised.get({16, y, 16}) != Grass && raised.get({16, y, 16}) != Dirt);
        Grid road = g;  // a Cobble road painted on the grass stays Cobble when sculpted, down or up
        for (int x = 2; x <= 8; ++x) road.set({x, 5, 2}, Cobble);
        CHECK(!applyEdit(road, {.tool = Tool::Lower, .center = {4, 2}, .radius = 0, .strength = 2}, h).dirty.empty());
        CHECK(groundTop(road, 4, 2) == 3 && road.get({4, 3, 2}) == Cobble);
        CHECK(!applyEdit(road, {.tool = Tool::Raise, .center = {7, 2}, .radius = 0, .strength = 2}, h).dirty.empty());
        CHECK(groundTop(road, 7, 2) == 7 && road.get({7, 7, 2}) == Cobble);
        CHECK(!applyEdit(g, {.tool = Tool::Flatten, .center = {16, 16}, .radius = 10, .strength = 64, .height = 4}, h).dirty.empty());
        int spread = 0;
        for (int z = 6; z <= 26; ++z)
            for (int x = 6; x <= 26; ++x) {
                if ((x - 16) * (x - 16) + (z - 16) * (z - 16) > 100) continue;
                CHECK(groundTop(g, x, z) == 4 && g.get({x, 4, z}) == Grass);
                for (int y = 0; y < 32; ++y) spread += g.get({x, y, z}) != Grass && g.get({x, y, z}) != Dirt && g.get({x, y, z}) != Air;
            }
        CHECK(spread == 0);
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
    // catalog: what the MCP bridge builds its schemas from, read from the loaded defs
    c = run(R"({"cmd": "catalog"})");
    const nlohmann::json& cat = c.response["result"];
    CHECK(ok(c) && cat["entities"].size() == size_t(EntityKind::Count) && cat["entities"][6]["id"] == "bandit_chief" &&
          cat["entities"][6]["loot"] == "Iron Helm" && cat["entities"][6]["temper"] == "aggressive" && cat["entities"][1]["name"].is_null() &&
          cat["entities"][1]["wander_radius"] == 12 && cat["materials"].size() == BlockCount && cat["solid_materials"].size() == BlockCount - 8 &&
          cat["materials"][Ember] == "Ember" && cat["materials"][Lantern] == "Lantern" &&
          std::ranges::count(cat["solid_materials"], "Slate") == 1);  // every block; solid: all but Air and Water

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
    // Every shipped file loads without errors, after the 8 generated prefabs.
    auto shipped = std::ranges::count_if(std::filesystem::directory_iterator(prefabs.dir), [](const auto& f) { return f.path().extension() == ".txt"; });
    CHECK(ok(c) && c.response["result"]["prefabs"].size() == size_t(8 + shipped) && shipped >= 6 && c.response["result"]["errors"].empty());
    for (size_t i = 0; ok(c) && i < c.response["result"]["prefabs"].size(); ++i)
        CHECK(c.response["result"]["prefabs"][i].contains(i < 8 ? "params" : "size"));
    CHECK(c.response["result"]["prefabs"][0]["params"][0]["name"] == "height" && c.response["result"]["prefabs"][8].contains("size"));
    c = run(R"({"cmd": "place", "args": {"prefab": "cottage", "x": 30, "z": 30, "rotation": 90}})");
    CHECK(ok(c) && c.response["result"]["instance"]["id"] == 1 && c.response["result"]["instance"]["rotation"] == 90 && !c.dirty.empty());
    c = run(R"({"cmd": "place", "args": {"prefab": "tree", "x": 12, "z": 50, "y": 60, "seed": 4000000000, "params": {"height": 13.4, "crown": 3.5}}})");
    CHECK(ok(c) && c.response["result"]["instance"]["params"]["height"] == 13 && c.response["result"]["instance"]["seed"] == 4000000000u &&
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
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "params": {"height": 99}}})",
             R"({"cmd": "place", "args": {"prefab": "tree", "x": 10, "z": 10, "params": {"radius": 9}}})",   // not a tree parameter
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

    {  // fill and paint take any block by name, building blocks too; paint and path still need a solid one
        Grid w({64, 16, 64});
        for (int z = 0; z < 64; ++z)
            for (int x = 0; x < 64; ++x) w.set({x, 3, z}, Grass);
        EditHistory wh;
        std::vector<Instance> wi;
        std::vector<Marker> wm;
        auto send = [&](std::string_view request) { return runCommand(request, w, wi, wm, wh, prefabs, view); };
        CHECK(ok(send(R"({"cmd": "fill", "args": {"min": [2, 4, 4], "max": [3, 4, 4], "material": "ember"}})")) &&
              w.get({2, 4, 4}) == Ember && w.get({3, 4, 4}) == Ember);
        CHECK(ok(send(R"({"cmd": "paint", "args": {"x": 2, "z": 4, "radius": 0, "material": "Planks"}})")) && w.get({2, 4, 4}) == Planks);
        CHECK(ok(send(R"({"cmd": "path", "args": {"points": [[10, 10], [20, 10]], "width": 1, "material": "Cobble"}})")) &&
              w.get({15, groundTop(w, 15, 10), 10}) == Cobble);
        c = send(R"({"cmd": "paint", "args": {"x": 2, "z": 4, "radius": 0, "material": "Water"}})");
        CHECK(!ok(c) && c.response["error"].get<std::string>().contains("solid") && w.get({2, 4, 4}) == Planks);
        c = send(R"({"cmd": "fill", "args": {"min": [0, 0, 0], "max": [0, 0, 0], "material": "Lava"}})");
        CHECK(!ok(c) && c.response["error"].get<std::string>().contains("Ember") && wh.undo.size() == 3);
        // a camp brings its bandits: place lists them, markers marks them as the instance's, remove takes them along
        c = send(R"({"cmd": "place", "args": {"prefab": "camp", "x": 32, "z": 32}})");
        nlohmann::json made = c.response["result"]["markers"];
        uint32_t camp = ok(c) ? c.response["result"]["instance"]["id"].get<uint32_t>() : 0;
        CHECK(ok(c) && made.size() == 1 && made[0]["entity"] == "bandit" && made[0]["count"] == 2 && made[0]["instance"] == camp &&
              wm.size() == 1 && wm[0].owner == camp && wh.undo.size() == 4);
        c = send(R"({"cmd": "markers"})");
        CHECK(ok(c) && c.response["result"]["markers"][0]["instance"] == camp);
        c = send(std::format(R"({{"cmd": "move", "args": {{"id": {}, "x": 30, "z": 34}}}})", camp));
        CHECK(ok(c) && wm.size() == 1 && wm[0].pos.x == made[0]["x"].get<float>() - 2 + 0.5f && wm[0].pos.z == made[0]["z"].get<float>() + 2 + 0.5f);
        c = send(std::format(R"({{"cmd": "remove", "args": {{"id": {}}}}})", camp));
        CHECK(ok(c) && c.response["result"]["markers_removed"] == 1 && wm.empty() && wh.undo.size() == 6);
    }
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
        std::string wide = "S = Stone\nlayer\n" + std::string(65, 'S') + "\n", tall = "S = Stone\n", nine, crowd;
        for (int i = 0; i < 9; ++i) nine += "line: Halt!\n";
        for (int i = 0; i < 33; ++i) crowd += "marker: spawn x=0 z=0 entity=bandit count=8\n";  // 264 NPCs
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
            {"S = Stone\nmarker: spawn x=1 z=0 entity=bandit\nlayer\nS\n", "line 2: marker cell 1 0 is outside the layers (1 x 1)"},
            {"marker: player_spawn x=0 z=0\n", "line 1: marker kind must be spawn, poi or region"},
            {"marker: spawn x=0 z=0\n", "line 1: a spawn marker needs entity="},
            {"marker: spawn z=0 entity=bandit\n", "line 1: a marker needs x= and z="},
            {"marker: spawn x=-1 z=0 entity=bandit\n", "line 1: marker x must be a whole number"},
            {"marker: spawn x=0 z=0 entity=dragon\n", "line 1: unknown entity 'dragon'"},
            {"marker: region x=0 z=0 colour=red\n", "line 1: unknown marker field 'colour'"},
            {"marker: poi x=0 z=0 entity=boar\n", "line 1: entity and count are for spawn markers"},
            {"marker: poi x=0 z 0\n", "line 1: expected key=value, not 'z'"},
            {"line: hi\n", "line 1: a line belongs to the marker above it"},
            {"S = Stone\nmarker: poi x=0 z=0\nlayer\nS\n", "line 2: marker: a poi or region needs a name"},
            {"S = Stone\nmarker: spawn x=0 z=0 entity=bandit count=9\nlayer\nS\n", "line 2: marker: count out of range"},
            {"S = Stone\nmarker: spawn x=0 z=0 entity=dummy\nline: hi\nlayer\nS\n", "line 3: line: lines are for spawn markers"},
            {"marker: spawn x=0 z=0 entity=bandit\nline: hi\nline:\n", "line 3: line: each line needs 1..160 bytes"},
            {"marker: spawn x=0 z=0 entity=bandit\n" + nine, "line 10: line: more than 8 lines"},
            {crowd, "line 33: the spawn markers make more than 256 NPCs"},
            {"description: a\nS = Stone\ndescription: b\n", "line 3: 'description' is given twice"},
            {"y: 1\ny: 1\n", "line 2: 'y' is given twice"},
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
        // markers: a cell of the layers, the kind's default radius, name to the end of the line, lines in order
        CHECK(parsePrefab("marker: region x=1 z=0 radius=40 name= Old  Field \nmarker: spawn x=0 z=0 entity=boar count=2\n"
                          "line: Grr!\nline: Snort.\nS = Stone\nlayer\nSS\n", p).empty() && p.markers.size() == 2);
        CHECK(p.markers[0].kind == MarkerKind::Region && p.markers[0].name == "Old  Field" && p.markers[0].radius == 40 &&
              p.markers[0].pos == glm::vec3(1.5f, 0, 0.5f) && p.markers[1].entity == EntityKind::Boar && p.markers[1].count == 2 &&
              p.markers[1].radius == typeOf(EntityKind::Boar).wanderRadius && (p.markers[1].lines == std::vector<std::string>{"Grr!", "Snort."}));
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
        CHECK((names == std::vector<std::string>{"tree", "pine", "bush", "boulder", "spire", "maple", "birch", "araucaria", "box"}));
        CHECK(lib.errors.size() == 3 && lib.errors[0] == "broken.txt: line 3: 'Q' is not in the legend" &&
              lib.errors[1].starts_with("huge.txt") && lib.errors[2].starts_with("tree.txt"));
        fs::remove_all(dir);
        CHECK(loadPrefabs(dir).errors.size() == 1);  // a missing folder is reported, the generated prefabs remain
        fs::create_directories(dir);
        for (int i = 0; i < 8; ++i)  // 8 x 260 KB: the last one is over the 2 MB total
            std::ofstream(dir / std::format("big{}.txt", i)) << "#" << std::string(260 << 10, 'x') << "\nS = Stone\nlayer\nS\n";
        lib = loadPrefabs(dir);
        CHECK(lib.prefabs.size() == 8 + 7 && lib.errors.size() == 1 && lib.errors[0].starts_with("big7.txt: skipped"));
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
    {  // replant (ADR-029): an old baked tree goes, a hand-made post stays, instances and their cells stay, undo restores
        Grid g({128, 64, 128});
        for (int z = 0; z < 128; ++z)
            for (int x = 0; x < 128; ++x)
                for (int y = 0; y <= 10; ++y) g.set({x, y, z}, y == 10 ? Grass : Stone);
        for (int y = 11; y < 16; ++y) g.set({30, y, 30}, Wood);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dz = -2; dz <= 2; ++dz)
                for (int dx = -2; dx <= 2; ++dx)
                    if (g.get({30 + dx, 17 + dy, 30 + dz}) == Air) g.set({30 + dx, 17 + dy, 30 + dz}, Leaves);
        for (int y = 11; y < 14; ++y) g.set({60, y, 60}, Wood);
        std::vector<Instance> list;
        std::vector<Marker> marks;
        EditHistory h;
        const Prefab& cottage = prefabNamed(lib, "cottage");
        CHECK(!placePrefab(g, cottage, at(cottage, {90, 10, 90}), std::nullopt, list, h).error);
        const uint64_t before = hashCells(g);
        const std::vector<Instance::Cell> cells = list[0].cells;
        ReplantReport r;
        replantTrees(g, lib, 7, {0, 0}, {127, 127}, true, list, marks, h, r);
        CHECK(hashCells(g) == before && h.undo.size() == 1 && list.size() == 1 && r.removedTrees == 1 && r.keptClusters == 1);
        CHECK(!replantTrees(g, lib, 7, {0, 0}, {127, 127}, false, list, marks, h, r).error && r.removedTrees == 1 && r.keptClusters == 1);
        CHECK(h.undo.size() == 2 && g.get({60, 12, 60}) == Wood && g.get({30, 17, 30}) != Leaves);
        CHECK(std::ranges::all_of(cells, [&](const Instance::Cell& c) { return g.get({c.x, c.y, c.z}) == c.after; }));
        undoEdit(g, h, list, marks);
        CHECK(hashCells(g) == before && list.size() == 1);

        // Today's trees (placed by hand or by a first replant) are left alone, so a second run changes nothing.
        const Prefab& oak = prefabNamed(lib, "tree");
        Instance young = at(oak, {60, 10, 30});
        young.params = {12, 5};
        CHECK(!placePrefab(g, oak, young, std::nullopt, list, h).error);
        replantTrees(g, lib, 7, {0, 0}, {127, 127}, false, list, marks, h, r);
        const uint64_t once = hashCells(g);
        const size_t steps = h.undo.size(), count = list.size();
        CHECK(r.regrown == 0 && std::ranges::any_of(list, [](const Instance& in) { return in.params == std::vector<float>{12, 5}; }));
        replantTrees(g, lib, 7, {0, 0}, {127, 127}, false, list, marks, h, r);
        CHECK(hashCells(g) == once && h.undo.size() == steps && list.size() == count && r.regrown == 0 && r.removedTrees == 0 && r.planted == 0);
    }
    {  // replant regrows hundreds of old instances in one step that undo restores exactly (no undo cap drops any)
        Grid g({128, 32, 128});
        for (int z = 0; z < 128; ++z)
            for (int x = 0; x < 128; ++x)
                for (int y = 0; y <= 10; ++y) g.set({x, y, z}, y == 10 ? Grass : Stone);
        std::vector<Instance> list;
        std::vector<Marker> marks;
        EditHistory h;
        for (int i = 0; i < 300; ++i) {  // old-style bushes: one Leaves voxel each, as a file would hold them
            glm::ivec3 p{4 + i % 20 * 6, 10, 4 + i / 20 * 6};
            g.set(p + glm::ivec3(0, 1, 0), Leaves);
            list.push_back({.id = uint32_t(i + 1), .prefab = "bush", .pos = p, .seed = uint32_t(i), .params = {1},
                            .cells = {{uint16_t(p.x), uint16_t(p.y + 1), uint16_t(p.z), Air, Leaves}}});
        }
        const uint64_t before = hashCells(g);
        ReplantReport r;
        replantTrees(g, lib, 3, {0, 0}, {127, 127}, false, list, marks, h, r);
        CHECK(r.regrown == 300 && h.undo.size() == 1 && hashCells(g) != before);
        undoEdit(g, h, list, marks);
        CHECK(hashCells(g) == before && list.size() == 300 && list[0].cells.size() == 1);
    }
    {  // placement checks
        Grid g = plain();
        std::vector<Instance> list;
        EditHistory h;
        const Prefab& tree = prefabNamed(lib, "tree");
        Instance in = at(tree, {8, 0, 8});
        in.params[0] = 41;
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
        size_t inst = file.find("INS2");
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
    {  // prefab markers: a camp brings its bandits, owned by the instance; they turn and move with it, go with it,
       // and every step is one undo step; hand-placed markers are never touched; files keep who owns what
        Grid g = plain();
        std::vector<Instance> list;
        std::vector<Marker> ms;
        EditHistory h;
        const Prefab& camp = prefabNamed(lib, "camp");
        CHECK(camp.markers.size() == 1 && camp.markers[0].kind == MarkerKind::Spawn && camp.markers[0].entity == EntityKind::Bandit &&
              camp.markers[0].count == 2 && camp.markers[0].lines.size() == 1);
        const Marker hand{.kind = MarkerKind::PlayerSpawn, .pos = {5.5f, 4, 5.5f}};
        CHECK(!putMarker(g, ms, h, hand).error && ms.size() == 1);
        const glm::ivec3 s = camp.size;
        // where layer cell (11, 12) lands: cells turn like rotate() (pinned by the rotation test), feet on the bottom layer
        auto expect = [&](glm::ivec3 pos, int rotation) {
            glm::ivec2 c = rotation == 0 ? glm::ivec2(11, 12) : glm::ivec2(12, s.x - 1 - 11);  // rotation 1: (x, z) -> (z, s.x - 1 - x)
            glm::ivec2 half = rotation == 0 ? glm::ivec2(s.x / 2, s.z / 2) : glm::ivec2(s.z / 2, s.x / 2);
            return glm::vec3(float(pos.x - half.x + c.x) + 0.5f, float(pos.y + camp.y + 2), float(pos.z - half.y + c.y) + 0.5f);
        };
        uint64_t empty = hashCells(g);
        EditResult r = placePrefab(g, camp, at(camp, {30, 0, 30}), std::nullopt, list, h, false, &ms);
        CHECK(!r.error && ms.size() == 2 && ms[1].owner == r.id && ms[1].id == 2 && ms[1].count == 2 && ms[1].lines == camp.markers[0].lines &&
              ms[1].pos == expect({30, 3, 30}, 0) && h.undo.size() == 2 && ms[0].owner == 0);
        Marker renamed = ms[1];
        renamed.name = "Raiders";
        renamed.owner = 0;  // putMarker keeps the owner: only the instance's own steps change it
        CHECK(!putMarker(g, ms, h, renamed).error && ms[1].owner == r.id && ms[1].name == "Raiders" && h.undo.size() == 3);
        CHECK(!movePrefab(g, camp, r.id, {34, 33}, std::nullopt, 1, list, h, &ms).error && h.undo.size() == 4);
        const Marker moved = ms[1];
        CHECK(moved.pos == expect({34, 3, 33}, 1) && moved.name == "Raiders" && ms[0].pos == hand.pos);
        const uint64_t movedCells = hashCells(g);
        CHECK(!undoEdit(g, h, list, ms).empty() && ms[1].pos == expect({30, 3, 30}, 0) && list[0].rotation == 0);
        CHECK(!redoEdit(g, h, list, ms).empty() && ms[1] == moved && hashCells(g) == movedCells);
        CHECK(!removePrefab(g, r.id, list, h, &ms).error && list.empty() && ms.size() == 1 && ms[0].id == 1 && h.undo.size() == 5);
        CHECK(!undoEdit(g, h, list, ms).empty() && list.size() == 1 && ms.size() == 2 && ms[1] == moved);

        std::string path = (fs::temp_directory_path() / "flycoos_prefab_markers.flyw").string();
        CHECK(saveWorld(path.c_str(), g, 5, list, ms).empty());
        Grid back({64, 32, 64});
        uint32_t seed = 0;
        std::vector<Instance> loaded;
        CHECK(loadWorld(path.c_str(), back, seed, loaded, fileMarks).empty() && fileMarks && *fileMarks == ms && loaded.size() == 1);
        EditHistory bh;
        CHECK(!removePrefab(back, r.id, loaded, bh, &*fileMarks).error && fileMarks->size() == 1);  // a loaded camp still takes its bandits
        std::ifstream saved(path, std::ios::binary);
        const std::string file((std::istreambuf_iterator<char>(saved)), {});
        size_t owners = file.rfind("OWNR") + 12;  // past the tag and size: count, marker id, instance id
        auto patched = [&](size_t at, uint32_t v) {  // the file with one u32 replaced, resealed
            std::string bytes = file;
            for (int i = 0; i < 4; ++i) bytes[at + size_t(i)] = char(v >> (8 * i));
            uint32_t hash = 2166136261u;
            for (size_t i = 0; i + 4 < bytes.size(); ++i) hash = (hash ^ uint8_t(bytes[i])) * 16777619u;
            for (int i = 0; i < 4; ++i) bytes[bytes.size() - 4 + size_t(i)] = char(hash >> (8 * i));
            std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
            return loadWorld(path.c_str(), back, seed, loaded, fileMarks);
        };
        CHECK(owners > 12 && patched(owners + 4, 2).empty());  // unchanged: loads
        CHECK(patched(owners + 8, 99).starts_with("bad owner for marker 2"));  // not an instance
        CHECK(patched(owners + 4, 1).starts_with("bad owner for marker 1"));   // the player_spawn
        CHECK(patched(owners + 4, 3).starts_with("bad owner for marker 3"));   // no such marker
        CHECK(patched(owners, 2).starts_with("marker owners section size mismatch"));
        CHECK(saveWorld(path.c_str(), g, 5, {}, ms).empty());  // an owner that isn't live: saved as hand-placed
        CHECK(loadWorld(path.c_str(), back, seed, loaded, fileMarks).empty() && fileMarks->size() == 2 && (*fileMarks)[1].owner == 0);
        CHECK(saveWorld(path.c_str(), g, 5, list, {hand}).empty());  // nothing owned: no section, so older builds see the same file as before
        std::ifstream noOwners(path, std::ios::binary);
        CHECK(std::string((std::istreambuf_iterator<char>(noOwners)), {}).find("OWNR") == std::string::npos);
        fs::remove(path);

        for (int i = 0; i < 3; ++i) undoEdit(g, h, list, ms);  // move, rename, place (the remove is undone already)
        CHECK(list.empty() && ms.size() == 1 && ms[0].owner == 0 && hashCells(g) == empty);
        std::vector<Marker> crowd;  // 256 NPCs already: the camp's two bandits don't fit, and nothing changes
        for (uint32_t i = 1; i <= 32; ++i)
            crowd.push_back({.id = i, .kind = MarkerKind::Spawn, .pos = {float(i) + 0.5f, 4, 2.5f}, .entity = EntityKind::Bandit, .count = 8});
        size_t undos = h.undo.size();
        CHECK(placePrefab(g, camp, at(camp, {30, 0, 30}), std::nullopt, list, h, false, &crowd).error && list.empty() &&
              crowd.size() == 32 && hashCells(g) == empty && h.undo.size() == undos);
        CHECK(!placePrefab(g, camp, at(camp, {30, 0, 30}), std::nullopt, list, h).error && list.size() == 1);  // no list: no markers
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
    auto sync = [&] { syncSpawns(npcs, g, list, 1, {16, 4, 16}, 0.5f); };
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
    {  // one spawned in front of the player (F6) stands 5 blocks the way it faces, and syncs leave it be
        Player me;
        me.pos = {16.5f, 4, 16.5f};
        me.yaw = std::numbers::pi_v<float> / 2;  // facing +X
        const Entity& b = spawnAhead(npcs, g, me, EntityKind::Bandit, 7);
        CHECK(glm::distance(b.body.pos, glm::vec3(21.5f, 4, 16.5f)) < 1e-4f && !overlapsSolid(g, b.body.pos));
        CHECK(b.spawn.id == 0 && std::abs(b.body.yaw + std::numbers::pi_v<float> / 2) < 1e-4f);  // faces the player
        sync();
        CHECK(npcs.size() == 4 && npcs[3].kind == EntityKind::Bandit);
        npcs.pop_back();
    }
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
    syncSpawns(after, w, defaults, 9, findSpawn(w), 0.5f);
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
        syncSpawns(es, t, ms, 1, p, 0.5f);
        CHECK(es.size() == 1 && es[0].body.pos.y == 9 && !overlapsSolid(t, es[0].body.pos));
        for (int y = 9; y < 12; ++y) t.set({10, y, 10}, Stone);  // built over since: freed on the next update
        Player far;
        far.pos = {60, 4, 60};
        updateEntities(es, t, far, 1 / 60.0f);
        CHECK(!overlapsSolid(t, es[0].body.pos));
        ms[1].name = "Renamed";
        syncSpawns(es, t, ms, 1, p, 0.5f);
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
        if (r.temper == Temper::Aggressive) CHECK(t.aggroRange > 0 && t.aggroRange < t.reaggroRadius && t.chaseSpeed > 0);
    }
    CHECK(typeOf(EntityKind::Wolf).chaseSpeed > 5 && typeOf(EntityKind::Ghoul).chaseSpeed < 5);  // the player walks 5, runs 9
    CHECK(typeOf(EntityKind::BanditChief).lootChance == 1);
    CHECK(std::string_view(kEntityKindNames[int(EntityKind::BanditChief)]) == "bandit_chief");
    CHECK(itemType(WolfPelt).slot == SlotNone && itemType(BoneCharm).slot == SlotTrinket && std::string_view(itemType(BoneCharm).name) == "Bone Charm");
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

    // Fights: a level-1 player in the whole Iron set (armor 37) with the sword stands still, facing
    // the nearest foe, and plays it the way Elden Ring teaches (ADR-035): guards when a foe in front is
    // about to strike, attacks when no strike can land before its light does (while there's stamina
    // to), ripostes when a stance breaks. Foes
    // come from a spawn marker 6 blocks away. Returns the player's health left, or minus the foes'
    // health left when the player goes down.
    Grid g = meadow();
    auto fight = [&](EntityKind kind, int count, bool geared, uint32_t seed) {
        Player player;
        player.pos = {20.5f, 4, 32.5f};
        player.onGround = true, player.offGround = 0;  // standing (no physics here)
        if (geared)
            for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, Shortsword}) {
                giveItem(player.inv, id);
                quickMove(player.inv, {SlotRef::Item, 0, 0});
            }
        CHECK(sheet(player)[Stat::Armor] == (geared ? 37 : 0) && player.inv.equipped[SlotMainHand] == (geared ? Shortsword : NoItem));
        Marker m{.id = 1, .kind = MarkerKind::Spawn, .pos = {26.5f, 4, 32.5f}, .radius = 3, .entity = kind, .count = count};
        std::vector<Entity> es;
        syncSpawns(es, g, {m}, seed, player.pos, 0.5f);
        std::mt19937 rng(seed);
        for (int i = 0; i < 120 * 60; ++i) {
            updateEntities(es, g, player, 1 / 60.0f);
            separate(player, g, es);
            auto alive = es | std::views::filter([](const Entity& e) { return e.body.alive(); });
            auto flat = [&](const Entity& e) { return glm::vec2(e.body.pos.x - player.pos.x, e.body.pos.z - player.pos.z); };
            auto foe = std::ranges::min_element(alive, {}, [&](const Entity& e) { return glm::length(flat(e)); });
            if (foe == alive.end()) return player.health;
            glm::vec2 to = glm::normalize(flat(*foe));
            if (!player.punching()) player.yaw = std::atan2(to.x, to.y);
            auto due = [&](const Entity& e) {  // when a foe in reach strikes next (99: it isn't winding up)
                if (e.state != AiState::Windup || glm::length(flat(e)) > kPunchReach + 0.5f) return 99.0f;
                if (!e.body.hitPending) return e.string > 1 ? 0.25f : 99.0f;  // between the hits of a string: the next is coming
                return e.body.hold + std::max(0.0f, impactTime(e.body) - e.body.moveTime);
            };
            MeleeInput melee{.aim = to};
            melee.guard = !player.punching() && std::ranges::any_of(alive, [&](const Entity& e) {  // a guard only helps in front
                return due(e) < 0.3f && glm::dot(glm::normalize(flat(e)), to) >= gMelee.guardAngle;
            });
            bool safe = std::ranges::none_of(alive, [&](const Entity& e) { return due(e) < 0.35f; });  // a light lands first
            melee.attack = safe && glm::length(flat(*foe)) < kPunchReach - 0.1f && player.stamina > 15;
            if (Entity* broken = riposteTarget(es, player)) melee.riposte = true, melee.aim = flat(*broken);
            Input still;
            updateMelee(player, melee, still, 1 / 60.0f);
            updateCombat(player, es, 1 / 60.0f, rng);
            if (!player.alive()) return -std::ranges::fold_left(es, 0.0f, [](float n, const Entity& e) { return n + e.body.health; });
        }
        return 999.0f;  // nobody won in two minutes
    };
    // Twelve seeds. Since enemies answer attacks by chance (guards, parries, dodges: ADR-043) one fight
    // can go either way, as a close fight does; so at least 10 of the 12 go as tuned, and the means
    // of the health left are what's tuned.
    constexpr int kSeeds = 12;
    float chiefSum = 0, wolvesSum = 0, ghoulsSum = 0;
    int chiefAsTuned = 0, wolvesAsTuned = 0, ghoulsAsTuned = 0;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
        float chief = fight(EntityKind::BanditChief, 1, true, seed), wolves = fight(EntityKind::Wolf, 2, true, seed);
        float ghouls = fight(EntityKind::Ghoul, 2, true, seed), bare = fight(EntityKind::BanditChief, 1, false, seed);
        chiefAsTuned += chief > 0 && chief < 50;     // the chief alone: won, never easily
        wolvesAsTuned += wolves > 0 && wolves < 60;  // a pair of wolves
        ghoulsAsTuned += ghouls > 0 && ghouls < 60;  // a pair of ghouls
        CHECK(bare < 0);                             // bare-handed and unarmored, the chief always wins
        if (!(chief > 0 && chief < 50 && wolves > 0 && wolves < 60 && ghouls > 0 && ghouls < 60))
            std::println("story fight seed {} off its band: chief {:.0f} wolves {:.0f} ghouls {:.0f}", seed, chief, wolves, ghouls);
        chiefSum += chief, wolvesSum += wolves, ghoulsSum += ghouls;
    }
    CHECK(chiefAsTuned >= 10 && wolvesAsTuned >= 10 && ghoulsAsTuned >= 10);
    float chiefMean = chiefSum / kSeeds, wolvesMean = wolvesSum / kSeeds, ghoulsMean = ghoulsSum / kSeeds;
    std::println("story fights, health left on average: chief {:.0f}, wolves {:.0f}, ghouls {:.0f}", chiefMean, wolvesMean, ghoulsMean);
    CHECK(chiefMean >= 20 && chiefMean <= 30);                           // about a quarter
    CHECK(wolvesMean >= 25 && wolvesMean <= 45 && ghoulsMean >= 20 && ghoulsMean <= 40);  // a quarter to two fifths
    CHECK(fight(EntityKind::Wolf, 3, true, 1) < 0);  // a pack of three is too much alone
    // Their hits on that player: punch or sword + strength x speed, less armor / (armor + 40 x level).
    std::mt19937 rng(3);
    Player armored;
    for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) armored.inv.equipped[itemType(id).slot] = id;
    for (auto [kind, lo, hi] : {std::tuple{EntityKind::Wolf, 4, 5}, std::tuple{EntityKind::Ghoul, 4, 6}, std::tuple{EntityKind::BanditChief, 7, 10}}) {
        const EntityType& t = typeOf(kind);
        Player foe;
        foe.stats = t.stats, foe.level = t.level;
        int least = 99, most = 0;
        for (int i = 0; i < 500; ++i) {
            int d = resolveHit(foe, armored, weaponAttack(t.weapon), {rng});
            least = std::min(least, d), most = std::max(most, d);
        }
        CHECK(least == lo && most == hi);
        if (least != lo || most != hi) std::println(stderr, "  {} hits {}..{}", t.name, least, most);
    }

    // The chief of a story camp: named by its marker, taunts when it turns on the player, always drops the helm.
    Marker camp{.id = 3, .kind = MarkerKind::Spawn, .name = "Hakon the Red", .pos = {12.5f, 4, 12.5f}, .radius = 6,
                .entity = EntityKind::BanditChief, .count = 1, .lines = {"You walked into the wrong camp.", "Is that all?"}};
    CHECK(!checkMarker(g, camp));
    std::vector<Entity> es;
    syncSpawns(es, g, {camp}, 1, {20, 4, 12}, 0.5f);
    Entity& hakon = es[0];
    CHECK(hakon.displayName() == "Hakon the Red" && hakon.speech() == nullptr);
    Player player;
    player.pos = hakon.body.pos + glm::vec3(5, 0, 0);
    updateEntities(es, g, player, 1 / 60.0f);
    CHECK(hakon.angry() && hakon.speech() && std::string_view(hakon.speech()) == "You walked into the wrong camp.");
    CHECK(damage(hakon, 10000) && hakon.loot == IronHelm);
    hakon.speechTimer = 0;
    for (int i = 0; i < 3; ++i) updateEntities(es, g, player, 1 / 60.0f);
    CHECK(hakon.speech() == nullptr);  // the dead don't taunt

    // Guards: talk while calm (E), shout their next line when attacked, never talk once dead.
    Marker post{.id = 4, .kind = MarkerKind::Spawn, .name = "Gate", .pos = {30.5f, 4, 30.5f}, .radius = 4,
                .entity = EntityKind::Guard, .count = 1, .lines = {"Halt.", "Have at you!", "Move along."}};
    std::vector<Entity> gs;
    syncSpawns(gs, g, {post}, 1, {34, 4, 30}, 0.5f);
    Entity& guard = gs[0];
    player.pos = guard.body.pos + glm::vec3(2, 0, 0);
    CHECK(talkTarget(gs, player) == &guard);
    talk(guard);
    CHECK(std::string_view(guard.speech()) == "Halt.");
    CHECK(damage(guard, 5));
    updateEntities(gs, g, player, 1 / 60.0f);
    CHECK(guard.angry() && std::string_view(guard.speech()) == "Have at you!");
    // Hit from outside its territory it can't stay angry: one shout, not one per hit, and it doesn't
    // stand frozen under the balloon.
    Player far;
    far.pos = guard.home + glm::vec3(guard.type().leashRadius + 2, 0, 0);
    setState(guard, AiState::Wander, "test");
    guard.shoutTimer = 0, guard.speechTimer = 0;
    int shouts = 0;
    for (int i = 0; i < 4 * 60; ++i) {  // within the balloon's 5 s
        int before = guard.phrase;
        if (i % 30 == 0) damage(guard, 1);
        float waited = guard.wanderTimer;
        updateEntities(gs, g, far, 1 / 60.0f);
        shouts += guard.phrase != before;
        CHECK(!guard.angry() && (guard.wanderTimer < waited || guard.wanderTimer > waited + 1));  // it keeps wandering
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
    auto sync = [&] { syncSpawns(npcs, g, list, 1, {16, 4, 16}, 0.5f); };
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
    g1->events = 0;  // forget the hit
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
    // Equip slots aren't saved (data files name them), so the Trinket slot was appended and moved
    // SlotBag and SlotNone up one (ADR-022). Once a player save stores them they're append-only too.
    same("EquipSlot", "EquipCount", int(EquipCount), 8);
    for (auto [slot, id, name] : {std::tuple{SlotHead, 0, "Head"}, {SlotChest, 1, "Chest"}, {SlotHands, 2, "Hands"},
                                  {SlotLegs, 3, "Legs"}, {SlotFeet, 4, "Feet"}, {SlotMainHand, 5, "Main Hand"},
                                  {SlotOffHand, 6, "Off Hand"}, {SlotTrinket, 7, "Trinket"}}) {
        same(name, "id", int(slot), id);
        same(name, "kSlotNames", std::string_view(kSlotNames[id]), std::string_view(name));
    }
    same("SlotBag", "id", int(SlotBag), 8);
    same("SlotNone", "id", int(SlotNone), 9);

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
        {"Wolf",           1, {8, 6, 70, 2, 2},   true,  true,  false, ModelWolf,
                           Temper::Aggressive, 12,   10,    7.5f, NoItem,     WolfPelt,  0.6f,  30},
        {"Guard",          4, {16, 5, 3, 2, 4},  true,  true,  true,  ModelGuard,
                           Temper::Passive,    0,    6,     6,    Shortsword, NoItem,    0,     60},
        {"Bandit Chief",   3, {11, 3, 2, 2, 2},  true,  true,  false, ModelChief,
                           Temper::Aggressive, 10,   6,     5.5f, Shortsword, IronHelm,  1,     120},
        {"Barrow Ghoul",   3, {7, 4, 2, 2, 2},   true,  true,  false, ModelGhoul,
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
        {"Bone Charm",      SlotTrinket,  {0, 0, 0},     0,         0},  // was SlotNone until ADR-022
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
        same(row, "mods.size()", t.mods.size(), size_t(i == BoneCharm));  // only the Bone Charm has stat modifiers: +2 spirit
    }
    const std::vector<Modifier>& charm = itemType(BoneCharm).mods;
    CHECK(charm.size() == 1 && charm[0].stat == Stat::Spirit && charm[0].kind == ModKind::Flat && charm[0].value == 2);
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
    CHECK(sheet(player)[Stat::Armor] == 37 && player.inv.equipped[SlotMainHand] == Shortsword);
    Marker m{.id = 1, .kind = MarkerKind::Spawn, .pos = {26.5f, 4, 32.5f}, .radius = 3, .entity = EntityKind::Bandit};
    std::vector<Entity> es;
    syncSpawns(es, g, {m}, 1, player.pos, 0.5f);
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
    // the player for 8-12 (sword 6-10 + 2 strength); the bandit has 120 health. Re-pinned for ADR-035:
    // the bandit's windup telegraphs (0.5 s, varied), and the player's hits make it flinch (cancelling
    // its swing) while its own don't break the Iron set's poise; it died at frame 704, the player at 43.
    // Re-pinned for ADR-042: its swings come in strings of up to two, each costing its swing time, so
    // fewer of them land on the player between its hits (4, was 6); it still dies at frame 712.
    const std::vector<std::tuple<int, char, int>> want{
        {65, 'b', 11},  {121, 'b', 12}, {171, 'p', 3},  {182, 'b', 9}, {241, 'b', 11}, {298, 'b', 8},  {347, 'p', 3}, {359, 'b', 9},
        {417, 'b', 12}, {474, 'b', 12}, {535, 'b', 9},  {588, 'p', 5}, {594, 'b', 12}, {651, 'b', 10}, {712, 'b', 13},
    };
    CHECK(hits == want);
    if (hits != want)
        for (auto [frame, who, amount] : hits) std::println(stderr, "  got {{{}, '{}', {}}}", frame, who, amount);
    same("fightReplay", "deathFrame", deathFrame, 712);
    if (std::abs(player.health - 89) >= 0.01f) same("fightReplay", "player.health", player.health, 89.0f);
    same("fightReplay", "bandit.loot", bandit.loot, NoItem);  // the 40% knife roll fails for this seed
    CHECK(!takeLoot(bandit, player.inv) && freeSlots(player.inv) == kBackpackSlots);

    // The bandit chief always drops his iron helm, whatever the seed.
    for (uint32_t seed : {1u, 2u, 3u}) {
        std::vector<Entity> chief;
        syncSpawns(chief, g, {{.id = 1, .kind = MarkerKind::Spawn, .pos = {26.5f, 4, 32.5f}, .entity = EntityKind::BanditChief}}, seed, player.pos, 0.5f);
        CHECK(chief.size() == 1 && damage(chief[0], 1000) && !chief[0].body.alive());
        same(std::format("chief seed {}", seed), "loot", chief[0].loot, IronHelm);
    }
}

// Characterization of the passive temper and the leash, pinned before the AI became a state machine
// (ADR-020) and re-pinned only where that changed it on purpose (noted at the value). A
// bare-handed player stands by a boar (seed 2, fixed 60 fps): it ignores them until punched once, then
// chases and fights back; the player doesn't swing again. Then the player steps beyond the leash: the
// boar calms down and walks home.
static void boarReplay() {
    Grid g = meadow();
    Player player;
    player.pos = {14.5f, 4, 20.5f};
    Marker m{.id = 1, .kind = MarkerKind::Spawn, .pos = {10.5f, 4, 20.5f}, .entity = EntityKind::Boar};
    std::vector<Entity> es;
    syncSpawns(es, g, {m}, 2, player.pos, 0.5f);
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
    CHECK(hits.empty() && !boar.angry());
    player.pos = boar.body.pos + glm::vec3(1.2f, 0, 0);
    run(60, true);  // one punch
    CHECK(boar.angry());
    run(5 * 60, false);  // it fights back
    CHECK(boar.angry());
    same("boarReplay", "boar health", boar.body.health, 97.0f);  // one punch, no regen in combat
    same("boarReplay", "player health", player.health, 76.0f);  // was 71: its windup telegraphs now (ADR-035)
    player.pos = boar.home + glm::vec3(0, 0, 15);  // backs off, still in its territory
    run(3 * 60, false);  // it runs after them and keeps biting
    CHECK(boar.angry());
    auto near = [](float got, float want) { return std::abs(got - want) < 0.01f; };
    // Was 13.75: the boar used to keep running through a swing; a windup now stands committed. Then
    // 13.65, before the windup telegraphed (ADR-035).
    if (!near(glm::distance(boar.body.pos, boar.home), 12.47f))
        same("boarReplay", "chase distance from home", glm::distance(boar.body.pos, boar.home), 12.47f);
    player.pos = boar.home + glm::vec3(0, 0, boar.type().leashRadius + 5);  // out of its territory: gives up at once
    run(1, false);
    CHECK(!boar.angry());
    run(10 * 60, false);  // walks back home, both heal out of combat
    CHECK(!boar.angry());
    if (!near(glm::distance(boar.body.pos, boar.home), 0.067f))
        same("boarReplay", "distance from home", glm::distance(boar.body.pos, boar.home), 0.067f);
    same("boarReplay", "boar health", boar.body.health, 100.0f);
    if (!near(player.health, 84.84f)) same("boarReplay", "player health", player.health, 84.84f);
    const std::vector<std::tuple<int, char, int>> want{  // fewer, later gores since ADR-035's telegraphed windup
        {132, 'b', 3}, {180, 'p', 6}, {241, 'p', 4}, {299, 'p', 3}, {370, 'p', 5}, {431, 'p', 6},
    };
    CHECK(hits == want);
    if (hits != want)
        for (auto [f, who, amount] : hits) std::println(stderr, "  got {{{}, '{}', {}}}", f, who, amount);
}

// The AI state machine (ADR-020): leash hysteresis, the committed windup, passive until hit, giving
// up, Return, fleeing, and the transition log.
static void npcAi() {
    namespace fs = std::filesystem;
    auto flat = [](glm::vec3 v) { return glm::vec2(v.x, v.z); };
    auto newest = [](const Entity& e, uint32_t back = 0) -> const AiTransition& {
        return e.history[(e.transitions - 1 - back) % e.history.size()];
    };
    auto reason = [&](const Entity& e, uint32_t back = 0) { return std::string_view(newest(e, back).reason); };
    // The shipped kinds: the old global leash, nobody flees; a readable windup (ADR-035).
    for (const EntityType& t : defs().creatures)
        CHECK(t.leashRadius == 20 && t.reaggroRadius == 16 && t.windup >= 0.28f && t.cooldown == 0 && t.fleeHealth == 0);
    CHECK(typeOf(EntityKind::Bandit).windup == kDefaultWindup && typeOf(EntityKind::Bandit).recover == kDefaultRecover);
    // The state machine on its own: the bandit here has no tactics (strings, answers, backing off:
    // ADR-042, ADR-043; the enemies test has those). A test-only kind joins it below.
    fs::path dir = fs::temp_directory_path() / "flycoos_ai_test";
    fs::create_directories(dir);
    fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
    std::ostringstream text;
    text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string creatures = text.str();
    size_t tactics = creatures.find(R"("combo")", creatures.find(R"("id": "bandit",)"));
    CHECK(tactics < creatures.find(R"("id": "wolf")"));
    size_t from = creatures.rfind(',', tactics);
    creatures.erase(from, creatures.find('}', tactics) - from);
    creatures.erase(creatures.rfind(']'));
    Defs plain, shipped;
    std::string error;
    auto plainDefs = [&](std::string_view extra) {
        std::ofstream(dir / "creatures.json") << creatures << extra << "]";
        Defs d;
        CHECK(loadDefs(dir, d, error));
        if (!error.empty()) std::println(stderr, "FAIL npcAi data: {}", error);
        return d;
    };
    CHECK(loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
    setDefs(plainDefs(""));
    CHECK(typeOf(EntityKind::Bandit).combo == 1 && typeOf(EntityKind::Bandit).dodge == 0);

    Grid g = meadow();
    Player player;
    std::mt19937 rng(4);
    std::vector<Entity> es;
    const glm::vec3 home{26.5f, 4, 32.5f};
    auto spawn = [&](EntityKind kind) {
        es.clear();
        syncSpawns(es, g, {{.id = 1, .kind = MarkerKind::Spawn, .pos = home, .radius = 3, .entity = kind}}, 1, player.pos, 0.5f);
        CHECK(es.size() == 1);
        return &es[0];
    };
    auto sim = [&](int ticks, auto&& each) {
        for (int i = 0; i < ticks; ++i) {
            each(i);
            updateEntities(es, g, player, kTick);
            separate(player, g, es);
            updateCombat(player, es, kTick, rng);
        }
    };
    auto none = [](int) {};

    // Leash hysteresis: a bandit chases the player to the edge of its territory, where the player
    // stands for 10 s stepping back and forth across it every tick. It gives up once and walks home;
    // the player, still within its aggro range of the bandit's path but outside the re-aggro radius,
    // doesn't pull it back (before the state machine it flipped angry every tick).
    player.pos = home + glm::vec3(-8, 0, 0);
    Entity* bandit = spawn(EntityKind::Bandit);
    sim(1, none);
    CHECK(bandit->state == AiState::Chase && reason(*bandit) == "saw_target");
    uint32_t before = bandit->transitions;
    float leash = bandit->type().leashRadius;
    sim(10 * 60, [&](int i) {
        player.pos = home + glm::vec3(-leash + (i % 2 ? 0.05f : -0.05f), 0, 0);
        player.health = 100;
    });
    CHECK(bandit->transitions - before <= 3 && !bandit->angry());  // Return, then Wander at home
    CHECK(glm::distance(flat(bandit->body.pos), flat(home)) < 3 + 1);
    // Height counts: the same spot 20 blocks up (a cliff, another floor) is out of its aggro range.
    player.pos = bandit->body.pos + glm::vec3(-6, 20, 0);
    sim(1, none);
    CHECK(!bandit->angry());
    // Re-aggro: inside the re-aggro radius and its aggro range it attacks again.
    player.pos = bandit->body.pos + glm::vec3(-6, 0, 0);
    sim(1, none);
    CHECK(bandit->state == AiState::Chase && reason(*bandit) == "saw_target");

    // Windup (ADR-035): a telegraph of 0.85 .. 1.35 x attack_windup, turning after the player until
    // just before the strike, then committed; the hit lands once, at its end, and only if the player is
    // still in reach.
    player.pos = bandit->body.pos + glm::vec3(-1.5f, 0, 0);
    player.health = 100;
    int ticks = 0;
    while (bandit->state != AiState::Windup && ticks++ < 120) sim(1, none);
    CHECK(bandit->state == AiState::Windup && reason(*bandit) == "in_reach" && bandit->body.hitPending);
    float windup = impactTime(bandit->body) + bandit->body.hold;
    const EntityType& bt = bandit->type();
    CHECK(windup > 0.85f * bt.windup - 1e-4f && windup < 1.35f * bt.windup + 1e-4f);
    player.pos = bandit->body.pos + glm::vec3(0, 0, -4);  // steps out of reach, around to its side
    bool turned = false, turnedLate = false;
    for (ticks = 0; bandit->state == AiState::Windup && ticks < 90; ++ticks) {
        float before = bandit->body.yaw;
        bool late = bandit->body.moveTime >= holdTime(bandit->body) && bandit->body.hold <= gMelee.trackSeconds;
        sim(1, none);
        (late ? turnedLate : turned) |= bandit->body.yaw != before;
    }
    CHECK(turned && !turnedLate);  // tracks, then commits
    CHECK(std::abs(ticks - windup * 60) <= 2 && player.health == 100);  // dodged
    CHECK(bandit->state == AiState::Recover && reason(*bandit) == "swung" && newest(*bandit).after >= windup - 2 * kTick);
    // Standing in reach: exactly one hit per windup.
    player.pos = bandit->body.pos + glm::vec3(-1.5f, 0, 0);
    int hits = 0, windups = 0;
    sim(5 * 60, [&](int) {
        windups += bandit->state == AiState::Windup && bandit->stateTime == 0;
        hits += player.hitTime == 0;
        player.health = 100;
    });
    CHECK(hits > 3 && std::abs(hits - windups) <= 1);

    // Return ignores the player: hit from close by on the way home it keeps going, then heals and
    // stays calm for its re-aggro cooldown even with the player beside it.
    sim(60, [&](int) { player.pos = home + glm::vec3(-12, 0, 0); });  // draws it away from home
    player.pos = home + glm::vec3(-leash - 2, 0, 0);
    for (ticks = 0; bandit->state != AiState::Return && ticks < 60; ++ticks) sim(1, none);  // finishes a windup it started
    CHECK(bandit->state == AiState::Return && reason(*bandit) == "target_left_leash");
    sim(30, none);
    damage(*bandit, 20);
    player.pos = bandit->body.pos + glm::vec3(-2, 0, 0);  // behind it: it walks toward home, +x
    float hurtHealth = bandit->body.health;
    sim(1, none);
    CHECK(bandit->state == AiState::Return && hurtHealth < float(maxHealth(bandit->body.stats)));
    for (ticks = 0; bandit->state == AiState::Return && ticks < 20 * 60; ++ticks) {
        player.pos = bandit->body.pos + glm::vec3(-2, 0, 0);  // tags along behind
        player.health = 100;
        sim(1, none);
    }
    CHECK(bandit->state == AiState::Wander && reason(*bandit) == "home" && player.health == 100);
    CHECK(bandit->body.health == float(maxHealth(bandit->body.stats)) && bandit->calmTimer > 0);
    sim(int(bandit->type().reaggroCooldown * 60) - 2, [&](int) { player.pos = bandit->body.pos + glm::vec3(-2, 0, 0); });
    CHECK(!bandit->angry());
    sim(5, none);
    CHECK(bandit->state == AiState::Chase && reason(*bandit) == "saw_target");

    // Giving up: a player behind a wall (no pathfinding) inside its territory.
    for (int z = 20; z < 45; ++z)
        for (int y = 4; y < 7; ++y) g.set({20, y, z}, Stone);
    bandit->body.teleport(home);
    player.pos = home + glm::vec3(-9, 0, 0);
    setState(*bandit, AiState::Chase, "test");
    sim(int(bandit->type().giveUpSeconds * 60) + 120, none);
    CHECK(bandit->state != AiState::Chase && reason(*bandit, bandit->state == AiState::Return ? 0 : 1) == "lost_target");
    g = meadow();

    // Up a pillar beside it the player is out of reach (a body height above): it doesn't swing, and gives up.
    bandit->body.teleport(home);
    for (int y = 4; y < 7; ++y) g.set({24, y, 32}, Stone);
    player.pos = {24.5f, 7, 32.5f};
    player.health = 100;
    setState(*bandit, AiState::Chase, "test");
    sim(int(bandit->type().giveUpSeconds * 60) + 60, [&](int) { player.pos = {24.5f, 7, 32.5f}; });
    CHECK(player.health == 100 && bandit->state == AiState::Wander && reason(*bandit, 1) == "lost_target");
    g = meadow();

    // A hit while chasing, then the player leaves the leash: home already, it turns back once and
    // stays calm (the hit is dropped on entering Return), and only a Return that walked heals.
    player.pos = home + glm::vec3(-2, 0, 0);
    bandit->body.teleport(home);
    bandit->body.swingTimer = 1;  // no swing this time
    setState(*bandit, AiState::Chase, "test");
    damage(*bandit, 10);
    player.pos = home + glm::vec3(-leash - 2, 0, 0);
    before = bandit->transitions;
    updateEntities(es, g, player, kTick);
    CHECK(bandit->state == AiState::Wander && bandit->transitions == before + 2 && reason(*bandit) == "home");
    CHECK(bandit->body.health == float(maxHealth(bandit->body.stats)) - 10);  // didn't travel: keeps its wounds

    // Forcing a state runs no enter actions: a forced Chase doesn't shout.
    {
        Marker m{.id = 2, .kind = MarkerKind::Spawn, .pos = home, .radius = 3, .entity = EntityKind::Guard, .count = 1, .lines = {"Halt."}};
        std::vector<Entity> gs;
        syncSpawns(gs, g, {m}, 1, player.pos, 0.5f);
        setState(gs[0], AiState::Chase, "forced");
        CHECK(gs[0].speech() == nullptr && gs[0].shoutTimer == 0);
        setState(gs[0], AiState::Wander, "forced");
        setState(gs[0], AiState::Chase, "damaged");
        CHECK(gs[0].speech() && std::string_view(gs[0].speech()) == "Halt.");
    }

    // Passive until hit: ten seconds beside a boar, nothing; one hit, one transition.
    player.pos = home + glm::vec3(-3, 0, 0);
    Entity* boar = spawn(EntityKind::Boar);
    sim(10 * 60, [&](int) { player.pos = boar->body.pos + glm::vec3(-1.5f, 0, 0); });
    CHECK(boar->state == AiState::Wander && player.health == 100);
    before = boar->transitions;
    player.pos = boar->body.pos + glm::vec3(-3, 0, 0);  // out of reach: it only starts the chase
    CHECK(damage(*boar, 1) && boar->state == AiState::Wander && boar->events == EventDamaged);  // read on its next update
    updateEntities(es, g, player, kTick);
    CHECK(boar->state == AiState::Chase && boar->transitions == before + 1 && reason(*boar) == "damaged" && boar->events == 0);

    // The log keeps the latest 8, newest last; forcing works from any state.
    for (int i = 0; i < 10; ++i) setState(*boar, i % 2 ? AiState::Idle : AiState::Flee, "forced");
    CHECK(boar->transitions == before + 11 && newest(*boar).from == AiState::Flee && newest(*boar).to == AiState::Idle);
    CHECK(reason(*boar, 7) == "forced" && newest(*boar, 7).from == AiState::Idle);
    setState(*boar, AiState::Dead, "forced");
    updateEntities(es, g, player, kTick);  // alive: it leaves Dead at once
    CHECK(boar->state != AiState::Dead && reason(*boar, boar->state == AiState::Wander ? 0 : 1) == "respawned");

    // Fleeing: a test-only kind, a boar that runs below half health, then goes home and heals.
    setDefs(plainDefs(R"(,{"id": "coward", "save_id": 8, "copy_from": "boar", "flee_health": 0.5})"));
    fs::remove_all(dir);
    player.pos = home + glm::vec3(-3, 0, 0);
    Entity* coward = spawn(EntityKind(8));
    CHECK(coward->type().fleeHealth == 0.5f);
    damage(*coward, 10);
    sim(1, none);
    CHECK(coward->state == AiState::Chase);
    damage(*coward, 50);  // 40 left of 100
    sim(1, none);
    CHECK(coward->state == AiState::Flee && reason(*coward) == "low_health");
    float startDist = glm::distance(flat(coward->body.pos), flat(player.pos));
    sim(int(kFleeSeconds * 60) - 2, none);
    CHECK(coward->state == AiState::Flee && glm::distance(flat(coward->body.pos), flat(player.pos)) > startDist + 5);
    sim(3, none);
    CHECK(coward->state == AiState::Return && reason(*coward) == "fled");
    sim(30 * 60, none);
    CHECK(coward->state == AiState::Wander && coward->body.health == 100);
    setDefs(std::move(shipped));
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
        for (int frame = 0; frame < 10 * 60; ++frame) {
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

// Player saves (ADR-025): everything round-trips, NPCs are keyed by spawn marker and slot (so world
// edits that add or reorder markers keep them), and a bad file never touches the session.
static void playerSaves() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "flycoos_player_save_test";
    fs::remove_all(dir);
    std::string path = (dir / "w.flys").string();
    std::vector<Marker> markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {24.5f, 4, 8.5f}},
                                   {.id = 3, .kind = MarkerKind::Spawn, .pos = {8.5f, 4, 52.5f}, .radius = 4, .entity = EntityKind::Bandit, .count = 2},
                                   {.id = 4, .kind = MarkerKind::Spawn, .pos = {24.5f, 4, 40.5f}, .radius = 3, .entity = EntityKind::Boar}};
    auto make = [&](std::vector<Marker> m) {
        Session s{.world = meadow(), .seed = 5};
        s.markers = std::move(m);
        startSession(s);
        return s;
    };
    auto npc = [](Session& s, uint32_t marker, int slot) -> Entity* {
        auto it = std::ranges::find_if(s.entities, [&](const Entity& e) { return e.spawn.id == marker && e.slot == slot; });
        return it != s.entities.end() ? &*it : nullptr;
    };

    // Play a little: gear, bags, wounds, a dead bandit with loot on its respawn timer.
    Session a = make(markers);
    Player& p = a.player;
    p.teleport({20.25f, 4, 12.75f});
    p.yaw = 1.25f;
    p.level = 3;
    p.stats.strength = 7;
    a.elapsed = 123.5;
    a.timeOfDay = 0.625f;
    for (ItemId id : {IronHelm, BoneCharm, Knife, WolfPelt}) CHECK(giveItem(p.inv, id));
    CHECK(quickMove(p.inv, {SlotRef::Item, 0}));  // shortsword to the main hand
    CHECK(quickMove(p.inv, {SlotRef::Item, 2}));  // helm
    CHECK(quickMove(p.inv, {SlotRef::Item, 3}));  // the charm to the trinket slot
    CHECK(quickMove(p.inv, {SlotRef::Item, 1}));  // the satchel to the bag bar
    CHECK(moveItem(p.inv, {SlotRef::Item, 5}, {SlotRef::Item, 2, 1}));  // the pelt into the satchel
    CHECK(p.inv.equipped[SlotTrinket] == BoneCharm && p.inv.equipped[SlotHead] == IronHelm && p.inv.bags[0] == Satchel);
    p.health = 37;
    Entity *b0 = npc(a, 3, 0), *b1 = npc(a, 3, 1);
    CHECK(b0 && b1 && npc(a, 4, 0));
    b0->body.teleport({10.5f, 4, 50.5f});
    b0->body.yaw = -2;
    b0->body.health = 5;
    b1->body.health = 0;
    b1->body.deadTime = 7;
    b1->loot = Knife;
    glm::vec3 corpse = b1->body.pos;
    CHECK(savePlayer(path.c_str(), a, "w.flyw", 0x1234abcd).empty());

    Session b = make(markers);
    SaveReport r;
    CHECK(loadPlayer(path.c_str(), b, 0x1234abcd, r).empty());
    CHECK(r.world == "w.flyw" && !r.worldChanged && !r.fromBackup && r.npcs == 3 && r.droppedNpcs == 0 && r.notes.empty());
    CHECK(b.elapsed == 123.5 && b.timeOfDay == 0.625f);
    CHECK(b.player.pos == glm::vec3(20.25f, 4, 12.75f) && b.player.prevPos == b.player.pos && b.player.yaw == 1.25f);
    CHECK(b.player.health == 37 && b.player.level == 3 && b.player.stats.strength == 7 && b.player.stats.stamina == p.stats.stamina);
    CHECK(b.player.inv.equipped == p.inv.equipped && b.player.inv.bags == p.inv.bags && b.player.inv.containers == p.inv.containers);
    Entity *c0 = npc(b, 3, 0), *c1 = npc(b, 3, 1);
    CHECK(c0 && c0->body.pos == glm::vec3(10.5f, 4, 50.5f) && c0->body.yaw == -2 && c0->body.health == 5 && c0->body.alive());
    CHECK(c1 && !c1->body.alive() && c1->state == AiState::Dead && c1->body.deadTime == 7 && c1->loot == Knife && c1->body.pos == corpse);
    // The respawn timer runs on: back home, healed, after the rest of the 30 s.
    float rest = c1->type().respawnSeconds - 7;
    for (int i = 0; i < int((rest - 0.5f) / kTick); ++i) step(b, {}, kTick);
    CHECK(!npc(b, 3, 1)->body.alive());
    for (int i = 0; i < int(1 / kTick); ++i) step(b, {}, kTick);
    CHECK(npc(b, 3, 1)->body.alive() && npc(b, 3, 1)->loot == NoItem);

    // Bad files are rejected and leave the session alone: a flipped byte, a truncation, a huge count.
    auto bytes = [](const std::string& f) {
        std::ifstream in(f, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
    };
    auto write = [](const std::string& f, const std::vector<uint8_t>& v) {
        std::ofstream(f, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size()));
    };
    auto le = [](std::vector<uint8_t>& out, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    // A save of these sections, with a valid checksum.
    auto craft = [&](std::vector<std::pair<std::string, std::vector<uint8_t>>> sections) {
        std::vector<uint8_t> out{'F', 'L', 'Y', 'S', 1, 0, 0, 0};
        for (auto& [tag, payload] : sections) {
            out.insert(out.end(), tag.begin(), tag.end());
            le(out, payload.size(), 8);
            out.insert(out.end(), payload.begin(), payload.end());
        }
        le(out, fnv1a(out.data(), out.size()), 4);
        return out;
    };
    std::vector<uint8_t> head{0};  // no world name, checksum 0x1234abcd, seed 5, 10 s, 0.5
    le(head, 0x1234abcd, 4), le(head, 5, 4), le(head, std::bit_cast<uint64_t>(10.0), 8), le(head, std::bit_cast<uint32_t>(0.5f), 4);
    std::vector<uint8_t> good = bytes(path);
    std::string bad = (dir / "bad.flys").string();
    std::vector<uint8_t> hugeNpcs, hugeEquip{200};
    le(hugeNpcs, 1000000, 4);
    std::vector<std::vector<uint8_t>> broken = {
        [&] { auto v = good; v[v.size() / 2] ^= 1; return v; }(),
        std::vector<uint8_t>(good.begin(), good.begin() + long(good.size() / 2)),
        craft({{"HEAD", head}, {"NPCS", hugeNpcs}}),
        craft({{"HEAD", head}, {"INVT", hugeEquip}}),
        craft({{"HEAD", head}, {"HEAD", head}}),
        craft({{"PLYR", {}}}),  // no header
        [&] {  // a later version
            auto v = craft({{"HEAD", head}});
            v[4] = 2;
            v.resize(v.size() - 4);
            le(v, fnv1a(v.data(), v.size()), 4);
            return v;
        }(),
    };
    Session before = make(markers);
    before.player.health = 12;
    for (const std::vector<uint8_t>& v : broken) {
        write(bad, v);
        Session t = before;
        SaveReport tr;
        tr.npcs = 99;
        CHECK(!loadPlayer(bad.c_str(), t, 0, tr).empty());
        CHECK(t.player.health == 12 && t.player.pos == before.player.pos && t.elapsed == before.elapsed && tr.npcs == 99);
    }
    CHECK(!loadPlayer((dir / "missing.flys").string().c_str(), before, 0, r).empty());

    // An unknown section (from a later build) is skipped; missing ones keep startSession's state.
    {
        std::vector<uint8_t> v(good.begin(), good.end() - 4);
        v.insert(v.end(), {'Z', 'Z', 'Z', 'Z', 3, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3});
        le(v, fnv1a(v.data(), v.size()), 4);
        write(bad, v);
        Session t = make(markers);
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && r.npcs == 3 && t.player.inv.equipped[SlotTrinket] == BoneCharm);
        write(bad, craft({{"HEAD", head}}));
        t = a;
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty());
        CHECK(t.elapsed == 10 && t.timeOfDay == 0.5f && t.player.pos == glm::vec3(24.5f, 4, 8.5f) && t.player.health == float(maxHealth(sheet(t.player))));
        CHECK(t.player.inv.containers[0][0] == Shortsword && t.player.inv.equipped[SlotTrinket] == NoItem && npc(t, 3, 1)->body.alive());
    }

    // The world changed: a marker added before the bandits (other vector indices), the boar's gone,
    // and the bandit marker spawns one now. The bandit in slot 0 keeps its state; the rest drop, with notes.
    {
        std::vector<Marker> edited = {markers[0],
                                      {.id = 2, .kind = MarkerKind::Spawn, .pos = {16.5f, 4, 30.5f}, .radius = 3, .entity = EntityKind::Wolf},
                                      markers[1]};
        edited[2].count = 1;
        Session t = make(edited);
        CHECK(t.entities[0].spawn.id == 2);  // the wolf comes first now
        CHECK(loadPlayer(path.c_str(), t, 0x99, r).empty());
        CHECK(r.worldChanged && r.npcs == 1 && r.droppedNpcs == 2 && r.notes.size() == 3);
        CHECK(npc(t, 3, 0)->body.health == 5 && npc(t, 3, 0)->body.pos == glm::vec3(10.5f, 4, 50.5f));
        CHECK(npc(t, 2, 0)->body.health == float(maxHealth(sheet(npc(t, 2, 0)->body))));
        CHECK(std::ranges::count_if(r.notes, [](const std::string& n) { return n.contains("gone"); }) == 1);
        // With the count back, both bandits load: the dead one too, still holding its loot.
        edited[2].count = 2;
        t = make(edited);
        CHECK(loadPlayer(path.c_str(), t, 0x1234abcd, r).empty() && r.npcs == 2 && r.droppedNpcs == 1 && !r.worldChanged);
        CHECK(!npc(t, 3, 1)->body.alive() && npc(t, 3, 1)->loot == Knife);
    }

    // The inventory is fitted to the current data: an item in a slot it doesn't fit, or past a bag's
    // (smaller) size, moves to a free slot, else is lost; unknown item and creature ids are dropped.
    {
        auto inventory = [&](std::vector<uint8_t> equip, std::vector<uint8_t> bags, std::vector<std::vector<uint8_t>> containers) {
            std::vector<uint8_t> v{uint8_t(equip.size())};
            v.insert(v.end(), equip.begin(), equip.end());
            v.push_back(uint8_t(bags.size()));
            v.insert(v.end(), bags.begin(), bags.end());
            v.push_back(uint8_t(containers.size()));
            for (auto& c : containers) v.push_back(uint8_t(c.size())), v.insert(v.end(), c.begin(), c.end());
            return v;
        };
        std::vector<uint8_t> satchel(size_t(itemType(Satchel).bagSlots) + 2, NoItem);
        satchel.back() = WolfPelt;  // past the satchel's size
        std::vector<uint8_t> backpack(kBackpackSlots, NoItem);
        backpack[0] = 250;  // no such item
        write(bad, craft({{"HEAD", head}, {"INVT", inventory({Knife}, {Satchel}, {backpack, satchel})}}));  // a knife on the head
        Session t = make(markers);
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && r.notes.size() == 3);
        CHECK(t.player.inv.equipped[SlotHead] == NoItem && t.player.inv.bags[0] == Satchel &&
              t.player.inv.containers[1].size() == size_t(itemType(Satchel).bagSlots));
        CHECK(t.player.inv.containers[0][0] == Knife && t.player.inv.containers[0][1] == WolfPelt);
        CHECK(std::ranges::count_if(r.notes, [](const std::string& n) { return n.contains("moved"); }) == 2 && r.notes[0].contains("unknown item 250"));
        std::vector<uint8_t> full(kBackpackSlots, BoarSkin);
        write(bad, craft({{"HEAD", head}, {"INVT", inventory({Knife}, {}, {full})}}));
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && r.notes.size() == 1 && r.notes[0].contains("lost"));
        CHECK(t.player.inv.equipped[SlotHead] == NoItem && freeSlots(t.player.inv) == 0);
        std::vector<uint8_t> npcs;  // one of a creature kind the data doesn't have
        le(npcs, 1, 4), le(npcs, 3, 4), le(npcs, 0, 1), le(npcs, 200, 1);
        for (float f : {10.0f, 0.0f}) le(npcs, std::bit_cast<uint32_t>(f), 4);
        le(npcs, 0, 1);
        for (float f : {8.5f, 4.0f, 52.5f, 0.0f}) le(npcs, std::bit_cast<uint32_t>(f), 4);
        write(bad, craft({{"HEAD", head}, {"NPCS", npcs}}));
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && r.npcs == 0 && r.notes.size() == 1 && r.notes[0].contains("unknown kind"));
        // Not a number anywhere is a corrupt file: the player's position, an NPC's health.
        std::vector<uint8_t> player;
        for (float f : {std::numeric_limits<float>::quiet_NaN(), 4.0f, 8.0f, 0.0f, 50.0f, 0.0f}) le(player, std::bit_cast<uint32_t>(f), 4);
        for (int v : {1, 10, 2, 2, 2, 2}) le(player, uint32_t(v), 4);
        write(bad, craft({{"HEAD", head}, {"PLYR", player}}));
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).starts_with("bad player"));
        npcs[10] = 0, npcs[11] = 0, npcs[12] = 0xc0, npcs[13] = 0x7f;  // the health: a NaN
        write(bad, craft({{"HEAD", head}, {"NPCS", npcs}}));
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).starts_with("bad NPC"));
    }

    // Marker ids are never reused, so a save's NPCs can't land on a newer marker: the highest id ever
    // handed out survives undo-then-edit (redo cleared) and a world save (MKID); older files use max + 1.
    {
        Grid g({32, 32, 32});  // world files hold whole chunks
        EditHistory h;
        std::vector<Marker> list;
        std::vector<Instance> none;
        Marker poi{.kind = MarkerKind::Poi, .name = "Well", .pos = {5.5f, 4, 5.5f}};
        CHECK(putMarker(g, list, h, poi).id == 1 && putMarker(g, list, h, poi).id == 2);
        undoEdit(g, h, none, list);  // marker 2 is gone, its id only in the redo stack
        CHECK(putMarker(g, list, h, poi).id == 3 && h.redo.empty());  // then not even there
        CHECK(!removeMarker(g, list, h, 3).error && list.size() == 1 && h.markerIds == 3);
        std::string world = (dir / "ids.flyw").string();
        CHECK(saveWorld(world.c_str(), g, 1, {}, list, h.markerIds).empty());
        Grid back({32, 32, 32});
        uint32_t seed = 0, ids = 0;
        std::optional<std::vector<Marker>> loaded;
        CHECK(loadWorld(world.c_str(), back, seed, none, loaded, &ids).empty() && ids == 3 && loaded && loaded->size() == 1);
        EditHistory fresh{.markerIds = ids};
        CHECK(nextMarkerId(*loaded, fresh) == 4);
        CHECK(saveWorld(world.c_str(), g, 1, {}, list).empty());  // an older file: no MKID
        CHECK(loadWorld(world.c_str(), back, seed, none, loaded, &ids).empty() && ids == 0 && nextMarkerId(*loaded, {}) == 2);
    }

    // The previous save is kept as .bak, and loads when the save itself is broken.
    a.player.health = 20;
    CHECK(savePlayer(path.c_str(), a, "w.flyw", 0x1234abcd).empty());
    CHECK(fs::exists(path + ".bak") && !fs::exists(path + ".tmp"));
    std::vector<uint8_t> second = bytes(path);
    second[10] ^= 0xff;
    write(path, second);
    Session t = make(markers);
    CHECK(loadPlayer(path.c_str(), t, 0x1234abcd, r).empty() && r.fromBackup && t.player.health == 37 && r.notes.size() == 1);
    fs::remove_all(dir);
}

// Time-of-day spawns (ADR-023): a spawn marker's active window, its NPCs coming and going with it
// through step(), and the window in world files, commands and prefab files.
static void timedSpawns() {
    Grid g = meadow();  // grass top at y = 3 (feet at 4), pond at x >= 32
    Marker night{.id = 2, .kind = MarkerKind::Spawn, .name = "Ring", .pos = {20.5f, 4, 40.5f}, .radius = 3,
                 .entity = EntityKind::Ghoul, .count = 2, .activeFrom = 0.8f, .activeTo = 0.2f};
    // Windows: [from, to), across midnight when from > to; none (both 0) is always.
    CHECK(!checkMarker(g, night));
    for (float t : {0.8f, 0.9f, 0.0f, 0.1999f}) CHECK(activeAt(night, t));
    for (float t : {0.2f, 0.5f, 0.7999f}) CHECK(!activeAt(night, t));
    Marker day = night;
    day.id = 3, day.activeFrom = 0.25f, day.activeTo = 0.75f;
    CHECK(!checkMarker(g, day) && activeAt(day, 0.25f) && activeAt(day, 0.5f) && !activeAt(day, 0.75f) && !activeAt(day, 0.1f));
    Marker always = night;
    always.activeFrom = always.activeTo = 0;
    CHECK(!checkMarker(g, always) && activeAt(always, 0) && activeAt(always, 0.5f) && activeAt(always, 0.99f));
    for (auto [from, to] : std::initializer_list<std::pair<float, float>>{{0.5f, 0.5f}, {-0.1f, 0.2f}, {0.8f, 1}, {NAN, 0.2f}, {0.8f, INFINITY}}) {
        Marker m = night;
        m.activeFrom = from, m.activeTo = to;
        CHECK(checkMarker(g, m));
    }
    Marker well{.id = 1, .kind = MarkerKind::Poi, .name = "Well", .pos = {5.5f, 4, 5.5f}, .radius = 8, .activeFrom = 0.8f, .activeTo = 0.2f};
    CHECK(checkMarker(g, well));  // windows are for spawns
    well.activeFrom = well.activeTo = 0;

    // step(): none by day; they come as the window opens and leave as it closes.
    Session s{.world = meadow(), .seed = 4};
    s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {5.5f, 4, 5.5f}}, night};
    s.timeOfDay = 0.5f;
    startSession(s);
    CHECK(s.entities.empty());
    s.timeOfDay = 0.79f;
    step(s, {}, kTick);
    CHECK(s.entities.empty());
    s.timeOfDay = 0.8f - 2e-6f;  // a tick is ~3.9e-6 of a day
    step(s, {}, kTick);
    CHECK(s.entities.size() == 2 && s.entities[0].home == night.pos && s.entities[0].kind == EntityKind::Ghoul);
    s.timeOfDay = 1 - 1e-5f;
    for (int i = 0; i < 10; ++i) step(s, {}, kTick);  // past midnight: the same ones stay
    CHECK(s.timeOfDay < 0.1f && s.entities.size() == 2 && s.entities[0].stateTime > 0.1f);
    s.timeOfDay = 0.2f - 2e-6f;
    step(s, {}, kTick);
    CHECK(s.entities.empty());
    for (int i = 0; i < 60; ++i) step(s, {}, kTick);
    CHECK(s.entities.empty());  // and none come back by day

    // A fight outlasts the window: the fighter finishes it, then leaves; the other leaves at once,
    // and the marker doesn't fill up again by day.
    s.timeOfDay = 0.9f;
    step(s, {}, kTick);
    CHECK(s.entities.size() == 2);
    s.entities[1].calmTimer = 1000;  // doesn't attack on sight: stays out of the fight
    s.player.teleport(s.entities[0].body.pos + glm::vec3(2, 0, 0));
    for (int i = 0; i < 30 && !s.entities[0].angry(); ++i) step(s, {}, kTick);
    CHECK(s.entities.size() == 2 && s.entities[0].angry() && !s.entities[1].angry());
    s.timeOfDay = 0.5f;
    step(s, {}, kTick);
    CHECK(s.entities.size() == 1 && s.entities[0].angry());  // mid-fight: stays
    for (int i = 0; i < 20; ++i) step(s, {}, kTick);
    CHECK(s.entities.size() == 1);
    // The window opens again mid-fight: only the missing slot spawns, the fighter keeps its state.
    int fighter = s.entities[0].slot;
    uint32_t fighterTransitions = s.entities[0].transitions;
    s.entities[0].body.health = 33;
    s.timeOfDay = 0.9f;
    step(s, {}, kTick);
    auto bySlot = [&](int slot) -> Entity* {
        auto it = std::ranges::find(s.entities, slot, &Entity::slot);
        return it != s.entities.end() ? &*it : nullptr;
    };
    Entity *kept = bySlot(fighter), *fresh = bySlot(1 - fighter);
    CHECK(s.entities.size() == 2 && kept && fresh && kept->angry() && kept->body.health < 34 && kept->transitions >= fighterTransitions);
    CHECK(fresh && fresh->body.health == float(maxHealth(sheet(fresh->body))) && glm::distance(fresh->home, night.pos) < 4);
    s.timeOfDay = 0.5f;
    s.player.teleport({5.5f, 4, 5.5f});  // beyond its leash: it gives up, and goes
    int ticks = 0;
    for (; ticks < 120 && !s.entities.empty(); ++ticks) step(s, {}, kTick);
    CHECK(s.entities.empty() && ticks < 120);

    // Player saves (.flys, ADR-025): a save made at night keeps a wounded ghoul; one loaded where the
    // window no longer covers the save's time (the clock comes from the save) says why it's dropped.
    {
        std::string save = (std::filesystem::temp_directory_path() / "flycoos_timed.flys").string();
        Session a{.world = meadow(), .seed = 4};
        a.markers = s.markers;
        a.timeOfDay = 0.9f;
        startSession(a);
        CHECK(a.entities.size() == 2);
        a.entities[1].body.health = 20;
        int wounded = a.entities[1].slot;
        CHECK(savePlayer(save.c_str(), a, "w.flyw", 7).empty());
        Session b{.world = meadow(), .seed = 4};
        b.markers = s.markers;
        b.timeOfDay = 0.5f;  // whatever the game had: the save's clock wins
        SaveReport r;
        CHECK(loadPlayer(save.c_str(), b, 7, r).empty() && r.npcs == 2 && r.droppedNpcs == 0 && b.timeOfDay == 0.9f);
        auto it = std::ranges::find(b.entities, wounded, &Entity::slot);
        CHECK(it != b.entities.end() && it->body.health == 20);
        Session c{.world = meadow(), .seed = 4};
        c.markers = s.markers;
        c.markers[1].activeFrom = 0.25f, c.markers[1].activeTo = 0.75f;  // the world made them day-only since
        SaveReport cr;
        CHECK(loadPlayer(save.c_str(), c, 7, cr).empty() && c.entities.empty() && cr.npcs == 0 && cr.droppedNpcs == 2);
        CHECK(std::ranges::count_if(cr.notes, [](const std::string& n) { return n.contains("out of its active window"); }) == 2);
        std::filesystem::remove(save), std::filesystem::remove(save + ".bak");
    }

    // Corpses: an empty one lies kCorpseLinger (a kill out of the window shows), one with loot until
    // it's looted or back up; none come back by day.
    s.timeOfDay = 0.9f;
    step(s, {}, kTick);
    CHECK(s.entities.size() == 2);
    s.timeOfDay = 0.5f;
    CHECK(damage(s.entities[1], 10000));
    s.entities[1].loot = NoItem;  // whatever the drop roll said
    step(s, {}, kTick);
    CHECK(s.entities.size() == 1 && !s.entities[0].body.alive());  // the living one left, the corpse lies
    for (int i = 0; i < int(kCorpseLinger * 60) - 3; ++i) step(s, {}, kTick);
    CHECK(s.entities.size() == 1);
    for (int i = 0; i < 4; ++i) step(s, {}, kTick);
    CHECK(s.entities.empty());
    s.timeOfDay = 0.9f;
    step(s, {}, kTick);
    CHECK(s.entities.size() == 2);
    for (Entity& e : s.entities) {
        CHECK(damage(e, 10000));
        e.loot = BoneCharm;
    }
    s.timeOfDay = 0.5f;
    for (int i = 0; i < int(kCorpseLinger * 60) + 2; ++i) step(s, {}, kTick);
    CHECK(s.entities.size() == 2);
    s.player.teleport(s.entities[1].body.pos + (s.entities[1].body.pos - s.entities[0].body.pos) * 0.3f);  // the nearer corpse
    SimInput loot;
    loot.interact = true;
    step(s, loot, kTick);  // looted past kCorpseLinger: it goes at once
    CHECK(s.entities.size() == 1 && s.entities[0].loot == BoneCharm && s.player.inv.containers[0][2] == BoneCharm);
    for (int i = 0; i < int(typeOf(EntityKind::Ghoul).respawnSeconds * 60) + 2; ++i) step(s, {}, kTick);
    CHECK(s.entities.empty());

    // Files: windows round-trip in their TIME section; without it (older files) every marker is always active.
    std::string path = (std::filesystem::temp_directory_path() / "flycoos_times_test.flyw").string();
    std::vector<Marker> list = {well, night, day};
    std::optional<std::vector<Marker>> loaded;
    std::vector<Instance> instances;
    uint32_t seed = 0;
    Grid fg({64, 32, 64}), back({32, 32, 32});
    CHECK(saveWorld(path.c_str(), fg, 5, {}, list).empty());
    CHECK(loadWorld(path.c_str(), back, seed, instances, loaded).empty() && loaded && *loaded == list);
    std::ifstream in(path, std::ios::binary);
    const std::string file((std::istreambuf_iterator<char>(in)), {});
    in.close();
    CHECK(file.contains("TIME"));
    auto u32 = [](uint32_t v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
    auto seal = [&](std::string head, std::string payload) {  // `head`, then `payload` as TIME (none: no section)
        std::string bytes = head;
        if (!payload.empty()) {
            bytes += "TIME";
            for (int i = 0; i < 8; ++i) bytes += char(uint64_t(payload.size()) >> (8 * i));
            bytes += payload;
        }
        uint32_t hash = 2166136261u;
        for (char c : bytes) hash = (hash ^ uint8_t(c)) * 16777619u;
        for (int i = 0; i < 4; ++i) bytes += char(hash >> (8 * i));
        std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
        return loadWorld(path.c_str(), back, seed, instances, loaded);
    };
    std::string head = file.substr(0, file.rfind("TIME"));
    CHECK(seal(head, "").empty() && loaded && loaded->size() == 3 &&
          std::ranges::all_of(*loaded, [](const Marker& m) { return m.activeFrom == 0 && m.activeTo == 0 && activeAt(m, 0.5f); }));
    auto times = [&](uint32_t id, float from, float to) { return u32(id) + u32(std::bit_cast<uint32_t>(from)) + u32(std::bit_cast<uint32_t>(to)); };
    CHECK(seal(head, u32(1) + times(3, 0.1f, 0.3f)).empty() && (*loaded)[2].activeFrom == 0.1f && (*loaded)[1].activeFrom == 0);
    for (const std::string& hostile : {
             u32(1) + times(9, 0.8f, 0.2f),                          // no such marker
             u32(1) + times(1, 0.8f, 0.2f),                          // a poi
             u32(1) + times(2, 0.5f, 0.5f),                          // no window
             u32(1) + times(2, 0, 0),                                // no window either
             u32(1) + times(2, 1.5f, 0.2f),                          // not a time of day
             u32(1) + times(2, NAN, 0.2f),                           // NaN
             u32(2) + times(3, 0.8f, 0.2f) + times(2, 0.8f, 0.2f),  // ids not ascending
             u32(2) + times(2, 0.8f, 0.2f) + times(2, 0.8f, 0.2f),  // twice
             u32(2) + times(2, 0.8f, 0.2f),                          // truncated
             u32(1) + times(2, 0.8f, 0.2f) + "x",                    // trailing bytes
             std::string("ab"),                                      // no count
         }) {
        std::string error = seal(head, hostile);
        CHECK(!error.empty() && error.contains("times"));
    }
    {  // two TIME sections; one without markers
        std::string first = u32(1) + times(2, 0.8f, 0.2f), section = "TIME";
        for (int i = 0; i < 8; ++i) section += char(uint64_t(first.size()) >> (8 * i));
        CHECK(seal(head + section + first, u32(1) + times(3, 0.8f, 0.2f)).contains("duplicate"));
        CHECK(seal(file.substr(0, file.rfind("MARK")), first).contains("without markers"));
    }
    std::filesystem::remove(path);

    // Commands: mark and edit_marker set the window, markers lists it, bad ones change nothing.
    std::vector<Marker> marks;
    EditHistory ch;
    PrefabLibrary lib = loadPrefabs("no-such-folder");
    GameView view;
    auto run = [&](std::string_view request) { return runCommand(request, g, instances, marks, ch, lib, view); };
    auto ok = [&](const Command& c) { return c.response["ok"].get<bool>(); };
    Command c = run(R"({"cmd": "mark", "args": {"kind": "spawn", "x": 20, "z": 40, "entity": "ghoul", "count": 2, "active": [0.8, 0.2]}})");
    CHECK(ok(c) && marks.size() == 1 && marks[0].activeFrom == 0.8f && marks[0].activeTo == 0.2f);
    CHECK(ok(c) && c.response["result"]["marker"]["active"] == nlohmann::json::array({0.8, 0.2}));  // rounded, not 0.800000011920929
    CHECK(ok(run(R"({"cmd": "mark", "args": {"kind": "poi", "name": "Well", "x": 5, "z": 5}})")) && ch.undo.size() == 2);
    c = run(R"({"cmd": "markers", "args": {"kind": "spawn"}})");
    CHECK(ok(c) && c.response["result"]["markers"][0]["active"] == nlohmann::json::array({0.8, 0.2}));
    c = run(R"({"cmd": "markers", "args": {"kind": "poi"}})");
    CHECK(ok(c) && !c.response["result"]["markers"][0].contains("active"));
    for (const char* hostile : {
             R"({"cmd": "edit_marker", "args": {"id": 1, "active": "night"}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "active": [0.5]}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "active": [0.1, 0.2, 0.3]}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "active": [0.5, 0.5]}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "active": [1.5, 0.2]}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "active": [1, 0.2]}})",
             R"({"cmd": "edit_marker", "args": {"id": 1, "active": [0.8, "x"]}})",
             R"({"cmd": "edit_marker", "args": {"id": 2, "active": [0.8, 0.2]}})",
             R"({"cmd": "mark", "args": {"kind": "region", "name": "Vale", "x": 9, "z": 9, "active": [0.8, 0.2]}})",
         }) {
        c = run(hostile);
        CHECK(!ok(c) && c.dirty.empty());
    }
    CHECK(marks.size() == 2 && ch.undo.size() == 2 && marks[0].activeFrom == 0.8f);
    c = run(R"({"cmd": "edit_marker", "args": {"id": 1, "active": []}})");
    CHECK(ok(c) && marks[0].activeFrom == 0 && marks[0].activeTo == 0 && !c.response["result"]["marker"].contains("active"));
    CHECK(ok(run(R"({"cmd": "edit_marker", "args": {"id": 1, "active": [0.25, 0.75]}})")) && marks[0].activeFrom == 0.25f);
    CHECK(ok(run(R"({"cmd": "edit_marker", "args": {"id": 1, "name": "Ring"}})")) && marks[0].activeTo == 0.75f);  // left out: kept
    CHECK(ok(run(R"({"cmd": "undo"})")) && ok(run(R"({"cmd": "undo"})")) && ok(run(R"({"cmd": "undo"})")) && marks[0].activeFrom == 0.8f);
    c = run(R"({"cmd": "edit_marker", "args": {"id": 1, "active": [0.99999999, 0.2]}})");  // rounds to 1.0f as a float
    CHECK(ok(c) && marks[0].activeFrom < 1 && marks[0].activeFrom > 0.9999f && !checkMarker(g, marks[0]));

    // Prefab files: `active=FROM-TO` on a marker line.
    Prefab p{.name = "x"};
    CHECK(parsePrefab("S = Stone\nmarker: spawn x=0 z=0 entity=ghoul active=0.8-0.2 name=Ring\nlayer\nS\n", p).empty() &&
          p.markers.size() == 1 && p.markers[0].activeFrom == 0.8f && p.markers[0].activeTo == 0.2f && p.markers[0].name == "Ring");
    for (const char* bad : {"active=0.8", "active=0.8-", "active=-0.2", "active=x-0.2", "active=0.8-0.2x", "active=0.5-0.5", "active=0.8-1.5"}) {
        Prefab q{.name = "x"};
        CHECK(!parsePrefab(std::format("S = Stone\nmarker: spawn x=0 z=0 entity=ghoul {}\nlayer\nS\n", bad), q).empty());
    }
    Prefab q{.name = "x"};
    CHECK(parsePrefab("S = Stone\nmarker: poi x=0 z=0 active=0.8-0.2 name=Well\nlayer\nS\n", q).contains("for spawn markers"));
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

// Data definitions (ADR-018): the shipped files load cleanly, and every kind of bad record is a hard
// error naming the file, record and field.
static void dataFiles() {
    namespace fs = std::filesystem;
    Defs shipped;
    std::string error;
    CHECK(loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error) && error.empty());
    if (!error.empty()) std::println(stderr, "FAIL shipped data: {}", error);
    CHECK(shipped.items.size() >= ItemCount && shipped.itemIds[0].empty() && shipped.itemById.at("iron_helm") == IronHelm);
    {  // the shields (ADR-036): small to great, guarding better and parrying less
        const Shield &small = shipped.items[shipped.itemById.at("oak_buckler")].shield, &medium = shipped.items[shipped.itemById.at("heater_shield")].shield,
                     &great = shipped.items[shipped.itemById.at("tower_shield")].shield;
        CHECK(small.size == ShieldSize::Small && medium.size == ShieldSize::Medium && great.size == ShieldSize::Great);
        CHECK(small.negation < medium.negation && small.boost < medium.boost && medium.boost < great.boost);
        CHECK(small.parry > 0 && small.parry < medium.parry && great.parry == 0);
        CHECK(!shipped.creatures[size_t(EntityKind::Boar)].parryable && shipped.creatures[size_t(EntityKind::BanditChief)].parries == 2 &&
              shipped.creatures[size_t(EntityKind::Bandit)].parryable && shipped.creatures[size_t(EntityKind::Bandit)].parries == 1);
    }
    {  // weights (ADR-039): everything worn weighs, shields the more the bigger; a knife parries (ADR-041);
       // beasts bite, gore and claw, ghouls are weak to strike (ADR-040)
        for (size_t i = 1; i < shipped.items.size(); ++i)
            if (shipped.items[i].slot < EquipCount && shipped.items[i].weight <= 0) std::println(stderr, "FAIL {} weighs nothing", shipped.itemIds[i]), ++failures;
        auto weight = [&](const char* id) { return shipped.items[shipped.itemById.at(id)].weight; };
        CHECK(weight("oak_buckler") < weight("heater_shield") && weight("heater_shield") < weight("tower_shield") && weight("iron_cuirass") > weight("iron_helm"));
        CHECK(shipped.items[Knife].parry > 0 && shipped.items[Shortsword].parry == 0);
        const EntityType &wolf = shipped.creatures[size_t(EntityKind::Wolf)], &ghoul = shipped.creatures[size_t(EntityKind::Ghoul)];
        CHECK(wolf.damage == DamageType::Pierce && ghoul.resist[size_t(DamageType::Strike)] < 0 && ghoul.resist[size_t(DamageType::Pierce)] > 0);
    }

    fs::path dir = fs::temp_directory_path() / "flycoos_defs_test";
    fs::create_directories(dir);
    std::ofstream(dir / "creatures.json") << codeItemCreatures();
    std::string base = codeNamedItems();
    auto write = [&](std::string_view text) { std::ofstream(dir / "items.json") << text; };
    auto load = [&](std::string_view text, Defs& d) {
        write(text);
        return loadDefs(dir, d, error);
    };
    // copy_from merge-patches: every field but id and save_id is inherited, objects merge deeply,
    // null removes. Data may add items after the ones code names.
    Defs grown;
    CHECK(load(base + R"(,{"id": "dented_helm", "save_id": 12, "copy_from": "iron_helm", "name": "Dented Helm", "armor": 4},
                         {"id": "quick_sword", "save_id": 13, "copy_from": "iron_shortsword", "name": "Quick Sword", "attack": {"speed": 0.5}},
                         {"id": "sword_pommel", "save_id": 14, "copy_from": "iron_shortsword", "name": "Pommel", "slot": null, "attack": null,
                          "weight": null}])", grown));
    if (!error.empty()) std::println(stderr, "FAIL grown data: {}", error);
    CHECK(grown.items.size() == 15 && grown.items[12].slot == SlotHead && grown.items[12].armor == 4 &&
          std::string_view(grown.items[12].name) == "Dented Helm" && grown.itemIds[12] == "dented_helm");
    const Attack& quick = grown.items[13].attack;
    CHECK(quick.minDamage == 6 && quick.maxDamage == 10 && quick.speed == 0.5f && grown.items[13].slot == SlotMainHand);
    CHECK(grown.items[14].slot == SlotNone && grown.items[14].attack.speed == 0);
    std::string chain = base;  // 16 copies deep is fine, 17 isn't
    for (int i = 0; i <= 16; ++i)
        chain += std::format(R"(,{{"id": "c{}", "save_id": {}, "copy_from": "{}", "name": "C"}})", i, 12 + i, i ? std::format("c{}", i - 1) : "boar_skin");
    std::string chain16 = chain.substr(0, chain.rfind(",{")) + "]";
    CHECK(load(chain16, grown) && grown.items.size() == 28);
    for (auto [text, want] : std::initializer_list<std::pair<std::string, std::string_view>>{
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_sword", "name": "A"}])",
              "record 11 'a': field 'copy_from': unknown id 'iron_sword'"},
             {base + R"(,{"id": "boar_skin", "save_id": 12, "name": "A"}])", "record 11 'boar_skin': field 'id': duplicate of record 3"},
             {base + R"(,{"id": "a", "save_id": 5, "name": "A"}])", "record 11 'a': field 'save_id': 5 is already 'iron_helm'"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "armor": -1}])", "record 11 'a': field 'armor': -1 is out of range"},
             {base + R"(,{"id": "a", "save_id": 256, "name": "A"}])", "field 'save_id': 256 is out of range"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "main_hand", "attack": {"min": 6, "max": 2, "speed": 1}}])",
              "field 'attack.max': 2 is out of range 6..1000"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "b", "name": "A"},{"id": "b", "save_id": 13, "copy_from": "a", "name": "B"}])",
              "field 'copy_from': a cycle, or a chain longer than 16"},
             {chain + "]", "'c16': field 'copy_from': a cycle, or a chain longer than 16"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": 5, "name": "A"}])", "record 11 'a': field 'copy_from': must be a string id"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "armour": 3}])", "record 11 'a': field 'armour': unknown field"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "hat"}])", "field 'slot': must be one of"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "main_hand"}])", "field 'attack': missing"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "attack": {"min": 1, "max": 2, "speed": 1}}])",
              "field 'attack': only main_hand and off_hand items attack"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "bag"}])", "field 'bag_slots': missing"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "bag_slots": 4}])", "field 'bag_slots': only bag items"},
             {base + R"(,{"id": "a", "save_id": 12.5, "name": "A"}])", "field 'save_id': must be an integer"},
             // Stat modifiers (ADR-022): a known stat and exactly one of flat, increased, more, on worn items only.
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": {"stat": "spirit", "flat": 1}}])",
              "record 11 'a': field 'mods': must be an array"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [5]}])", "field 'mods[0]': must be an object"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "luck", "flat": 1}]}])",
              "field 'mods[0].stat': must be one of stamina"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"flat": 1}]}])", "field 'mods[0].stat': must be one of"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "spirit"}]}])",
              "field 'mods[0]': needs exactly one of flat, increased, more"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "spirit", "flat": 1}, {"stat": "spirit", "flat": 1, "more": 0.1}]}])",
              "field 'mods[1]': needs exactly one of flat, increased, more"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "spirit", "flat": 1, "for": 5}]}])",
              "field 'mods[0].for': unknown field"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "spirit", "flat": "2"}]}])",
              "field 'mods[0].flat': must be a number"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "spirit", "increased": 20}]}])",
              "field 'mods[0].increased': 20 is out of range -1..10"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "spirit", "more": -2}]}])",
              "field 'mods[0].more': -2 is out of range -1..10"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "trinket", "mods": [{"stat": "armor", "flat": 5000}]}])",
              "field 'mods[0].flat': 5000 is out of range -1000..1000"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "mods": [{"stat": "spirit", "flat": 1}]}])",
              "field 'mods': only items worn in an equip slot have mods"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "bag", "bag_slots": 4, "mods": [{"stat": "spirit", "flat": 1}]}])",
              "field 'mods': only items worn in an equip slot have mods"},
             {base + R"(,{"id": "Bad Id", "save_id": 12, "name": "A"}])", "record 11: field 'id': must be a snake_case string"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "shield": {"size": "small", "negation": 1, "boost": 0.5}}])",
              "field 'shield': only off_hand items are shields"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "huge", "negation": 1, "boost": 0.5}}])",
              "field 'shield.size': must be one of small, medium, great"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "small", "boost": 0.5}}])",
              "field 'shield.negation': missing"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "great", "negation": 1, "boost": 0.7, "parry": 1}}])",
              "field 'shield.parry': greatshields can't parry"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "small", "negation": 1, "boost": 0.5, "weight": 3}}])",
              "field 'shield.weight': unknown field"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "bag", "bag_slots": 4, "weight": 1}])",
              "field 'weight': only items worn in an equip slot weigh on the equip load"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "weight": 101}])", "field 'weight': 101 is out of range 0..100"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "damage": "slash"}])",
              "field 'damage': only weapons (with an 'attack') do damage"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_shortsword", "name": "A", "damage": "fire"}])",
              "field 'damage': must be one of slash, strike, pierce"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "parry": 1}])",
              "field 'parry': only main_hand weapons parry (shields: shield.parry)"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "small", "negation": {"slash": 1, "fire": 1}, "boost": 0.5}}])",
              "field 'shield.negation.fire': unknown damage type"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "small", "negation": {"slash": 1}, "boost": 0.5}}])",
              "field 'shield.negation.strike': missing"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "small", "negation": "all", "boost": 0.5}}])",
              "field 'shield.negation': must be a number or an object {slash, strike, pierce}"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "off_hand", "shield": {"size": "small", "negation": 2, "boost": 0.5}}])",
              "field 'shield.negation': 2 is out of range 0..1"},
             {base + R"(,{"id": "a", "save_id": 12}])", "record 11 'a': field 'name': must be 1..48 bytes of text"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A‮B"}])", "field 'name': must be 1..48 bytes of text without control"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A\nB"}])", "field 'name': must be 1..48 bytes of text without control"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "armor": 1, "armor": 2}])", "record 11: field 'armor': duplicate key"},
             {base + R"(,{"id": "a", "save_id": 14, "name": "A"}])", "no record has save_id 12"},
             {R"([{"id": "a", "save_id": 1, "name": "A"}])", "save_id 1 must be 'iron_shortsword' (code names it), not 'a'"},
             {base, "not valid JSON"},
             {"{}", "must be an array of records"},
         }) {
        Defs d;
        CHECK(!load(text, d) && d.items.size() == 1);  // a failed load leaves its target alone
        if (error.find(want) == std::string::npos || error.find("items.json") == std::string::npos)
            std::println(stderr, "FAIL defs error: got \"{}\", want \"{}\"", error, want), ++failures;
    }
    fs::remove(dir / "items.json");
    CHECK(!loadDefs(dir, grown, error) && error.find("cannot open") != std::string::npos && grown.items.size() == 28);

    // Reloading may add items but never drop or repurpose a loaded save id; a refusal keeps the defs.
    {  // from just the code-named items (the shipped ones after them would be repurposed)
        Defs named;
        CHECK(load(base + "]", named));
        setDefs(std::move(named));
    }
    write(base + R"(,{"id": "dented_helm", "save_id": 12, "copy_from": "iron_helm", "name": "Dented Helm"}])");
    CHECK(reloadDefs(dir, error) && defs().items.size() == 13 && defs().itemIds[12] == "dented_helm");
    write(base + R"(,{"id": "spare_helm", "save_id": 12, "copy_from": "iron_helm", "name": "Spare Helm"}])");
    CHECK(!reloadDefs(dir, error) && error.find("save_id 12 is now 'spare_helm', was 'dented_helm'") != std::string::npos);
    write(base + "]");
    CHECK(!reloadDefs(dir, error) && error.find("11 items, fewer than the 12 loaded") != std::string::npos);
    write("[");
    CHECK(!reloadDefs(dir, error) && error.find("not valid JSON") != std::string::npos);
    CHECK(defs().items.size() == 13 && std::string_view(itemType(ItemId(12)).name) == "Dented Helm");

    // Creatures: save ids are the EntityKind values from 0; weapon and loot name items, model a body
    // model, temper one of three. Null removes an inherited name: the kind gets random villager names.
    write(base + R"(,{"id": "dented_helm", "save_id": 12, "copy_from": "iron_helm", "name": "Dented Helm"}])");
    std::string creatures = codeItemCreatures();
    creatures.erase(creatures.rfind(']'));
    auto writeCreatures = [&](std::string_view text) { std::ofstream(dir / "creatures.json") << text; };
    std::string scarecrow = creatures;
    scarecrow.replace(scarecrow.find(R"("id": "dummy")"), std::string_view(R"("id": "dummy")").size(), R"("id": "scarecrow")");
    CHECK(shipped.creatures.size() == size_t(EntityKind::Count) && shipped.creatureById.at("ghoul") == EntityKind::Ghoul &&
          shipped.creatures[1].name == nullptr && shipped.creatureIds[1] == "villager");
    writeCreatures(creatures + R"(,{"id": "townsfolk", "save_id": 8, "copy_from": "villager", "leash_radius": null}])");
    CHECK(loadDefs(dir, grown, error));  // null is no AI number: fine on a harmless kind
    if (!error.empty()) std::println(stderr, "FAIL null AI key: {}", error);
    writeCreatures(creatures + R"(,{"id": "boar_sow", "save_id": 8, "copy_from": "boar", "name": null, "stats": {"stamina": 14}, "loot": "wolf_pelt"}])");
    CHECK(loadDefs(dir, grown, error) && grown.creatures.size() == 9 && grown.creatureById.at("boar_sow") == EntityKind(8));
    if (!error.empty()) std::println(stderr, "FAIL grown creatures: {}", error);
    const EntityType& sow = grown.creatures[8];
    CHECK(sow.name == nullptr && sow.stats.stamina == 14 && sow.stats.strength == 2 && sow.model == ModelBoar &&
          sow.temper == Temper::Passive && sow.loot == WolfPelt && sow.lootChance == 0.75f && sow.respawnSeconds == 30);
    for (auto [text, want] : std::initializer_list<std::pair<std::string, std::string_view>>{
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "loot": "boar_hide"}])",
              "record 8 'a': field 'loot': unknown item \"boar_hide\""},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "weapon": 3}])", "field 'weapon': unknown item 3"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "weapon": "boar_skin"}])",
              "field 'weapon': 'boar_skin' is not a main_hand item"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "model": "dragon"}])", "record 8 'a': field 'model': must be one of"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "model": "sword"}])", "field 'model': must be one of"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "temper": "angry"}])",
              "field 'temper': must be one of harmless, passive, aggressive"},
             {creatures + R"(,{"id": "a", "save_id": 3, "copy_from": "boar"}])", "field 'save_id': 3 is already 'bandit'"},
             {creatures + R"(,{"id": "a", "save_id": -1, "copy_from": "boar"}])", "field 'save_id': -1 is out of range 0..255"},
             {creatures + R"(,{"id": "a", "save_id": 9, "copy_from": "boar"}])", "no record has save_id 8 (save ids run 0..9"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "aggro_range": 5}])",
              "field 'aggro_range': only aggressive creatures attack on sight"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "loot": null}])", "field 'loot_chance': no 'loot' to drop"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "stats": {"luck": 3}}])", "field 'stats.luck': unknown field"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "stats": {"stamina": null}}])", "field 'stats.stamina': missing"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "attackable": 1}])", "field 'attackable': must be true or false"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "stats": {"stamina": 0}}])", "field 'stats.stamina': 0 is out of range 1..1000"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "chase_speed": null}])",
              "field 'chase_speed': missing (a creature that fights chases)"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "loot_chance": 1.5}])", "field 'loot_chance': 1.5 is out of range"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "health": 50}])", "record 8 'a': field 'health': unknown field"},
             {creatures + R"(,{"id": "a", "save_id": 8, "model": "boar", "temper": "passive", "respawn_seconds": 5}])", "field 'level': missing"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "respawn_seconds": null}])", "field 'respawn_seconds': missing"},
             {creatures + R"(,{"id": "wolf", "save_id": 8, "copy_from": "boar"}])", "record 8 'wolf': field 'id': duplicate of record 4"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "leash_radius": 10, "reaggro_radius": 10}])",
              "field 'reaggro_radius': must be less than leash_radius"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "flee_health": 1}])", "field 'flee_health': 1 is out of range 0..0.95"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "attack_windup": 0}])", "field 'attack_windup': 0 is out of range"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "villager", "leash_radius": 10}])",
              "field 'leash_radius': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "give_up_seconds": "soon"}])", "field 'give_up_seconds': must be a number"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "villager", "stance": 10}])",
              "field 'stance': only attackable creatures are hit"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "dummy", "hit_stance": 10}])",
              "field 'hit_stance': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "dummy", "parries": 2}])", "field 'parries': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "parries": 2}])", "field 'parries': it can't be parried"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "damage": "pierce"}])",
              "field 'damage': its weapon decides (only creatures without one have their own)"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "dummy", "damage": "slash"}])", "field 'damage': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "wolf", "damage": "bite"}])", "field 'damage': must be one of slash, strike, pierce"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "villager", "resist": 0.2}])", "field 'resist': only attackable creatures are hit"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "wolf", "resist": {"slash": 2}}])", "field 'resist.slash': 2 is out of range -1..0.9"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "parryable": 1}])", "field 'parryable': must be true or false"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "villager", "combo": 2}])", "field 'combo': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "combo": 5}])", "field 'combo': 5 is out of range 1..4"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "combo": null, "delay": 0.3}])",
              "field 'delay': only a hit after the first is delayed"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "shield": "boar_skin"}])", "field 'shield': 'boar_skin' is not a shield"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "wolf", "shield": "boar_skin"}])", "field 'shield': only creatures with hands hold one"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "ghoul", "guard": 0.5}])", "field 'guard': bare-handed creatures don't guard"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit_chief", "parry": 0.2}])", "field 'parry': nothing it holds parries"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "bandit", "guard": 0.5, "parry": 0.5}])",
              "field 'dodge': guard + parry + dodge are chances of one answer"},
             {creatures + R"(,{"id": "a", "save_id": 8, "copy_from": "boar", "wanders": false}])",
              "field 'wanders': must be true for a creature that fights"},
             {scarecrow + "]",
              "save_id 0 must be 'dummy' (code names it), not 'scarecrow'"},
             {creatures, "not valid JSON"},
         }) {
        writeCreatures(text);
        Defs d;
        CHECK(!loadDefs(dir, d, error) && d.creatures.empty());  // a failed load leaves its target alone
        if (error.find(want) == std::string::npos || error.find("creatures.json") == std::string::npos)
            std::println(stderr, "FAIL creature defs error: got \"{}\", want \"{}\"", error, want), ++failures;
    }
    // Reloading may add creatures but never drop or repurpose one: spawn markers hold their save ids.
    writeCreatures(creatures + R"(,{"id": "boar_sow", "save_id": 8, "copy_from": "boar"}])");
    CHECK(reloadDefs(dir, error) && defs().creatures.size() == 9 && std::string_view(typeOf(EntityKind(8)).name) == "Boar");
    writeCreatures(creatures + R"(,{"id": "boar_runt", "save_id": 8, "copy_from": "boar"}])");
    CHECK(!reloadDefs(dir, error) && error.find("creature save_id 8 is now 'boar_runt', was 'boar_sow'") != std::string::npos);
    writeCreatures(creatures + "]");
    CHECK(!reloadDefs(dir, error) && error.find("8 creatures, fewer than the 9 loaded") != std::string::npos);
    CHECK(defs().creatures.size() == 9 && defs().creatureIds[8] == "boar_sow");
    // A kind only data has works end to end: marked by its id, listed, saved, loaded and spawned.
    Grid g({32, 32, 32});  // world files hold whole chunks
    for (int z = 0; z < 32; ++z)
        for (int x = 0; x < 32; ++x) g.set({x, 3, z}, Grass);
    std::vector<Marker> marks;
    std::vector<Instance> instances;
    EditHistory history;
    PrefabLibrary prefabs = loadPrefabs("no-such-folder");
    auto run = [&](std::string_view request) { return runCommand(request, g, instances, marks, history, prefabs, GameView{}).response; };
    nlohmann::json r = run(R"({"cmd": "mark", "args": {"kind": "spawn", "x": 10, "z": 10, "entity": "boar_sow"}})");
    CHECK(r["ok"] == true && marks.size() == 1 && marks[0].entity == EntityKind(8) && r["result"]["marker"]["entity"] == "boar_sow" &&
          marks[0].radius == 10);
    r = run(R"({"cmd": "catalog"})");
    CHECK(r["result"]["entities"].size() == 9 && r["result"]["entities"][8]["id"] == "boar_sow");
    std::string sowWorld = (dir / "sow.flyw").string();
    std::string saved = saveWorld(sowWorld.c_str(), g, 1, instances, marks);
    CHECK(saved.empty());
    if (!saved.empty()) std::println(stderr, "FAIL save: {}", saved);
    Grid back({1, 1, 1});
    uint32_t seed = 0;
    std::optional<std::vector<Marker>> loaded;
    CHECK(loadWorld(sowWorld.c_str(), back, seed, instances, loaded).empty() && loaded && *loaded == marks);
    std::vector<Entity> sows;
    if (loaded) syncSpawns(sows, back, *loaded, 1, {}, 0.5f);
    CHECK(sows.size() == 1 && sows[0].kind == EntityKind(8) && std::string_view(sows[0].name) == "Boar");
    setDefs(std::move(shipped));  // the other tests see exactly the shipped items and creatures
    // Without that kind the world is refused, saying why.
    CHECK(loadWorld(sowWorld.c_str(), back, seed, instances, loaded) == "bad marker 1: unknown entity kind");
    fs::remove_all(dir);
}

// Voxel light (light.h): sky fill, ember falloff, relight after edits (incremental == full fill,
// across chunk borders), light in the quads and the remesh covering where light changed.
static bool sameLight(const Light& a, const Light& b) {
    for (int y = 0; y < a.size.y; ++y)
        for (int z = 0; z < a.size.z; ++z)
            for (int x = 0; x < a.size.x; ++x)
                if (a.get({x, y, z}) != b.get({x, y, z})) return false;
    return true;
}

// A streamed grid (a window onto the endless world, its storage wrapping in x and z) holds and
// lights the same voxels as a plain grid shifted to its corner: chunk slots, light, meshes, and an
// edit and its undo across the wrap seam.
static void streamedGrids() {
    Grid d({128, 64, 128});
    std::mt19937 rng(5);
    for (int z = 0; z < 128; ++z)
        for (int x = 0; x < 128; ++x) {
            int h = 10 + int(rng() % 20);
            for (int y = 0; y <= h; ++y) d.set({x, y, z}, y == h ? Grass : Stone);
            if (rng() % 97 == 0) d.set({x, h + 1, z}, Ember);
        }
    for (int i = 0; i < 40; ++i) {  // overhangs, so light spreads sideways under them
        glm::ivec3 c(int(rng() % 120), 32 + int(rng() % 20), int(rng() % 120));
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x) d.set(c + glm::ivec3(x, 0, z), Leaves);
    }
    const glm::ivec3 lo(-1056, 0, 4064);  // chunk-aligned; the seam (a multiple of 128) at x = -1024, z = 4096
    Grid s = Grid::streamed(d.size, lo);
    for (int y = 0; y < 64; ++y)
        for (int z = 0; z < 128; ++z)
            for (int x = 0; x < 128; ++x) s.set(lo + glm::ivec3(x, y, z), d.get({x, y, z}));
    CHECK(s.streaming() && !d.streaming() && s.hi() == lo + glm::ivec3(127, 63, 127));
    CHECK(s.get(lo - glm::ivec3(1, 0, 0)) == Air && !s.inside({0, 10, 0}));
    bool same = true, slots = true;
    for (int y = 0; y < 64; ++y)
        for (int z = 0; z < 128; ++z)
            for (int x = 0; x < 128; ++x) same &= s.get(lo + glm::ivec3(x, y, z)) == d.get({x, y, z});
    for (size_t i = 0; i < 4 * 2 * 4; ++i) {
        glm::ivec3 o = s.chunkOrigin(i);
        slots &= s.chunkSlot(o) == i && s.inside(o) && s.inside(o + 31) && o % 32 == glm::ivec3(0);
    }
    CHECK(same && slots);

    Light ld, ls;
    fillLight(d, ld);
    fillLight(s, ls);
    auto sameShifted = [&] {
        for (int y = 0; y < 64; ++y)
            for (int z = 0; z < 128; ++z)
                for (int x = 0; x < 128; ++x)
                    if (ls.get(lo + glm::ivec3(x, y, z)) != ld.get({x, y, z})) return false;
        return true;
    };
    CHECK(sameShifted() && ls.get(lo - glm::ivec3(0, 0, 1)) == 0xF0);
    std::vector<Mesh> md = meshChunks(d, &ld), ms = meshChunks(s, &ls);  // quads are chunk-local: equal
    bool meshes = true;
    for (size_t i = 0; i < ms.size(); ++i) meshes &= ms[i].opaque == md[d.chunkSlot(s.chunkOrigin(i) - lo)].opaque;
    CHECK(meshes);

    // A fill across both seams, its relight and remesh, and its undo, as on the plain grid.
    EditHistory hd, hs;
    std::vector<Instance> none;
    std::vector<Marker> marks;
    EditOp fill{.tool = Tool::Fill, .min = {28, 20, 28}, .max = {36, 40, 36}, .block = Stone};
    EditResult rd = applyEdit(d, fill, hd);
    fill.min += lo, fill.max += lo;
    EditResult rs = applyEdit(s, fill, hs);
    CHECK(!rs.error && rs.dirty.lo == rd.dirty.lo + lo && rs.dirty.hi == rd.dirty.hi + lo);
    CHECK(s.get(lo + glm::ivec3(31, 30, 33)) == Stone && s.get(lo + glm::ivec3(32, 30, 32)) == Stone);
    Box ld2 = relight(d, ld, rd.dirty), ls2 = relight(s, ls, rs.dirty);
    CHECK(ls2.lo == ld2.lo + lo && ls2.hi == ld2.hi + lo && sameShifted());
    std::vector<size_t> id = remeshChunks(d, ld2.lo, ld2.hi, md, &ld), is = remeshChunks(s, ls2.lo, ls2.hi, ms, &ls);
    meshes = id.size() == is.size();
    for (size_t i = 0; i < ms.size(); ++i) meshes &= ms[i].opaque == md[d.chunkSlot(s.chunkOrigin(i) - lo)].opaque;
    CHECK(meshes);
    undoEdit(d, hd, none, marks);
    undoEdit(s, hs, none, marks);
    same = true;
    for (int y = 0; y < 64; ++y)
        for (int z = 0; z < 128; ++z)
            for (int x = 0; x < 128; ++x) same &= s.get(lo + glm::ivec3(x, y, z)) == d.get({x, y, z});
    CHECK(same);

    // Moving the window a chunk east: the slot of the chunk that left now holds the one that came in.
    size_t slot = s.chunkSlot(lo);
    s.lo.x += 32;
    CHECK(s.chunkOrigin(slot) == lo + glm::ivec3(128, 0, 0) && !s.inside(lo) && s.inside(lo + glm::ivec3(128, 0, 0)));

    // Plant cells pack x and z modulo 1024 and unpack against the window, negative coordinates too.
    for (glm::ivec3 w : {glm::ivec3(-1056, 0, 4064), glm::ivec3(-512, 0, -512), glm::ivec3(0)})
        for (glm::ivec3 p : {w, w + glm::ivec3(1023, 191, 1023), w + glm::ivec3(500, 40, 3)}) {
            uint32_t cell = uint32_t(p.x & 1023) | uint32_t(p.y) << 10 | uint32_t(p.z & 1023) << 18;  // as meshRegion
            CHECK(plantCell(cell, w) == p);
        }

    // What the endless world refuses or skips: a relight outside the window (it moved on), being
    // written to a world file (a wrapped window isn't a world), and commands other than looking around.
    CHECK(relight(s, ls, {{5000, 10, 5000}, {5004, 12, 5004}}).empty());
    std::string path = (std::filesystem::temp_directory_path() / "flycoos_streamed_test.flyw").string();
    CHECK(!saveWorld(path.c_str(), s, 1, {}, {}).empty() && !std::filesystem::exists(path));
    PrefabLibrary prefabs = loadPrefabs("no-such-folder");
    std::vector<Instance> list;
    EditHistory h;
    GameView view;
    auto ok = [&](std::string_view request) { return runCommand(request, s, list, marks, h, prefabs, view).response["ok"].get<bool>(); };
    CHECK(ok(R"({"cmd": "state"})") && ok(R"({"cmd": "goto", "args": {"position": [-90000, 80, 5]}})"));
    CHECK(!ok(R"({"cmd": "raise", "args": {"x": -1000, "z": 4100}})") && !ok(R"({"cmd": "save", "args": {"path": "worlds/x.flyw"}})"));
}

// Streaming (stream.h): the window fills from worker threads; a chunk is meshed once everything
// it reads is final, so the voxels and light of every meshed column match a fixed grid's; moving
// the window drops the columns behind (emptied) and brings in the ones ahead.
static void streaming() {
    const glm::ivec3 size(256, 128, 256);
    Grid w = Grid::streamed(size, {-128, 0, -128});
    Light l(w);
    Stream stream(w, l, 7, 2);
    std::set<size_t> meshed;
    auto settle = [&] {  // until every column is in and nothing more can be lit or meshed
        for (int i = 0; i < 20000; ++i) {
            Stream::Update u = stream.update(0);
            for (size_t s : u.empty) meshed.erase(s);
            meshed.insert(u.mesh.begin(), u.mesh.end());
            if (stream.stats().queued == 0 && u.mesh.empty() && !stream.stats().litNow && !stream.stats().installed) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    auto matches = [&](int want) {  // voxels and light of the meshed columns against a fixed grid around the window
        Grid f(size + glm::ivec3(64, 0, 64), w.lo - glm::ivec3(32, 0, 32));
        generateTerrain(f, 7);
        Light fl;
        fillLight(f, fl);
        bool voxels = true, light = true;
        int columns = 0;
        for (int cz = w.lo.z; cz <= w.hi().z; cz += kChunk)
            for (int cx = w.lo.x; cx <= w.hi().x; cx += kChunk) {
                if (stream.stage({cx, 0, cz}) != Stream::Stage::Meshed) continue;
                ++columns;
                for (int y = 0; y < size.y; ++y)
                    for (int z = cz; z < cz + kChunk; ++z)
                        for (int x = cx; x < cx + kChunk; ++x) {
                            voxels &= w.get({x, y, z}) == f.get({x, y, z});
                            light &= l.get({x, y, z}) == fl.get({x, y, z});
                        }
            }
        return voxels && light && columns == want;
    };
    CHECK(settle() && matches(4 * 4));  // all but two rings: the edge (never lit) and the next
    CHECK(meshed.size() == size_t(4 * 4 * 4) && stream.meshedAround({0, 0, 0}, 1) && !stream.meshedAround({0, 0, 0}, 2));

    stream.follow({100, 50, 10});  // three columns east: the window's middle is now column (3, 0)
    CHECK(w.lo == glm::ivec3(-32, 0, -128) && l.lo == w.lo && stream.stats().dropped == 3 * 8);
    CHECK(w.get({-100, 60, 0}) == Air && !stream.generated({150, 0, 120}));  // dropped; queued
    // Meshed: columns 1..4 now, plus -1 and 0 of before, still right (now in the outer rings); -2 emptied.
    CHECK(settle() && matches(6 * 4) && meshed.size() == size_t(6 * 4 * 4));

    // Back west: the dropped columns come back the same; then a jump far away drops every column.
    stream.follow({10, 50, 10});
    // Meshed: columns -2..1 again, and 2..3 still from before (now in the outer rings), rows -2..1.
    CHECK(w.lo == glm::ivec3(-128, 0, -128) && settle() && matches(6 * 4) && meshed.size() == size_t(6 * 4 * 4));
    stream.follow({-70000, 50, 33000});
    CHECK(stream.stats().dropped == 8 * 8 && settle() && matches(4 * 4) && meshed.size() == size_t(4 * 4 * 4));
}

static void lighting() {
    Grid g({16, 16, 16});
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
            for (int y = 0; y < 4; ++y) g.set({x, y, z}, Stone);  // ground top at y = 4
    Light l;
    fillLight(g, l);
    CHECK(l.get({5, 4, 5}) == 0xF0 && l.get({5, 15, 5}) == 0xF0);  // open sky: 15, no block light
    CHECK(l.get({5, 2, 5}) == 0);                                     // inside the ground
    CHECK(l.get({-1, 4, 5}) == 0xF0 && l.allocated() == 1);          // outside: open sky

    // An 8x8 roof at y = 8 over x, z in [4, 11]: below it, 15 minus the steps to the nearest open column.
    for (int z = 4; z <= 11; ++z)
        for (int x = 4; x <= 11; ++x) g.set({x, 8, z}, Stone);
    Box roof{{4, 8, 4}, {11, 8, 11}};
    Box changed = relight(g, l, roof);
    CHECK(skyLight(l.get({7, 5, 7})) == 11 && skyLight(l.get({4, 4, 4})) == 14 && skyLight(l.get({7, 9, 7})) == 15);
    CHECK(!changed.empty() && changed.lo.y == 4 && changed.hi.y == 8);  // the air under the roof and the roof
    Light fresh;
    fillLight(g, fresh);
    CHECK(sameLight(l, fresh));

    // An ember under the roof floods block light, -1 per block; removing it restores the darkness.
    g.set({7, 4, 7}, Ember);
    relight(g, l, {{7, 4, 7}, {7, 4, 7}});
    CHECK(blockLight(l.get({7, 4, 7})) == emission(Ember) && blockLight(l.get({8, 4, 7})) == emission(Ember) - 1);
    CHECK(blockLight(l.get({10, 4, 7})) == emission(Ember) - 3 && blockLight(l.get({7, 7, 9})) == emission(Ember) - 5);
    CHECK(blockLight(l.get({7, 9, 7})) == 0);  // the roof stops it (around the roof is 15 steps or more)
    CHECK(skyLight(l.get({7, 4, 7})) == 0);    // an ember is opaque: no sky light in it
    g.set({7, 4, 7}, Air);
    relight(g, l, {{7, 4, 7}, {7, 4, 7}});
    CHECK(blockLight(l.get({10, 4, 7})) == 0 && skyLight(l.get({7, 4, 7})) == 11);
    g.set({7, 8, 7}, Air);  // a hole in the roof: open sky falls straight down through it
    relight(g, l, {{7, 8, 7}, {7, 8, 7}});
    CHECK(skyLight(l.get({7, 4, 7})) == 15 && skyLight(l.get({8, 4, 7})) == 14);
    fillLight(g, fresh);
    CHECK(sameLight(l, fresh));

    // Leaves and water pass light at a cost of 1 per block, but only air passes open sky down for free.
    {
        Grid w({8, 16, 8});
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x) {
                w.set({x, 0, z}, Stone);
                for (int y = 1; y < 12; ++y) w.set({x, y, z}, x < 4 ? Leaves : Water);
            }
        Light wl;
        fillLight(w, wl);
        CHECK(skyLight(wl.get({1, 11, 1})) == 14 && skyLight(wl.get({1, 9, 1})) == 12);  // leaves
        CHECK(skyLight(wl.get({6, 11, 6})) == 14 && skyLight(wl.get({6, 9, 6})) == 12);  // water
    }

    {  // light from outside the relit columns comes back in through their border
        Grid w({64, 8, 4});
        for (int z = 0; z < 4; ++z)
            for (int x = 0; x < 64; ++x) w.set({x, 0, z}, Stone), w.set({x, 3, z}, Stone);
        w.set({10, 1, 1}, Ember);
        Light wl, full;
        fillLight(w, wl);
        w.set({30, 1, 1}, Stone);  // relights x in [15, 45]; the ember at x = 10 still reaches x = 15
        relight(w, wl, {{30, 1, 1}, {30, 1, 1}});
        fillLight(w, full);
        CHECK(blockLight(wl.get({15, 1, 1})) == emission(Ember) - 5 && sameLight(wl, full));
    }

    {  // a lantern is a dimmer emitter
        Grid w({16, 4, 4});
        w.set({2, 1, 1}, Lantern);
        Light wl;
        fillLight(w, wl);
        CHECK(emission(Lantern) == 11 && blockLight(wl.get({2, 1, 1})) == 11 && blockLight(wl.get({5, 1, 1})) == 8);
    }

    // Packing: light round-trips through the quad.
    for (QuadInfo q : {QuadInfo{{3, 4, 5}, 2, 3, 2, {3, 3, 2, 1}, Planks, 9, 13}, QuadInfo{{0, 0, 0}, 32, 32, 5, {0, 0, 0, 0}, Water, 0, 0},
                       QuadInfo{{31, 31, 31}, 1, 1, 1, {3, 3, 3, 3}, Ember, 15, 15}})
        CHECK(unpack(pack(q)) == q);
    {  // the mesher takes each face's light from the voxel in front of it
        Mesh m = meshRegion(g, {0, 0, 0}, g.size, &l);
        bool found = false;
        for (Quad q : m.opaque) {
            QuadInfo i = unpack(q);
            if (i.face != 2 || i.pos.y != 3 || i.pos.x > 9 || i.pos.x + i.w <= 9 || i.pos.z > 5 || i.pos.z + i.h <= 5) continue;
            found = true;
            CHECK(i.sky == skyLight(l.get({9, 4, 5})) && i.sky < 15 && i.glow == 0);
        }
        CHECK(found);
        CHECK(meshRegion(g, {0, 0, 0}, g.size).opaque != m.opaque);  // without light: all open sky
    }

    // The seed-7 world chunkMeshes pins, lit. Random edits across the
    // chunk borders relight to exactly a full fill, and remeshing the edit plus the changed light
    // matches meshing from scratch (no seams).
    Grid t({64, 128, 64});
    generateTerrain(t, 7);
    Light tl;
    fillLight(t, tl);
    std::vector<Mesh> meshes = meshChunks(t, &tl);
    size_t opaque = 0, water = 0;
    for (const Mesh& m : meshes) opaque += m.opaque.size(), water += m.water.size();
    int shaded = 0;
    for (const Mesh& m : meshes)
        for (Quad q : m.opaque) shaded += unpack(q).sky < kMaxLight;
    // Light splits 1 more face than unlit (7673). (The story world, with houses and the barrow, went
    // from 1772666 to 1863601 opaque quads with light, +5.1%.)
    CHECK(opaque == 7674 && water == 27 && shaded == 628);
    std::mt19937 rng(11);
    for (int i = 0; i < 8; ++i) {
        glm::ivec3 lo(int(rng() % 8) + 26, int(rng() % 40) + 30, int(rng() % 8) + 26);  // around the x = z = 32 borders
        glm::ivec3 hi = lo + glm::ivec3(int(rng() % 9), int(rng() % 3), int(rng() % 9));
        Block b = std::array{Stone, Air, Ember, Leaves, Water}[rng() % 5];
        for (int y = lo.y; y <= hi.y; ++y)
            for (int z = lo.z; z <= hi.z; ++z)
                for (int x = lo.x; x <= hi.x; ++x) t.set({x, y, z}, b);
        std::vector<uint8_t> before;
        for (int y = 0; y < t.size.y; ++y)
            for (int z = 0; z < t.size.z; ++z)
                for (int x = 0; x < t.size.x; ++x) before.push_back(tl.get({x, y, z}));
        Box lit = relight(t, tl, {lo, hi});
        fillLight(t, fresh);
        CHECK(sameLight(tl, fresh));
        bool inside = true;  // every voxel whose light changed is in the returned box
        for (int y = 0, k = 0; y < t.size.y; ++y)
            for (int z = 0; z < t.size.z; ++z)
                for (int x = 0; x < t.size.x; ++x, ++k)
                    if (before[size_t(k)] != tl.get({x, y, z}))
                        inside &= !lit.empty() && glm::all(glm::greaterThanEqual(glm::ivec3(x, y, z), lit.lo)) &&
                                  glm::all(glm::lessThanEqual(glm::ivec3(x, y, z), lit.hi));
        CHECK(inside);
        remeshChunks(t, lit.empty() ? lo : glm::min(lo, lit.lo), lit.empty() ? hi : glm::max(hi, lit.hi), meshes, &tl);
        std::vector<Mesh> scratch = meshChunks(t, &tl);
        bool same = true;
        for (size_t c = 0; c < scratch.size(); ++c) same &= scratch[c].opaque == meshes[c].opaque && scratch[c].water == meshes[c].water;
        CHECK(same);
    }
}

// Quests (ADR-026): the event drain, the condition evaluator, the quest loader and the QUST/FLAG save
// sections, and Act 1's "Torvald's Helm" played through step() on a scratch world.
static void quests() {
    namespace fs = std::filesystem;
    const Defs& shipped = defs();
    CHECK(shipped.questById.contains("torvalds_helm") && shipped.warnings.empty());
    const auto helm = size_t(shipped.questById.at("torvalds_helm"));
    auto flat = [] {  // 128 x 128 of grass, no water
        Grid g({128, 8, 128});
        for (int z = 0; z < 128; ++z)
            for (int x = 0; x < 128; ++x) {
                for (int y = 0; y < 3; ++y) g.set({x, y, z}, Dirt);
                g.set({x, 3, z}, Grass);
            }
        return g;
    };
    auto spawn = [](uint32_t id, const char* name, EntityKind kind, glm::vec3 pos, std::vector<std::string> lines = {}) {
        return Marker{.id = id, .kind = MarkerKind::Spawn, .name = name, .pos = pos, .radius = 2, .entity = kind, .count = 1, .lines = std::move(lines)};
    };
    std::vector<Marker> act1 = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}},
                                spawn(2, "Maren Holt", EntityKind::Villager, {20.5f, 4, 20.5f}, {"Welcome, traveler."}),
                                spawn(3, "Torvald", EntityKind::Villager, {60.5f, 4, 20.5f}, {"Iron? They took it all."}),
                                spawn(4, "Captain Brenna", EntityKind::Guard, {100.5f, 4, 20.5f}, {"Halt.", "Move along."}),
                                spawn(5, "Hakon the Red", EntityKind::BanditChief, {64.5f, 4, 100.5f}),
                                {.id = 6, .kind = MarkerKind::Region, .name = "Greyfang Rise", .pos = {64.5f, 4, 100.5f}, .radius = 12}};
    CHECK(checkQuestMarkers(shipped, act1).empty());
    {  // names that don't resolve to exactly one marker are warned about
        std::vector<Marker> twice = act1;
        twice.push_back(spawn(7, "Torvald", EntityKind::Villager, {70.5f, 4, 30.5f}));
        twice.erase(twice.begin() + 1);  // no Maren
        std::vector<std::string> w = checkQuestMarkers(shipped, twice);
        CHECK(w.size() == 2 && std::ranges::count_if(w, [](const std::string& x) { return x.contains("no spawn marker named 'Maren Holt'"); }) == 1 &&
              std::ranges::count_if(w, [](const std::string& x) { return x.contains("more than one spawn marker named 'Torvald'"); }) == 1);
    }

    Session s{.world = flat(), .seed = 9};
    s.markers = act1;
    startSession(s);
    auto npc = [&](const char* name) -> Entity& {
        return *std::ranges::find_if(s.entities, [&](const Entity& e) { return e.spawn.name == name; });
    };
    SimInput idle, interact;
    interact.interact = true;
    // Walks up to `name` and presses E: the line it says (picked before the quests react).
    auto talkTo = [&](const char* name) {
        s.player.teleport(npc(name).body.pos + glm::vec3(1.5f, 0, 0));
        step(s, interact, kTick);
        const char* said = npc(name).speech();
        return std::string(said ? said : "");
    };
    const QuestState& q = s.quests[helm];
    auto objective = [&](const char* id) {
        const auto& os = shipped.quests[helm].objectives;
        return q.objectives[size_t(std::ranges::find(os, std::string(id), &Objective::id) - os.begin())];
    };
    CHECK(q.stage == 0 && !q.active() && q.objectives.size() == 3);
    CHECK(talkTo("Captain Brenna") == "Halt.");  // nothing passes: the marker's own lines cycle
    CHECK(talkTo("Captain Brenna") == "Move along.");
    // Answered from the state the player walked up in (stage 0's line), then the quest starts.
    CHECK(talkTo("Maren Holt").starts_with("Hakon the Red's raiders stripped") && q.stage == 10 && objective("talk_torvald") == ObjectiveState::Shown);
    CHECK(s.toasts.size() == 1 && s.toasts[0].second == "Quest started: Torvald's Helm");
    s.toasts.clear();
    CHECK(talkTo("Maren Holt").starts_with("Torvald's forge is on the east side"));
    CHECK(talkTo("Torvald").starts_with("Maren sent you?") && q.stage == 20);
    CHECK(objective("talk_torvald") == ObjectiveState::Completed && objective("get_helm") == ObjectiveState::Shown);
    CHECK(talkTo("Torvald").starts_with("Greyfang Rise, past the south-east fields"));
    CHECK(talkTo("Torvald") == "Iron? They took it all.");  // the same quest line again: the marker's own line's turn
    CHECK(talkTo("Torvald").starts_with("Greyfang Rise, past"));
    CHECK(talkTo("Captain Brenna").starts_with("Going after Hakon?"));
    CHECK(talkTo("Maren Holt").starts_with("Greyfang Rise is south-east"));

    // Kill Hakon with the sword: the hit that kills raises Killed, his corpse holds the helm.
    CHECK(quickMove(s.player.inv, {SlotRef::Item, 0}) && s.player.inv.equipped[SlotMainHand] == Shortsword);
    Entity& hakon = npc("Hakon the Red");
    s.player.teleport(hakon.body.pos + glm::vec3(0, 0, -1.2f));
    hakon.body.health = 1;
    for (int i = 0; i < 120 && hakon.body.alive(); ++i) {
        SimInput swing;
        swing.attack = true;
        glm::vec3 d = hakon.body.pos - s.player.pos;
        swing.aim = glm::normalize(glm::vec2(d.x, d.z));
        step(s, swing, kTick);
    }
    CHECK(!hakon.body.alive() && hakon.loot == IronHelm && q.stage == 25);
    CHECK(std::ranges::any_of(s.eventLog, [](const std::string& l) { return l.contains("killed bandit_chief of #5 Hakon the Red"); }));
    CHECK(std::ranges::any_of(s.eventLog, [](const std::string& l) { return l.contains("entered #6 Greyfang Rise"); }));
    CHECK(talkTo("Captain Brenna").starts_with("Word is Hakon fell"));  // npc_dead
    CHECK(talkTo("Torvald").starts_with("Hakon's down?"));
    s.player.teleport(hakon.body.pos + glm::vec3(1, 0, 0));
    step(s, interact, kTick);  // loot: the count changes this tick, so has_item fires in the same drain
    CHECK(countItem(s.player.inv, IronHelm) == 1 && q.stage == 30);
    CHECK(objective("get_helm") == ObjectiveState::Completed && objective("return_helm") == ObjectiveState::Shown);
    CHECK(talkTo("Maren Holt").starts_with("Is that Torvald's helm?"));
    // Wearing it still counts as having it; Torvald takes it off your head and gives the cuirass.
    CHECK(quickMove(s.player.inv, {SlotRef::Item, int(std::ranges::find(s.player.inv.containers[0], IronHelm) - s.player.inv.containers[0].begin())}));
    CHECK(s.player.inv.equipped[SlotHead] == IronHelm);
    s.toasts.clear();
    CHECK(talkTo("Torvald").starts_with("That's my mark on the brow!"));
    CHECK(q.done && q.stage == 40 && objective("return_helm") == ObjectiveState::Completed);
    CHECK(countItem(s.player.inv, IronHelm) == 0 && countItem(s.player.inv, IronCuirass) == 1 && s.flags.at("ravensford_friend") == 1);
    CHECK(s.toasts.size() == 1 && s.toasts[0].second == "Quest complete: Torvald's Helm");
    CHECK(talkTo("Torvald").starts_with("Wear that cuirass well"));
    CHECK(talkTo("Maren Holt").starts_with("Torvald sings at his anvil"));
    CHECK(talkTo("Maren Holt") == "Welcome, traveler." && talkTo("Maren Holt").starts_with("Torvald sings"));  // they take turns
    CHECK(talkTo("Captain Brenna").starts_with("So you're the one"));  // the flag
    {  // a shout on turning angry says only the marker's lines
        Entity& brenna = npc("Captain Brenna");
        brenna.speechTimer = brenna.shoutTimer = 0;
        brenna.state = AiState::Wander;
        setState(brenna, AiState::Chase, "damaged");
        std::string shout = brenna.speech() ? brenna.speech() : "";
        CHECK(shout == "Halt." || shout == "Move along.");
        setState(brenna, AiState::Return, "forced");
    }
    for (int i = 0; i < 4; ++i) talkTo("Torvald");  // a done quest listens no more
    CHECK(countItem(s.player.inv, IronCuirass) == 1);
    setQuestStage(s, helm, 20);  // nor does it enter stages
    CHECK(q.stage == 40 && q.done);

    // The hand-over never loses the reward: with the bags full it goes into the empty chest slot;
    // with that taken too the trigger waits (and says so) until there is room.
    auto handOver = [&](bool chestFree) {
        Session h{.world = flat(), .seed = 9};
        h.markers = act1;
        startSession(h);
        setQuestStage(h, helm, 30);
        Inventory& inv = h.player.inv;
        while (giveItem(inv, BoarSkin)) {}
        inv.equipped[SlotHead] = IronHelm;
        if (!chestFree) inv.equipped[SlotChest] = IronCuirass;
        Entity& torvald = *std::ranges::find_if(h.entities, [](const Entity& e) { return e.spawn.name == "Torvald"; });
        h.player.teleport(torvald.body.pos + glm::vec3(1.5f, 0, 0));
        h.toasts.clear();
        step(h, interact, kTick);
        return h;
    };
    {
        Session h = handOver(true);
        CHECK(h.quests[helm].done && h.player.inv.equipped[SlotChest] == IronCuirass && countItem(h.player.inv, IronHelm) == 0);
        h = handOver(false);
        CHECK(h.quests[helm].stage == 30 && !h.quests[helm].done && countItem(h.player.inv, IronHelm) == 1 && countItem(h.player.inv, IronCuirass) == 1);
        CHECK(h.toasts.size() == 1 && h.toasts[0].second == "Make room in your bags: Torvald's Helm");
        CHECK(discardItem(h.player.inv, {SlotRef::Item, 3}));  // destroy a boar skin, talk again
        Entity& torvald = *std::ranges::find_if(h.entities, [](const Entity& e) { return e.spawn.name == "Torvald"; });
        h.player.teleport(torvald.body.pos + glm::vec3(1.5f, 0, 0));
        step(h, interact, kTick);
        CHECK(h.quests[helm].done && countItem(h.player.inv, IronCuirass) == 2 && h.player.inv.containers[0][3] == IronCuirass);
    }
    {  // a helm-first player: Torvald takes it on the first talk, whatever the stage before 20
        for (int start : {0, 10}) {
            Session h{.world = flat(), .seed = 9};
            h.markers = act1;
            startSession(h);
            if (start) setQuestStage(h, helm, start);
            CHECK(giveItem(h.player.inv, IronHelm));
            step(h, idle, kTick);
            Entity& torvald = *std::ranges::find_if(h.entities, [](const Entity& e) { return e.spawn.name == "Torvald"; });
            h.player.teleport(torvald.body.pos + glm::vec3(1.5f, 0, 0));
            step(h, interact, kTick);
            CHECK(std::string_view(torvald.speech() ? torvald.speech() : "").starts_with("That's my mark on the brow!"));
            CHECK(h.quests[helm].done && countItem(h.player.inv, IronHelm) == 0 && countItem(h.player.inv, IronCuirass) == 1);
        }
    }

    // Save round trip: stage, objectives, done and the flags, by string id.
    fs::path dir = fs::temp_directory_path() / "flycoos_quest_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "quests");
    std::string path = (dir / "q.flys").string();
    s.flags["negative"] = -7;
    CHECK(savePlayer(path.c_str(), s, "q.flyw", 1).empty());
    Session t{.world = flat(), .seed = 9};
    t.markers = act1;
    startSession(t);
    SaveReport r;
    CHECK(loadPlayer(path.c_str(), t, 1, r).empty() && r.notes.empty());
    CHECK(t.quests[helm].stage == 40 && t.quests[helm].done && !t.quests[helm].failed && t.quests[helm].objectives == q.objectives);
    CHECK(t.flags == s.flags && t.flags.at("negative") == -7);
    {  // crafted QUST and FLAG sections: an unknown quest and objective are dropped with notes, bad ones reject the file
        auto le = [](std::vector<uint8_t>& out, uint64_t v, int n) {
            for (int i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i)));
        };
        auto str = [&](std::vector<uint8_t>& out, std::string_view v) { out.push_back(uint8_t(v.size())), out.insert(out.end(), v.begin(), v.end()); };
        auto craft = [&](std::vector<std::pair<std::string, std::vector<uint8_t>>> sections) {
            std::vector<uint8_t> out{'F', 'L', 'Y', 'S', 1, 0, 0, 0};
            std::vector<uint8_t> head{0};
            le(head, 1, 4), le(head, 9, 4), le(head, std::bit_cast<uint64_t>(5.0), 8), le(head, std::bit_cast<uint32_t>(0.5f), 4);
            sections.insert(sections.begin(), {"HEAD", head});
            for (auto& [tag, payload] : sections) {
                out.insert(out.end(), tag.begin(), tag.end());
                le(out, payload.size(), 8);
                out.insert(out.end(), payload.begin(), payload.end());
            }
            le(out, fnv1a(out.data(), out.size()), 4);
            std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        };
        auto quest = [&](std::string_view id, uint32_t stage, uint8_t bits, std::vector<std::pair<std::string_view, uint8_t>> objectives) {
            std::vector<uint8_t> v;
            str(v, id), le(v, stage, 4), v.push_back(bits), v.push_back(uint8_t(objectives.size()));
            for (auto [o, state] : objectives) str(v, o), v.push_back(state);
            return v;
        };
        auto quests = [&](std::vector<std::vector<uint8_t>> list) {
            std::vector<uint8_t> v;
            le(v, list.size(), 4);
            for (auto& one : list) v.insert(v.end(), one.begin(), one.end());
            return v;
        };
        fs::remove(path + ".bak");
        craft({{"QUST", quests({quest("lost_quest", 3, 0, {}), quest("torvalds_helm", 20, 0, {{"get_helm", 1}, {"old_objective", 2}})})}});
        Session u = t;
        CHECK(loadPlayer(path.c_str(), u, 1, r).empty() && r.notes.size() == 2 && r.notes[0].contains("dropped quest 'lost_quest'") &&
              r.notes[1].contains("dropped objective 'old_objective'"));
        CHECK(u.quests[helm].stage == 20 && !u.quests[helm].done && objective("get_helm") == ObjectiveState::Completed &&
              u.quests[helm].objectives[1] == ObjectiveState::Shown && u.quests[helm].objectives[0] == ObjectiveState::Hidden && u.flags.empty());
        craft({{"QUST", quests({quest("torvalds_helm", 15, 0, {})})}});
        CHECK(loadPlayer(path.c_str(), u, 1, r).empty() && r.notes.size() == 1 && r.notes[0].contains("stage 15 is gone") && u.quests[helm].stage == 15);
        std::vector<uint8_t> flags;
        le(flags, 2, 4), str(flags, "a"), le(flags, 1, 4), str(flags, "a"), le(flags, 2, 4);  // the same flag twice
        std::vector<uint8_t> badName;
        le(badName, 1, 4), str(badName, "a\nb"), le(badName, 1, 4);
        std::vector<uint8_t> hugeQuests, hugeFlags;
        le(hugeQuests, 70000, 4), le(hugeFlags, 5000, 4);
        std::vector<std::vector<std::pair<std::string, std::vector<uint8_t>>>> broken = {
            {{"QUST", quests({quest("torvalds_helm", 20, 4, {})})}},             // unknown bits
            {{"QUST", quests({quest("torvalds_helm", 20, 3, {})})}},             // done and failed
            {{"QUST", quests({quest("torvalds_helm", 70000, 0, {})})}},          // stage out of range
            {{"QUST", quests({quest("torvalds_helm", 20, 0, {{"get_helm", 9}})})}},  // no such objective state
            {{"QUST", quests({quest("torvalds_helm", 20, 0, {{"get_helm", 1}, {"get_helm", 2}})})}},
            {{"QUST", quests({quest("torvalds_helm", 20, 0, {}), quest("torvalds_helm", 10, 0, {})})}},
            {{"QUST", quests({quest("", 20, 0, {})})}},
            {{"QUST", hugeQuests}},
            {{"QUST", [&] { auto v = quests({quest("torvalds_helm", 20, 0, {})}); v.pop_back(); return v; }()}},  // truncated
            {{"QUST", [&] { auto v = quests({quest("torvalds_helm", 20, 0, {})}); v.push_back(0); return v; }()}},  // left over
            {{"FLAG", flags}},
            {{"FLAG", badName}},
            {{"FLAG", hugeFlags}},
            {{"QUST", quests({})}, {"QUST", quests({})}},
        };
        for (auto& sections : broken) {
            craft(sections);
            Session v = t;
            CHECK(!loadPlayer(path.c_str(), v, 1, r).empty() && v.quests[helm].stage == 40 && v.flags.size() == 2);
        }
    }

    // Data: a quest exercising every trigger, the drain order and the loader's checks, in a scratch data folder.
    for (const char* f : {"items.json", "creatures.json", "skills.json", "talents.json"})
        fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / f, dir / f);
    std::string error;
    auto load = [&](std::string_view quest, Defs& d) {
        std::ofstream(dir / "quests" / "a.json", std::ios::trunc) << quest;
        return loadDefs(dir, d, error);
    };
    Defs scratch;
    CHECK(load(R"([{"id": "pond", "title": "The Pond", "objectives": [{"id": "boar", "text": "Kill a boar"}],
        "stages": [
          {"id": 0, "on": [{"entered": "Pond", "do": [{"set_stage": 10}]}]},
          {"id": 10, "journal": "At the pond.", "actions": [{"show_objective": "boar"}, {"give_item": "rusty_knife"}, {"start_quest": "pond_b"}],
           "on": [{"has_item": "rusty_knife", "do": [{"set_stage": 20}]}]},
          {"id": 20, "journal": "Armed.", "on": [{"killed_creature": "boar", "if": [{"in_region": "Pond", "not": true}], "do": [{"set_stage": 30}]},
                                                 {"killed_creature": "boar", "do": [{"set_flag": "pond_kills", "value": 1}]}]},
          {"id": 30, "actions": [{"complete_objective": "boar"}, {"set_stage": 31}]},
          {"id": 31, "actions": [{"set_stage": 30}]}
        ],
        "lines": [{"spawn": "Boar", "say": "Oink."}, {"spawn": "Boar", "if": [{"done": "pond"}], "say": "Never."}]},
       {"id": "pond_b", "title": "Started", "stages": [{"id": 10, "on": [{"entered": "Pond", "do": [{"set_stage": 20}]}]}, {"id": 20}]}])", scratch));
    if (!error.empty()) std::println(stderr, "FAIL scratch quest: {}", error);
    CHECK(scratch.quests.size() == 2 && scratch.questLines.size() == 2 && scratch.warnings.size() == 1 &&
          scratch.warnings[0].contains("field 'lines[1]': never said: an earlier line for 'Boar' has no conditions"));
    setDefs(std::move(scratch));
    {
        Session p{.world = flat(), .seed = 3};
        p.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}},
                     {.id = 2, .kind = MarkerKind::Region, .name = "Pond", .pos = {40.5f, 4, 40.5f}, .radius = 5},
                     spawn(3, "Boar", EntityKind::Boar, {80.5f, 4, 80.5f})};
        startSession(p);
        const QuestState& pond = p.quests[0];
        step(p, idle, kTick);
        CHECK(pond.stage == 0);
        p.player.teleport({40.5f, 4, 40.5f});
        step(p, idle, kTick);  // RegionEntered: stage 10 gives the knife; its has_item re-check waits for the next batch
        CHECK(pond.stage == 10 && countItem(p.player.inv, Knife) == 1 && pond.objectives[0] == ObjectiveState::Shown);
        CHECK(p.events.size() == 1 && std::holds_alternative<ItemCountChanged>(p.events[0]));
        // pond_b was started on that RegionEntered: it didn't hear the same event (quest order doesn't matter).
        CHECK(p.quests[1].stage == 10);
        // has_item reads the count now, not the event's: the knife gone, the stale re-check doesn't fire.
        CHECK(takeItem(p.player.inv, Knife) && !takeItem(p.player.inv, Knife));
        step(p, idle, kTick);
        CHECK(pond.stage == 10);
        CHECK(giveItem(p.player.inv, Knife));
        p.events.push_back(ItemCountChanged{Knife, 1});  // and one count per item per batch (with step's own)
        size_t logged = p.eventLog.size();
        step(p, idle, kTick);
        CHECK(pond.stage == 20 && p.events.empty() && p.eventLog.size() == logged + 2);  // the event, the stage
        // The kill in the pond doesn't pass the first trigger's condition; the second one sets the flag.
        Entity& boar = p.entities[0];
        p.events.push_back(Killed{EntityKind::Boar, 3});
        step(p, idle, kTick);
        CHECK(pond.stage == 20 && p.flags.at("pond_kills") == 1);
        p.player.teleport({10.5f, 4, 10.5f});
        CHECK(damage(boar, 1000));
        p.events.push_back(Killed{boar.kind, boar.spawn.id});
        step(p, idle, kTick);  // stages 30 and 31 set each other: the chain stops, the log says so
        CHECK((pond.stage == 30 || pond.stage == 31) && pond.objectives[0] == ObjectiveState::Completed);
        CHECK(std::ranges::any_of(p.eventLog, [](const std::string& l) { return l.contains("set_stage chain longer than 16"); }));
        for (int i = 0; i < 40; ++i) p.events.push_back(TalkedTo{3});
        step(p, idle, kTick);
        CHECK(p.eventLog.size() == kEventLogSize && p.eventLog.back().contains("talked to #3 Boar"));
        p.entities[0].body.health = 10;
        p.entities[0].spawn.name = "Boar";
        CHECK(pickLine(p, p.entities[0]) == 0);  // the unconditioned line shadows the later one
    }
    {  // a reload keeps quest handles and objective indices: renaming an objective is refused, appending one is fine
        std::ostringstream text;
        text << std::ifstream(dir / "quests" / "a.json").rdbuf();
        std::string shipped = text.str(), renamed = shipped, grown = shipped;
        auto swap = [](std::string& in, std::string_view from, std::string_view to) { in.replace(in.find(from), from.size(), to); };
        swap(renamed, R"("id": "boar")", R"("id": "hog")");
        swap(renamed, R"("show_objective": "boar")", R"("show_objective": "hog")");
        swap(renamed, R"("complete_objective": "boar")", R"("complete_objective": "hog")");
        swap(grown, R"({"id": "boar", "text": "Kill a boar"})", R"({"id": "boar", "text": "Kill a boar"}, {"id": "more", "text": "More"})");
        std::ofstream(dir / "quests" / "a.json", std::ios::trunc) << renamed;
        CHECK(!reloadDefs(dir, error) && error.contains("objective 0 is now 'hog', was 'boar'"));
        std::ofstream(dir / "quests" / "a.json", std::ios::trunc) << grown;
        CHECK(reloadDefs(dir, error) && defs().quests[0].objectives.size() == 2);
        if (!error.empty()) std::println(stderr, "FAIL reload: {}", error);
    }
    // Loader errors name file, record and field.
    std::string file = (dir / "quests" / "a.json").string();
    auto quest = [](std::string_view stages, std::string_view more = "") {
        return std::format(R"([{{"id": "q", "title": "Q", "objectives": [{{"id": "o", "text": "Do it"}}], "stages": [{}]{}}}])", stages, more);
    };
    for (auto [text, want] : std::initializer_list<std::pair<std::string, std::string_view>>{
             {quest(R"({"id": 10, "actions": [{"give_item": "gold"}]})"), "record 0 'q': field 'stages[0].actions[0].give_item': unknown item \"gold\""},
             {quest(R"({"id": 10, "on": [{"talked_to": "X", "do": [{"set_stage": 50}]}]})"), "field 'stages[0].on[0].do[0].set_stage': 50 is not one of"},
             {quest(R"({"id": 10, "actions": [{"show_objective": "p"}]})"), "field 'stages[0].actions[0].show_objective': quest 'q' has no objective \"p\""},
             {quest(R"({"id": 10, "on": [{"talked_to": "X", "killed": "Y", "do": [{"set_stage": 10}]}]})"), "field 'stages[0].on[0]': needs exactly one of talked_to"},
             {quest(R"({"id": 10, "on": [{"talked_to": "X"}]})"), "field 'stages[0].on[0].do': missing"},
             {quest(R"({"id": 10, "on": [{"killed_creature": "dragon", "do": [{"set_stage": 10}]}]})"), "field 'stages[0].on[0].killed_creature': unknown creature"},
             {quest(R"({"id": 10, "jornal": "x"})"), "field 'stages[0].jornal': unknown field"},
             {quest(R"({"id": 10}, {"id": 10})"), "field 'stages[1].id': stage 10 is listed twice"},
             {quest(R"({"id": 0, "journal": "x"}, {"id": 10})"), "field 'stages[0]': stage 0 (not started) has only triggers"},
             {quest(R"({"id": 0})"), "field 'stages': needs a stage other than 0"},
             {quest(R"({"id": 70000})"), "field 'stages[0].id': 70000 is out of range"},
             {quest(R"({"id": 10, "on": [{"entered": "R", "if": [{"stage": "nope", "value": 1}], "do": [{"set_stage": 10}]}]})"),
              "field 'stages[0].on[0].if[0].stage': unknown quest \"nope\""},
             {quest(R"({"id": 10})", R"(, "lines": [{"spawn": "A", "if": [{"flag": "f", "op": "=>", "value": 1}], "say": "Hi"}])"),
              "field 'lines[0].if[0].op': must be one of ==, !="},
             {quest(R"({"id": 10})", R"(, "lines": [{"spawn": "A", "if": [{"objective": "o", "is": "shown"}], "say": "Hi"}])"),
              "field 'lines[0].if[0].objective': must be \"quest_id.objective_id\""},
             {quest(R"({"id": 10})", R"(, "lines": [{"spawn": "A", "if": [{"objective": "q.o", "is": "open"}], "say": "Hi"}])"),
              "field 'lines[0].if[0].is': must be one of hidden"},
             {quest(R"({"id": 10})", R"(, "lines": [{"spawn": "A", "if": [{"has_item": "iron_helm", "done": "q"}], "say": "Hi"}])"),
              "field 'lines[0].if[0]': needs exactly one of stage"},
             {quest(R"({"id": 10})", R"(, "lines": [{"spawn": "A", "say": ""}])"), "field 'lines[0].say': must be 1..160 bytes"},
             {quest(R"({"id": 10})", R"(, "lines": [{"spawn": "A", "if": [{"flag": "f"}], "say": "Hi"}])"), "field 'lines[0].if[0].value': missing"},
             {quest(R"({"id": 10, "actions": [{"set_flag": "Bad Name", "value": 1}]})"), "field 'stages[0].actions[0].set_flag': must be a snake_case id"},
             {quest(R"({"id": 10, "actions": [{"complete_quest": "q", "value": 1}]})"), "field 'stages[0].actions[0].value': unknown field"},
             {quest(R"({"id": 10})", R"(, "copy_from": "x")"), "record 0 'q': field 'copy_from': unknown field"},
             {quest(R"({"id": 10, "actions": [{"take_item": "canvas_satchel"}]})"), "field 'stages[0].actions[0].take_item': a bag can't be taken"},
         }) {
        Defs bad;
        CHECK(!load(text, bad));
        if (error.find(want) == std::string::npos || !error.starts_with(file)) std::println(stderr, "FAIL quest error '{}', want '{}'", error, want);
        CHECK(error.find(want) != std::string::npos && error.starts_with(file));
    }
    {  // two files with the same quest id
        std::ofstream(dir / "quests" / "b.json") << quest(R"({"id": 10})");
        Defs bad;
        CHECK(!load(quest(R"({"id": 10})"), bad) && error.contains("b.json: record 0 'q': field 'id': another quest file has this id"));
        fs::remove(dir / "quests" / "b.json");
    }
    {  // set back the shipped data for the tests after this one
        Defs again;
        CHECK(loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", again, error));
        setDefs(std::move(again));
    }
}

// Every condition kind against hand-set state, its operator and "not".
static void conditions() {
    const auto q = defs().questById.at("torvalds_helm");
    Session s{.world = meadow(), .seed = 1};
    s.markers = {{.id = 1, .kind = MarkerKind::Region, .name = "Vale", .pos = {20.5f, 4, 20.5f}, .radius = 30},
                 {.id = 2, .kind = MarkerKind::Region, .name = "Village", .pos = {10.5f, 4, 10.5f}, .radius = 5},
                 {.id = 3, .kind = MarkerKind::Spawn, .name = "Raiders", .pos = {20.5f, 4, 40.5f}, .radius = 2, .entity = EntityKind::Bandit, .count = 2}};
    startSession(s);
    s.player.teleport({10.5f, 4, 10.5f});
    auto is = [&](Condition c) { return passes(s, c); };
    using C = Condition;
    s.quests[q].stage = 20;
    CHECK(is({.kind = C::Stage, .op = Cmp::Eq, .value = 20, .quest = q}) && !is({.kind = C::Stage, .op = Cmp::Ne, .value = 20, .quest = q}));
    CHECK(is({.kind = C::Stage, .op = Cmp::Lt, .value = 21, .quest = q}) && !is({.kind = C::Stage, .op = Cmp::Lt, .value = 20, .quest = q}));
    CHECK(is({.kind = C::Stage, .op = Cmp::Le, .value = 20, .quest = q}) && is({.kind = C::Stage, .op = Cmp::Gt, .value = 19, .quest = q}));
    CHECK(is({.kind = C::Stage, .op = Cmp::Ge, .value = 20, .quest = q}) && !is({.kind = C::Stage, .op = Cmp::Gt, .value = 20, .quest = q}));
    CHECK(!is({.kind = C::Stage, .negate = true, .op = Cmp::Eq, .value = 20, .quest = q}));
    CHECK(!is({.kind = C::Done, .quest = q}) && is({.kind = C::Done, .negate = true, .quest = q}) && !is({.kind = C::Failed, .quest = q}));
    s.quests[q].failed = true;
    CHECK(is({.kind = C::Failed, .quest = q}) && !s.quests[q].active());
    s.quests[q].objectives[1] = ObjectiveState::Shown;
    CHECK(is({.kind = C::Objective, .value = int(ObjectiveState::Shown), .quest = q, .objective = 1}));
    CHECK(is({.kind = C::Objective, .value = int(ObjectiveState::Hidden), .quest = q, .objective = 0}));
    CHECK(!is({.kind = C::Objective, .value = int(ObjectiveState::Completed), .quest = q, .objective = 1}));
    CHECK(is({.kind = C::Flag, .op = Cmp::Eq, .value = 0, .name = "unset"}));  // an unset flag is 0
    s.flags["f"] = -3;
    CHECK(is({.kind = C::Flag, .op = Cmp::Lt, .value = 0, .name = "f"}) && !is({.kind = C::Flag, .op = Cmp::Ge, .value = -2, .name = "f"}));
    CHECK(is({.kind = C::HasItem, .op = Cmp::Ge, .value = 1, .item = Shortsword}) && !is({.kind = C::HasItem, .op = Cmp::Ge, .value = 1, .item = IronHelm}));
    CHECK(giveItem(s.player.inv, IronHelm) && giveItem(s.player.inv, IronHelm) && quickMove(s.player.inv, {SlotRef::Item, 2}));
    CHECK(countItem(s.player.inv, IronHelm) == 2 && countItem(s.player.inv, NoItem) == 0);  // one worn, one in the backpack
    CHECK(is({.kind = C::HasItem, .op = Cmp::Ge, .value = 2, .item = IronHelm}) && !is({.kind = C::HasItem, .op = Cmp::Ge, .value = 3, .item = IronHelm}));
    CHECK(quickMove(s.player.inv, {SlotRef::Item, 1}) && is({.kind = C::HasItem, .op = Cmp::Eq, .value = 1, .item = Satchel}));  // on the bag bar
    CHECK(is({.kind = C::InRegion, .name = "Village"}) && is({.kind = C::InRegion, .name = "Vale"}));  // nested: both count
    CHECK(!is({.kind = C::InRegion, .name = "Raiders"}) && is({.kind = C::InRegion, .negate = true, .name = "Elsewhere"}));
    CHECK(s.entities.size() == 2 && !is({.kind = C::NpcDead, .name = "Raiders"}) && !is({.kind = C::NpcDead, .name = "Nobody"}));
    CHECK(damage(s.entities[0], 1000) && !is({.kind = C::NpcDead, .name = "Raiders"}));
    CHECK(damage(s.entities[1], 1000) && is({.kind = C::NpcDead, .name = "Raiders"}));
    CHECK(passesAll(s, {}) && !passesAll(s, {{.kind = C::NpcDead, .name = "Raiders"}, {.kind = C::Done, .quest = q}}));
    // Loot and rewards go into an empty matching equip slot when the bags are full; Ctrl+right-click destroys.
    {
        Inventory inv;
        while (giveItem(inv, BoarSkin)) {}
        CHECK(!canStow(inv, WolfPelt) && canStow(inv, IronHelm) && stowItem(inv, IronHelm) && inv.equipped[SlotHead] == IronHelm);
        CHECK(!canStow(inv, IronHelm) && stowItem(inv, Satchel) && inv.bags[0] == Satchel && inv.containers[1].size() == size_t(itemType(Satchel).bagSlots));
        Entity& corpse = s.entities[0];
        corpse.loot = IronHelm;
        Player looter;
        looter.inv = inv;
        looter.pos = corpse.body.pos;
        while (giveItem(looter.inv, BoarSkin)) {}
        CHECK(lootTarget(s.entities, looter) == nullptr);  // head taken, bags full
        CHECK(discardItem(looter.inv, {SlotRef::Equip, SlotHead}) && lootTarget(s.entities, looter) == &corpse);
        CHECK(takeLoot(corpse, looter.inv) && looter.inv.equipped[SlotHead] == IronHelm && corpse.loot == NoItem);
        CHECK(!discardItem(looter.inv, {SlotRef::Bag, 0}));  // a bag that holds items stays
        CHECK(discardItem(looter.inv, {SlotRef::Item, 0, 0}) && looter.inv.containers[0][0] == NoItem);
        for (ItemId& id : looter.inv.containers[1]) id = NoItem;
        CHECK(discardItem(looter.inv, {SlotRef::Bag, 0}) && looter.inv.bags[0] == NoItem && looter.inv.containers[1].empty());
        CHECK(!discardItem(looter.inv, {SlotRef::Bag, 0}) && takeItem(looter.inv, IronHelm) && looter.inv.equipped[SlotHead] == NoItem);
    }
    // A quest handle a reload dropped reads as not started.
    CHECK(is({.kind = C::Stage, .op = Cmp::Eq, .value = 0, .quest = 999}));
}

// Progression (ADR-027): Skyrim's formulas reproduced, skill and character XP from the one award on
// the damage path and its anti-farm guards, talents (canLearn, their modifiers, respec), the
// level-band fight invariant, the PROG save section and the data files.
static void progression() {
    namespace fs = std::filesystem;
    // Skyrim's numbers from UESP (Skyrim:Leveling) first: the formulas with its constants.
    int toLevel81 = 0;
    for (int n = 1; n < 81; ++n) toLevel81 += levelXpToNext(n);
    CHECK(toLevel81 == 87000 && levelXpTotal(81) == 87000 && levelXpTotal(1) == 0 && levelXpTotal(2) == 100);
    auto skillChar = [](int from, int to) {  // character XP for raising one skill from..to
        int n = 0;
        for (int l = from + 1; l <= to; ++l) n += characterXpForSkill(l);
        return n;
    };
    CHECK(skillChar(15, 100) == 4930);
    // Its 18 skills (twelve start at 15, five at 20, one at 25) all at 100: 88,085 XP, the old level 81 cap.
    int all = 12 * skillChar(15, 100) + 5 * skillChar(20, 100) + skillChar(25, 100);
    CHECK(all == 88085);
    Progress sky;
    int skyLevel = 1;
    CHECK(addCharacterXp(sky, skyLevel, all) == 80 && skyLevel == 81 && sky.xp == all - 87000);
    double combat = 0, smithing = 0;  // XP to raise a skill 15 -> 100: combat (2, 0) 528,804, smithing (0.25, 300) 91,600
    for (int l = 15; l < 100; ++l) combat += skillXpToNext(2, 0, l), smithing += skillXpToNext(0.25f, 300, l);
    CHECK(std::abs(combat - 528804) < 2 && std::abs(smithing - 91600) < 2);

    // flycoos' numbers (data/skills.json): a few sword levels and three character levels for Act 1.
    const SkillDef &blade = skillDef(Skill::OneHanded), &fist = skillDef(Skill::Unarmed), &plate = skillDef(Skill::Armor);
    CHECK(blade.name == "One-Handed" && fist.name == "Unarmed" && plate.name == "Armor");
    Progress g;
    int level = 1;
    float first = skillXpToNext(blade.improveMult, blade.improveOffset, 1);
    CHECK(addSkillXp(g, level, Skill::OneHanded, first - 1) == 0 && g.skills[0].level == 1);
    CHECK(addSkillXp(g, level, Skill::OneHanded, 1) == 1 && g.skills[0].level == 2 && g.skills[0].xp == 0 && g.xp == 2);  // + its new level
    CHECK(addSkillXp(g, level, Skill::OneHanded, -5) == 0 && addSkillXp(g, level, Skill::OneHanded, std::nanf("")) == 0);
    CHECK(addCharacterXp(g, level, 98) == 1 && level == 2 && g.xp == 0 && talentPoints(g, level) == 1);
    CHECK(addCharacterXp(g, level, 125 + 150) == 2 && level == 4 && g.xp == 0);  // 2 -> 3 costs 125, 3 -> 4 150
    level = kMaxLevel - 1;
    CHECK(addCharacterXp(g, level, 1 << 30) == 1 && level == kMaxLevel && g.xp == 0);  // capped
    g.skills[0] = {kMaxSkill, 0};
    CHECK(addSkillXp(g, level, Skill::OneHanded, 1e9f) == 0);
    Progress big;  // a huge use stops at the cap, never loops forever
    int bigLevel = 1;
    CHECK(addSkillXp(big, bigLevel, Skill::Unarmed, 1e9f) == kMaxSkill - kMinSkill && big.skills[1].level == kMaxSkill && bigLevel > 10);

    // The award on the damage path (awardHitXp): only attackable NPCs, base damage, wear per NPC.
    Grid world = meadow();
    auto one = [&](EntityKind kind) {
        std::vector<Entity> es;
        syncSpawns(es, world, {{.id = 1, .kind = MarkerKind::Spawn, .pos = {10.5f, 4, 10.5f}, .entity = kind}}, 1, {}, 0.5f);
        return es.at(0);
    };
    Entity bandit = one(EntityKind::Bandit), dummy = one(EntityKind::TrainingDummy), villager = one(EntityKind::Villager);
    Player hero;
    hero.swingWeapon = Shortsword;
    auto bladeXp = [&] { return hero.progress.skills[size_t(Skill::OneHanded)].xp; };
    float sword = skillUseXp(blade.useMult, blade.useOffset, 8);  // the shortsword's 6-10 averages 8
    awardHitXp(hero, bandit, true, false);
    CHECK(bladeXp() == sword && bandit.xpWear == 1);
    awardHitXp(hero, dummy, true, false);
    CHECK(std::abs(bladeXp() - sword * (1 + kTrainingRate)) < 1e-4f);  // the dummy teaches at a quarter
    awardHitXp(hero, villager, true, true);
    CHECK(std::abs(bladeXp() - sword * (1 + kTrainingRate)) < 1e-4f && hero.progress.xp == 0 && villager.xpWear == 0);  // can't be hurt: nothing
    // The final hit doesn't matter: strength, damage modifiers and talents earn no more per hit.
    Player strong = hero;
    strong.progress = {};
    strong.stats.strength = 50;
    strong.inv.equipped[SlotTrinket] = BoneCharm;
    Entity fresh = one(EntityKind::Bandit);
    awardHitXp(strong, fresh, true, false);
    CHECK(strong.progress.skills[0].xp == sword);
    Player puncher;  // a punch (1-4) is unarmed XP: use_mult x 2.5
    Entity fresh2 = one(EntityKind::Bandit);
    awardHitXp(puncher, fresh2, true, false);
    CHECK(puncher.progress.skills[size_t(Skill::Unarmed)].xp == skillUseXp(fist.useMult, fist.useOffset, 2.5f) && puncher.progress.skills[0].xp == 0);
    // The same target teaches less and less: halved after kXpWearHalving hits.
    Player grinder;
    grinder.swingWeapon = Shortsword;
    Entity punchbag = one(EntityKind::Bandit);
    std::vector<float> per;
    for (int i = 0; i < 41; ++i) {
        float before = grinder.progress.skills[0].xp + 1000 * float(grinder.progress.skills[0].level);
        awardHitXp(grinder, punchbag, true, false);
        per.push_back(grinder.progress.skills[0].xp + 1000 * float(grinder.progress.skills[0].level) - before);
    }
    CHECK(std::abs(per[20] - sword / 2) < 0.01f && std::abs(per[40] - sword / 4) < 0.01f && per[0] == sword);
    // Kills: the creature's xp, once per kill (the default is 10 x level for hostile ones; the chief's is its own).
    CHECK(bandit.type().xp == 20 && dummy.type().xp == 0 && villager.type().xp == 0 && typeOf(EntityKind::BanditChief).xp == 60);
    Player killer;
    Entity victim = one(EntityKind::Bandit);
    awardHitXp(killer, victim, true, true);
    CHECK(killer.progress.xp == 20);
    awardHitXp(killer, dummy, true, true);
    CHECK(killer.progress.xp == 20);
    // Armor XP: from the NPC's hits, only while wearing armor, by its weapon's base damage (the knife's 3-6).
    Player bare, armored;
    armored.inv.equipped[SlotHead] = IronHelm;
    Entity knifer = one(EntityKind::Bandit), knifer2 = one(EntityKind::Bandit);
    knifer.body.swingWeapon = knifer2.body.swingWeapon = Knife;
    awardHitXp(bare, knifer, false, false);
    awardHitXp(armored, knifer2, false, false);
    CHECK(bare.progress.skills[size_t(Skill::Armor)].xp == 0 && armored.progress.skills[size_t(Skill::Armor)].xp == skillUseXp(plate.useMult, plate.useOffset, 4.5f));
    // Farming the dummy for ten minutes (it falls and gets back up every few seconds) barely moves Unarmed.
    {
        Player farmer;
        farmer.pos = {14.5f, 4, 10.5f};
        std::vector<Entity> es;
        syncSpawns(es, world, {{.id = 1, .kind = MarkerKind::Spawn, .pos = {13.5f, 4, 10.5f}, .entity = EntityKind::TrainingDummy}}, 1, {}, 0.5f);
        std::mt19937 rng(1);
        int punches = 0;
        for (int i = 0; i < 600 * 60; ++i) {
            updateEntities(es, world, farmer, kTick);
            punches += startPunch(farmer, {-1, 0});
            updateCombat(farmer, es, kTick, rng);
        }
        const SkillLevel& u = farmer.progress.skills[size_t(Skill::Unarmed)];
        float earned = u.xp + (u.level > 1 ? skillXpToNext(fist.improveMult, fist.improveOffset, 1) : 0);
        float fullRate = float(punches) * skillUseXp(fist.useMult, fist.useOffset, 2.5f);
        std::println("dummy farm: {} punches in 10 min, {:.0f} unarmed XP ({:.1f}% of full rate), level {}", punches, earned,
                     100 * earned / fullRate, u.level);
        CHECK(punches > 500 && u.level <= 2 && earned < 0.05f * fullRate && farmer.progress.xp < 5);
    }

    // Talents: canLearn's rules, one at a time.
    const Defs& d = defs();
    auto id = [&](const char* name) { return size_t(d.talentById.at(name)); };
    size_t keen = id("keen_edge"), quick = id("quick_wrist"), steady = id("steady_hand"), deep = id("deep_cuts"), worn = id("worn_in");
    const char* why = nullptr;
    Progress t;
    CHECK(!canLearn(t, 1, keen, &why) && std::string_view(why).starts_with("no talent point"));
    CHECK(!canLearn(t, 1, steady, &why) && std::string_view(why).starts_with("needs more points"));  // the rules before the budget
    CHECK(canLearn(t, 2, keen, &why) && !why && learn(t, 2, keen) && rank(t, keen) == 1 && talentPoints(t, 2) == 0 && !learn(t, 2, keen));
    CHECK(!canLearn(t, 20, steady, &why) && std::string_view(why).starts_with("needs more points"));  // tier 1: 3 points in the tree
    CHECK(learn(t, 20, quick) && !canLearn(t, 20, steady) && learn(t, 20, worn) && !canLearn(t, 20, steady));  // armor points don't count
    CHECK(learn(t, 20, keen) && canLearn(t, 20, steady) && pointsSpent(t, Skill::OneHanded) == 3 && pointsSpent(t, Skill::Armor) == 1);
    CHECK(learn(t, 20, quick) && learn(t, 20, quick) && !canLearn(t, 20, quick, &why) && std::string_view(why) == "at its highest rank");
    CHECK(pointsSpent(t, Skill::OneHanded) == 5 && learn(t, 20, steady) && pointsSpent(t, Skill::OneHanded) == 6);
    CHECK(!canLearn(t, 20, deep, &why) && std::string_view(why).starts_with("needs its prerequisite"));  // keen_edge 2 of 5
    while (learn(t, 20, keen)) {}
    CHECK(rank(t, keen) == 5 && canLearn(t, 20, deep) && !canLearn(t, 20, d.talents.size()));
    // Their modifiers go into the sheet; "with one_handed" ones only into sword hits, never the plain sheet.
    Player fighter;
    fighter.inv.equipped[SlotMainHand] = Shortsword;
    fighter.inv.equipped[SlotChest] = IronCuirass;
    fighter.level = 4;
    StatSheet plain = sheet(fighter), withBlade = sheet(fighter, Skill::OneHanded);
    CHECK(plain.v == withBlade.v);
    CHECK(learn(fighter.progress, 4, keen) && learn(fighter.progress, 4, keen) && learn(fighter.progress, 4, worn));
    CHECK(std::abs(sheet(fighter, Skill::OneHanded)[Stat::Damage] - 1.06f) < 1e-5f && sheet(fighter)[Stat::Damage] == 1 &&
          sheet(fighter, Skill::Unarmed)[Stat::Damage] == 1);
    CHECK(std::abs(sheet(fighter)[Stat::Armor] - 12 * 1.04f) < 1e-4f);
    fighter.progress.skills[size_t(Skill::OneHanded)].level = 6;  // +2% per level gained: 1 + 0.06 + 0.10
    CHECK(std::abs(sheet(fighter, Skill::OneHanded)[Stat::Damage] - 1.16f) < 1e-5f);
    {  // a rank above the max (data lowered it since) counts as the max
        Player over = fighter;
        over.progress.talents.assign(d.talents.size(), 0);
        over.progress.talents[keen] = 9;
        CHECK(std::abs(sheet(over, Skill::OneHanded)[Stat::Damage] - (1.15f + 0.10f)) < 1e-5f);  // 5 x 3%, + 2% x 5 levels
    }
    bool sourced = false;
    for (const Modifier& m : collect(fighter)) sourced |= std::string_view(m.source) == "Keen Edge" && m.with == Skill::OneHanded;
    CHECK(sourced);
    {  // resolveHit reads them through the swing's weapon: a fixed 8 roll, 2 strength, +16%
        Player a = fighter, b;
        a.swingWeapon = Shortsword;
        std::mt19937 rng(1);
        CHECK(resolveHit(a, b, Attack{8, 8, 1}, {rng}) == 12);  // 10 x 1.16 = 11.6
        a.swingWeapon = NoItem;  // a punch: none of the blade's
        CHECK(resolveHit(a, b, Attack{8, 8, 1}, {rng}) == 10);
    }
    fighter.progress.skills = {};
    respec(fighter.progress);
    CHECK(sheet(fighter).v == plain.v && sheet(fighter, Skill::OneHanded).v == withBlade.v && talentPoints(fighter.progress, 4) == 3);

    // Level band: a level-N player in the Iron set with the sword, its N - 1 points in the blade tree
    // and the skills clearing Act 1 gives at that pace (level 4: about One-Handed 6, Armor 5), against
    // each fighter of level N (wolf and boar 1, bandit 2, chief and ghoul 3, guard 4). Time to kill both
    // ways: swings x the time between them (the player's swing time; an NPC's swing time, or its
    // windup + recover + cooldown if longer). NPCs wear no armor yet, so the player's level pierces
    // nothing; this pins the band for when gear tiers and armored NPCs come.
    for (size_t k = 0; k < d.creatures.size(); ++k) {
        const EntityType& foe = d.creatures[k];
        if (foe.temper == Temper::Harmless || !foe.attackable) continue;
        Player p;
        for (ItemId item : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, Shortsword}) p.inv.equipped[itemType(item).slot] = item;
        p.swingWeapon = Shortsword;
        p.level = foe.level;
        p.progress.skills[size_t(Skill::OneHanded)].level = 1 + 5 * (foe.level - 1) / 3;
        p.progress.skills[size_t(Skill::Armor)].level = 1 + 4 * (foe.level - 1) / 3;
        for (size_t tl = 0; tl < d.talents.size(); ++tl)
            while (d.talents[tl].tree == Skill::OneHanded && learn(p.progress, p.level, tl)) {}
        CHECK(talentPoints(p.progress, p.level) == 0);
        Player npc;
        npc.stats = foe.stats, npc.level = foe.level, npc.swingWeapon = npc.inv.equipped[SlotMainHand] = foe.weapon;
        std::mt19937 rng(7);
        double dealt = 0, taken = 0;
        for (int i = 0; i < 2000; ++i) dealt += resolveHit(p, npc, weaponAttack(Shortsword), {rng}), taken += resolveHit(npc, p, weaponAttack(foe.weapon), {rng});
        int toKill = int(std::ceil(maxHealth(sheet(npc)) / (dealt / 2000))), toDown = int(std::ceil(maxHealth(sheet(p)) / (taken / 2000)));
        float killTime = float(toKill) * swingTime(sheet(p, Skill::OneHanded), weaponAttack(Shortsword));
        float downTime = float(toDown) * std::max(swingTime(sheet(npc, skillOf(foe.weapon)), weaponAttack(foe.weapon)), foe.windup + foe.recover + foe.cooldown);
        std::println("level band: level {} {}: {} swings ({:.1f} s) to kill it, it needs {} ({:.1f} s) to down the player", foe.level,
                     d.creatureIds[k], toKill, killTime, toDown, downTime);
        CHECK(toKill >= 6 && toKill <= 16);  // never a one-shot, never a slog
        // A same-level fight alone is won. The exception: guards are the village's watch, meant to be
        // tougher than anything a player of their level should take on (passive: they never start it).
        if (d.creatureIds[k] == "guard") CHECK(downTime < killTime && foe.temper == Temper::Passive);
        else CHECK(downTime > killTime);
    }
    // Kill XP wears too: killing the same respawning guard again and again gives less each time.
    {
        Player slayer;
        slayer.swingWeapon = Shortsword;
        slayer.pos = {40.5f, 4, 40.5f};
        slayer.progress.skills[0].level = kMaxSkill;  // no skill levels, so only the kill adds character XP
        std::vector<Entity> gs;
        syncSpawns(gs, world, {{.id = 1, .kind = MarkerKind::Spawn, .pos = {10.5f, 4, 10.5f}, .entity = EntityKind::Guard}}, 1, {}, 0.5f);
        Entity& guard = gs.at(0);
        std::mt19937 rng(1);
        std::vector<int> kills;
        for (int n = 0; n < 4; ++n) {
            for (int hit = 0; hit < 14; ++hit) awardHitXp(slayer, guard, true, false);  // about what killing one takes
            int before = slayer.progress.xp + 10000 * slayer.level;
            CHECK(damage(guard, 10000));
            awardHitXp(slayer, guard, true, true);
            kills.push_back(slayer.progress.xp + 10000 * slayer.level - before);
            updateCombat(slayer, gs, guard.type().respawnSeconds, rng);  // it gets back up; some wear fades
            CHECK(guard.body.alive());
        }
        std::println("guard kills: {} {} {} {} XP (no wear: {})", kills[0], kills[1], kills[2], kills[3], guard.type().xp);
        CHECK(kills[0] < guard.type().xp && kills[1] < kills[0] && kills[2] < kills[1] && kills[3] < kills[2] && kills[3] > 0);
    }

    // The PROG section: a round trip by string id, then unknown ids dropped with a note, bad ones rejected.
    fs::path dir = fs::temp_directory_path() / "flycoos_progress_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string path = (dir / "w.flys").string();
    auto make = [&] {
        Session s{.world = meadow(), .seed = 5};
        s.markers = {{.id = 7, .kind = MarkerKind::Spawn, .pos = {40.5f, 4, 40.5f}, .entity = EntityKind::Guard, .count = 2}};
        startSession(s);
        return s;
    };
    Session a = make();
    CHECK(a.entities.size() == 2);
    a.entities[1].xpWear = 7.5f, a.entities[1].armorWear = 2.25f;
    a.player.level = 5;
    a.player.progress.xp = 37;
    a.player.progress.skills = {SkillLevel{6, 12.5f}, SkillLevel{2, 0.25f}, SkillLevel{4, 99}};
    CHECK(learn(a.player.progress, 5, keen) && learn(a.player.progress, 5, keen) && learn(a.player.progress, 5, worn));
    CHECK(savePlayer(path.c_str(), a, "w.flyw", 1).empty());
    Session b = make();
    SaveReport r;
    CHECK(loadPlayer(path.c_str(), b, 1, r).empty() && r.notes.empty());
    const Progress &pa = a.player.progress, &pb = b.player.progress;
    CHECK(b.player.level == 5 && pb.xp == 37 && rank(pb, keen) == 2 && rank(pb, worn) == 1 && pointsSpent(pb) == 3);
    for (size_t k = 0; k < size_t(Skill::Count); ++k) CHECK(pb.skills[k].level == pa.skills[k].level && pb.skills[k].xp == pa.skills[k].xp);
    CHECK(sheet(b.player, Skill::OneHanded).v == sheet(a.player, Skill::OneHanded).v);
    // WEAR: by (marker id, slot), beside NPCS.
    CHECK(b.entities.size() == 2 && b.entities[0].xpWear == 0 && b.entities[0].armorWear == 0);
    CHECK(b.entities[1].slot == 1 && b.entities[1].xpWear == 7.5f && b.entities[1].armorWear == 2.25f);
    // Crafted files: HEAD plus a PROG payload.
    auto le = [](std::vector<uint8_t>& out, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    auto str = [&](std::vector<uint8_t>& out, std::string_view v) {
        le(out, v.size(), 1);
        out.insert(out.end(), v.begin(), v.end());
    };
    auto craft = [&](const std::vector<uint8_t>& prog, const char* tag2 = "PROG") {
        std::vector<uint8_t> out{'F', 'L', 'Y', 'S', 1, 0, 0, 0}, head{0};
        le(head, 1, 4), le(head, 5, 4), le(head, std::bit_cast<uint64_t>(10.0), 8), le(head, std::bit_cast<uint32_t>(0.5f), 4);
        for (auto [tag, payload] : {std::pair{"HEAD", head}, std::pair{tag2, prog}}) {
            out.insert(out.end(), tag, tag + 4);
            le(out, payload.size(), 8);
            out.insert(out.end(), payload.begin(), payload.end());
        }
        le(out, fnv1a(out.data(), out.size()), 4);
        std::string f = (dir / "crafted.flys").string();
        fs::remove(f + ".bak");
        std::ofstream(f, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        return f;
    };
    auto prog = [&](std::vector<std::tuple<std::string, uint64_t, float>> skills, std::vector<std::pair<std::string, uint64_t>> talents,
                    uint64_t xp = 0) {
        std::vector<uint8_t> out;
        le(out, xp, 4), le(out, skills.size(), 1);
        for (auto& [sid, lvl, sxp] : skills) str(out, sid), le(out, lvl, 4), le(out, std::bit_cast<uint32_t>(sxp), 4);
        le(out, talents.size(), 2);
        for (auto& [tid, rk] : talents) str(out, tid), le(out, rk, 1);
        return out;
    };
    Session c = make();
    // Level 1 (no PLYR): a talent is over budget, so the talents reset; unknown ids are dropped; rank above max lowered.
    CHECK(loadPlayer(craft(prog({{"one_handed", 3, 5}, {"archery", 9, 0}}, {{"keen_edge", 9}, {"juggling", 1}}, 7)).c_str(), c, 1, r).empty());
    CHECK(c.player.progress.skills[0].level == 3 && c.player.progress.skills[0].xp == 5 && c.player.progress.xp == 7);
    CHECK(pointsSpent(c.player.progress) == 0 && r.notes.size() == 4);  // archery, juggling, keen_edge 9 -> 5, then the reset
    for (const std::string& n : r.notes) std::println("  progress note: {}", n);
    for (auto bad : {prog({{"one_handed", 0, 0}}, {}), prog({{"one_handed", 101, 0}}, {}), prog({{"one_handed", 2, std::nanf("")}}, {}),
                     prog({{"one_handed", 2, -1}}, {}), prog({{"one_handed", 2, 0}, {"one_handed", 3, 0}}, {}),
                     prog({}, {{"keen_edge", 0}}), prog({}, {{"keen_edge", 1}, {"keen_edge", 1}}), prog({}, {{"", 1}})}) {
        Session e = make();
        e.player.level = 9;
        CHECK(!loadPlayer(craft(bad).c_str(), e, 1, r).empty() && e.player.level == 9);  // rejected, untouched
    }
    // XP past the next level is lowered with a note (level 1: 100 to level 2; One-Handed 1: 201 to 2).
    CHECK(loadPlayer(craft(prog({{"one_handed", 1, 500}}, {}, 150)).c_str(), c, 1, r).empty() && r.notes.size() == 2);
    CHECK(c.player.progress.xp == 99 && c.player.progress.skills[0].xp < skillXpToNext(blade.improveMult, blade.improveOffset, 1) &&
          c.player.progress.skills[0].xp > 200 && c.player.level == 1);
    {  // ranks that break the rules (Deep Cuts without Keen Edge at its max), with points enough: reset
        Session h = make();
        h.player.level = 10;
        h.player.progress.talents.assign(d.talents.size(), 0);
        h.player.progress.talents[deep] = 1, h.player.progress.talents[quick] = 3, h.player.progress.talents[steady] = 3;
        CHECK(savePlayer(path.c_str(), h, "w.flyw", 1).empty());
        CHECK(loadPlayer(path.c_str(), b, 1, r).empty() && pointsSpent(b.player.progress) == 0 && r.notes.size() == 1);
        h.player.progress.talents[deep] = 0;  // the same without it replays
        CHECK(savePlayer(path.c_str(), h, "w.flyw", 1).empty());
        CHECK(loadPlayer(path.c_str(), b, 1, r).empty() && pointsSpent(b.player.progress) == 6 && r.notes.empty());
    }
    auto wearOf = [&](std::vector<std::tuple<uint32_t, int, float, float>> es) {
        std::vector<uint8_t> out;
        le(out, es.size(), 4);
        for (auto [m, sl, x, y] : es) le(out, m, 4), le(out, uint64_t(sl), 1), le(out, std::bit_cast<uint32_t>(x), 4), le(out, std::bit_cast<uint32_t>(y), 4);
        return out;
    };
    CHECK(loadPlayer(craft(wearOf({{7, 0, 3, 1}, {99, 0, 5, 5}}), "WEAR").c_str(), c, 1, r).empty() && c.entities[0].xpWear == 3 &&
          c.entities[0].armorWear == 1 && r.notes.empty());  // marker 99 has no NPC: dropped silently
    for (auto bad : {wearOf({{7, 0, std::nanf(""), 0}}), wearOf({{7, 0, -1, 0}}), wearOf({{0, 0, 1, 0}}), wearOf({{7, 9, 1, 0}}),
                     wearOf({{7, 0, 1, 0}, {7, 0, 2, 0}})})
        CHECK(!loadPlayer(craft(bad, "WEAR").c_str(), c, 1, r).empty());
    std::vector<uint8_t> shortWear = wearOf({{7, 0, 1, 1}});
    shortWear.pop_back();
    CHECK(loadPlayer(craft(shortWear, "WEAR").c_str(), c, 1, r) == "wear section size mismatch");
    std::vector<uint8_t> cut = prog({{"one_handed", 2, 0}}, {});
    cut.pop_back();
    CHECK(loadPlayer(craft(cut).c_str(), c, 1, r) == "truncated progress");
    cut = prog({}, {});
    cut.push_back(0);
    CHECK(loadPlayer(craft(cut).c_str(), c, 1, r) == "progress section size mismatch");

    // A skill level and a level up show on the HUD (Session::toasts) from step().
    {
        Session s = make();
        Marker boar{.id = 9, .kind = MarkerKind::Spawn, .pos = s.player.pos + glm::vec3(1.2f, 0, 0), .entity = EntityKind::Boar};
        s.markers.push_back(boar);
        syncSpawns(s.entities, s.world, s.markers, s.seed, s.player.pos, s.timeOfDay);
        s.player.progress.skills[size_t(Skill::Unarmed)].xp = skillXpToNext(fist.improveMult, fist.improveOffset, 1) - 1;
        s.player.progress.xp = 98;  // the new skill level's 2 XP levels the character
        for (int i = 0; i < 60 && s.toasts.size() < 2; ++i) step(s, {.aim = {1, 0}, .attack = true}, kTick);
        CHECK(s.toasts.size() == 2 && s.toasts[0].second == "Unarmed 2" && s.toasts[1].second == "Level 2! A talent point to spend (K)" &&
              s.player.level == 2);
        s.toasts.clear();  // two levels at once: the count
        s.player.progress.skills[size_t(Skill::Unarmed)].xp = skillXpToNext(fist.improveMult, fist.improveOffset, 2) - 1;
        s.player.progress.xp = levelXpToNext(2) + levelXpToNext(3) - 3;  // the skill's 3 XP crosses both
        for (int i = 0; i < 120 && s.toasts.size() < 2; ++i) step(s, {.aim = {1, 0}, .attack = true}, kTick);
        CHECK(s.toasts.size() == 2 && s.toasts[1].second == "Level 4! 3 talent points to spend (K)");
    }

    // The data files: skills.json and talents.json are optional; their mistakes name file, record and field.
    fs::path data = dir / "data";
    fs::create_directories(data);
    for (const char* f : {"items.json", "creatures.json"}) fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / f, data / f);
    Defs none;
    std::string error;
    CHECK(loadDefs(data, none, error) && none.talents.empty() && none.skills[0].name == "one_handed" && none.skills[0].useMult == 0);
    CHECK(none.warnings.size() == 2 && none.warnings[0].ends_with("skills.json: missing: skills never improve"));
    const std::string skills = R"([{"id": "one_handed", "name": "A", "use_mult": 1, "improve_mult": 1, "improve_offset": 10},
                                   {"id": "unarmed", "name": "B", "use_mult": 1, "improve_mult": 1, "improve_offset": 10},
                                   {"id": "armor", "name": "C", "use_mult": 1, "improve_mult": 1, "improve_offset": 10)";
    const std::string talent = R"([{"id": "t", "name": "T", "tree": "armor", "tier": 0, "max_rank": 2, "per_rank": [{"stat": "armor", "flat": 1}]})";
    auto attempt = [&](const std::string& sk, const std::string& tl, std::string_view want) {
        std::ofstream(data / "skills.json") << sk;
        std::ofstream(data / "talents.json") << tl;
        Defs out;
        bool ok = loadDefs(data, out, error);
        if (want.empty()) {
            CHECK(ok);
            if (!ok) std::println(stderr, "FAIL progress data: {}", error);
        } else {
            CHECK(!ok && error.find(want) != std::string::npos);
            if (ok || error.find(want) == std::string::npos) std::println(stderr, "FAIL want '{}', got '{}'", want, error);
        }
    };
    attempt(skills + "}]", talent + "]", "");
    for (auto [sk, tl, want] : std::initializer_list<std::tuple<std::string, std::string, std::string_view>>{
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 1, "per_rank": [], "max_rank": 1}])", "'u': field 'per_rank': must add something"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "bow", "tier": 1, "max_rank": 1}])", "'u': field 'tree': must be one of"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 1, "max_rank": 0, "per_rank": [{"stat": "armor", "flat": 1}]}])", "field 'max_rank': 0 is out of range"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 5, "max_rank": 1, "per_rank": [{"stat": "armor", "flat": 1}]}])", "field 'tier': 5 is out of range"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 1, "max_rank": 1, "requires": "v", "per_rank": [{"stat": "armor", "flat": 1}]}])", "field 'requires': unknown talent \"v\""},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 0, "max_rank": 1, "requires": "t", "per_rank": [{"stat": "armor", "flat": 1}]}])", "'u': field 'requires': 't' must be in the same tree and a lower tier"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "unarmed", "tier": 1, "max_rank": 1, "requires": "t", "per_rank": [{"stat": "armor", "flat": 1}]}])", "must be in the same tree"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 1, "max_rank": 1}])", "'u': field 'per_rank': missing"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 1, "max_rank": 1, "per_rank": [{"stat": "armor", "flat": 1, "with": "armor"}]}])", "field 'per_rank[0].with': must be a weapon skill"},
             {skills + "}]", talent + R"(,{"id": "u", "name": "U", "tree": "armor", "tier": 1, "max_rank": 1, "per_rank": [{"stat": "armor", "flat": 1}], "cost": 2}])", "'u': field 'cost': unknown field"},
             {skills + R"(},{"id": "archery", "name": "D", "use_mult": 1, "improve_mult": 1, "improve_offset": 10}])", talent + "]", "'archery': field 'id': must be one of one_handed, unarmed, armor"},
             {skills.substr(0, skills.rfind(",\n")) + "]", talent + "]", "skills.json: no record for skill 'armor'"},
             {skills + R"(, "improve_offset": 0}])", talent + "]", "duplicate key"},
             {skills.substr(0, skills.rfind("\"improve_offset\"")) + R"("improve_offset": 0}])", talent + "]", "'armor': field 'improve_offset': 0 is out of range 1.."},
             {skills.substr(0, skills.rfind("\"improve_offset\"")) + R"("improve_offset": 1, "per_level": [{"stat": "armor", "flat": 1, "with": "bow"}]}])", talent + "]", "field 'per_level[0].with': must be a weapon skill"},
         })
        attempt(sk, tl, want);
    std::ofstream(data / "skills.json") << skills + "}]";
    std::ofstream(data / "talents.json") << talent + "]";
    {  // a creature's xp: in range, and only on one that can be killed
        std::ostringstream text;
        text << std::ifstream(data / "creatures.json").rdbuf();
        std::string creatures = text.str();
        auto withXp = [&](std::string_view where, std::string_view xp) {
            std::string c = creatures;
            c.insert(c.find(where) + where.size(), xp);
            std::ofstream(data / "creatures.json") << c;
            Defs out;
            return loadDefs(data, out, error) ? std::string() : error;
        };
        CHECK(withXp(R"("id": "villager", "save_id": 1,)", R"( "xp": 5,)").find("'villager': field 'xp': a creature that can't be hurt") != std::string::npos);
        CHECK(withXp(R"("id": "wolf", "save_id": 4,)", R"( "xp": -1,)").find("'wolf': field 'xp': -1 is out of range") != std::string::npos);
        CHECK(withXp(R"("id": "wolf", "save_id": 4,)", R"( "xp": 0,)").empty());
    }
    fs::remove_all(dir);
}

// Debug notes: the state they keep, and the file they go to.
static void debugNotes() {
    Session s{.world = meadow(), .seed = 1};
    s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}},
                 {.id = 2, .kind = MarkerKind::Region, .name = "Pond", .pos = {10.5f, 4, 10.5f}, .radius = 6},
                 {.id = 3, .kind = MarkerKind::Spawn, .name = "Camp", .pos = {16.5f, 4, 10.5f}, .radius = 3, .entity = EntityKind::Bandit}};
    startSession(s);
    SimInput still;
    still.editing = true;  // NPCs stay put
    for (int i = 0; i < 30; ++i) step(s, still, kTick);
    for (int i = 0; i < 15; ++i) s.eventLog.push_back(std::format("event {}", i));
    GameView view{.path = "worlds/test.flyw", .eye = {1, 2, 3}};
    nlohmann::json c = noteContext(s, view);
    CHECK(c["world"] == "worlds/test.flyw" && c["seed"] == 1 && c["region"] == "#2 Pond");
    CHECK(c["player"]["motion"] == "on_ground" && c["player"]["block_below"] == kBlockNames[Grass] && !c["player"]["in_combat"]);
    CHECK(c["player"]["max_health"] == maxHealth(sheet(s.player)) && c["player"]["bags"].size() == 2);
    CHECK(c["nearby_npcs"].size() == 1 && c["nearby_npcs"][0]["kind"] == "bandit" && c["nearby_npcs"][0]["spawn"] == "#3 Camp");
    CHECK(c["recent_events"].size() == kNoteEvents && c["recent_events"].back() == "event 14");
    CHECK(c["camera"]["eye"] == nlohmann::json({1, 2, 3}));
    s.player.health = 0;
    CHECK(noteContext(s, view)["player"]["motion"] == "knocked_out");

    std::filesystem::path dir = std::filesystem::temp_directory_path() / "notes";
    std::string error;
    std::vector<uint8_t> rgb(2 * 2 * 3, 200);
    CHECK(saveNote(dir, "fell through \"the\" floor", c, rgb, 2, 2, error) == 1);
    CHECK(saveNote(dir, "second", c, {}, 0, 0, error) == 2 && error.empty());
    std::ifstream in(dir / "notes.jsonl");
    std::vector<nlohmann::json> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(nlohmann::json::parse(l));
    CHECK(lines.size() == 2 && lines[0]["note"] == "fell through \"the\" floor" && lines[1]["note"] == "second");
    CHECK(lines[1]["id"] == 2 && lines[1]["status"] == "open");
    CHECK(std::filesystem::exists(lines[0]["screenshot"].get<std::string>()) && lines[1]["screenshot"] == "");
    CHECK(lines[0]["player"] == c["player"]);
}

// Grass and flowers (ADR-030): plant blocks are air to everything but drawing, ride on the ground
// through brushes, go where their grass goes, and the meadow is deterministic.
static void plants() {
    auto field = [] {  // grass at y 3 on stone, a short-grass plant on every column
        Grid g({16, 16, 16});
        for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x) {
                for (int y = 0; y < 4; ++y) g.set({x, y, z}, y == 3 ? Grass : Stone);
                g.set({x, 4, z}, ShortGrass);
            }
        return g;
    };
    for (Block b : {ShortGrass, TallGrass, FlowerWhite, FlowerYellow, FlowerRed, FlowerViolet})
        CHECK(isPlant(b) && isClear(b) && !isOpaque(b) && lightCost(b) == 1);
    for (Block b : {Air, Grass, Water, Blade, PetalRed, Stem}) CHECK(!isPlant(b));
    {  // no collision, not ground, open sky passes
        Grid g = field();
        Player p;
        p.pos = {5.5f, 6, 8.5f};
        for (int i = 0; i < 240; ++i) updatePlayer(p, g, {}, 1 / 120.0f);
        CHECK(p.onGround && std::abs(p.pos.y - 4) < 0.01f && !overlapsSolid(g, p.pos));
        CHECK(groundTop(g, 5, 8) == 3 && groundUnderTrees(g, 5, 8) == 3);
        glm::vec3 spawn = findSpawn(g);
        CHECK(spawn.y == 4);
        Light l;
        fillLight(g, l);
        CHECK(skyLight(l.get({5, 4, 8})) == kMaxLight);  // the grass's top face is as bright as without plants
    }
    {  // meshing: no faces for plants, the grass under them still has its top, each plant listed once
        Grid g = field();
        g.set({2, 4, 2}, FlowerRed);
        g.set({7, 4, 7}, Water);  // a water cell among plants keeps its side faces
        Mesh m = meshRegion(g, {0, 0, 0}, g.size);
        CHECK(m.plants.size() == 255);  // 256 cells, one of them water
        int grassTops = 0;
        for (Quad q : m.opaque) {
            QuadInfo i = unpack(q);
            grassTops += i.block == Grass && i.face == 2 ? i.w * i.h : 0;
            CHECK(!isPlant(i.block));
        }
        CHECK(grassTops == 256);  // under the water cell too
        CHECK(std::ranges::count_if(m.water, [](Quad q) { return unpack(q).face == 2; }) == 1 && m.water.size() == 5);
        auto red = std::ranges::find(m.plants, uint32_t(2 | 4 << 10 | 2 << 18), &PlantInstance::cell);
        CHECK(red != m.plants.end() && (red->info >> 8) == Poppies && (red->info & 255) == kMaxLight << 4);
    }
    {  // edits: plants ride raised and lowered ground, and go where their grass goes
        Grid g = field();
        EditHistory h;
        applyEdit(g, {.tool = Tool::Raise, .center = {8, 8}, .radius = 2, .strength = 3}, h);
        CHECK(g.get({8, 6, 8}) == Grass && g.get({8, 7, 8}) == ShortGrass && g.get({8, 4, 8}) == Dirt);
        applyEdit(g, {.tool = Tool::Lower, .center = {8, 8}, .radius = 2, .strength = 3}, h);
        CHECK(g.get({8, 3, 8}) == Grass && g.get({8, 4, 8}) == ShortGrass && g.get({8, 5, 8}) == Air);
        applyEdit(g, {.tool = Tool::Paint, .center = {3, 3}, .radius = 0, .block = Sand}, h);
        CHECK(g.get({3, 3, 3}) == Sand && g.get({3, 4, 3}) == Air && g.get({4, 4, 3}) == ShortGrass);
        EditResult r = applyEdit(g, {.tool = Tool::Fill, .min = {12, 3, 12}, .max = {12, 3, 12}, .block = Air}, h);
        CHECK(g.get({12, 4, 12}) == Air && r.dirty.hi.y == 4);  // the plant above the hole is part of the step
        applyEdit(g, {.tool = Tool::Fill, .min = {13, 4, 13}, .max = {13, 4, 13}, .block = Planks}, h);
        CHECK(g.get({13, 4, 13}) == Planks);
        std::vector<Instance> none;
        std::vector<Marker> noMarks;
        while (!h.undo.empty()) undoEdit(g, h, none, noMarks);
        CHECK(g.cells == field().cells);
    }
    {  // the meadow: deterministic, Tool::Plant and growMeadow agree, nothing on sand, under trees or water
        Grid g({64, 96, 64});
        generateTerrain(g, 11);
        Grid bare = g;
        for (Block& b : bare.cells) b = isPlant(b) ? Air : b;
        Grid again = bare;
        EditHistory h;
        growMeadow(again, {0, 0}, {63, 63}, ShortGrass, 1, h);
        CHECK(again.cells == g.cells);
        std::vector<Instance> none;
        std::vector<Marker> noMarks;
        undoEdit(again, h, none, noMarks);
        CHECK(again.cells == bare.cells);
        Grid brushed = bare;
        applyEdit(brushed, {.tool = Tool::Plant, .center = {20, 20}, .radius = 12, .strength = 10, .block = ShortGrass}, h);
        for (int z = 10; z <= 30; ++z)
            for (int x = 10; x <= 30; ++x)
                if ((x - 20) * (x - 20) + (z - 20) * (z - 20) <= 144) CHECK(brushed.get({x, groundTop(g, x, z) + 1, z}) == g.get({x, groundTop(g, x, z) + 1, z}));
        std::array<int, BlockCount> kinds{};
        for (int y = 1; y < g.size.y; ++y)
            for (int z = 0; z < g.size.z; ++z)
                for (int x = 0; x < g.size.x; ++x)
                    if (Block b = g.get({x, y, z}); isPlant(b)) {
                        ++kinds[b];
                        CHECK(g.get({x, y - 1, z}) == Grass && groundTop(g, x, z) == y - 1);
                    }
        CHECK(kinds[ShortGrass] > kinds[TallGrass] && kinds[TallGrass] > 0);
        Grid cleared = g;
        growMeadow(cleared, {0, 0}, {63, 63}, Air, 1, h);
        CHECK(cleared.cells == bare.cells);
        Grid canopy = bare;  // a plant left under a crown (placed before its tree) is cleared too
        int cx = 5, cz = 5;
        while (canopy.get({cx, groundTop(canopy, cx, cz), cz}) != Grass) ++cx;  // on land
        int gy = groundTop(canopy, cx, cz);
        canopy.set({cx, gy + 1, cz}, ShortGrass);
        canopy.set({cx, gy + 6, cz}, Leaves);
        plantColumn(canopy, cx, cz, Air, 1);
        CHECK(canopy.get({cx, gy + 1, cz}) == Air);
    }
    {  // several tiles, off the tile grid: the same as planting column by column, and one undo restores it
        Grid g({160, 96, 160});
        generateTerrain(g, 12);
        for (Block& b : g.cells) b = isPlant(b) ? Air : b;
        Grid by = g, start = g;
        for (int z = 7; z <= 130; ++z)
            for (int x = 10; x <= 140; ++x) plantColumn(by, x, z, FlowerYellow, 2);
        EditHistory h;
        EditResult r = growMeadow(g, {140, 130}, {10, 7}, FlowerYellow, 2, h);  // corners in any order
        CHECK(g.cells == by.cells && h.undo.size() == 1 && h.undo[0].parts.size() > 4 && !r.dirty.empty());
        std::vector<Instance> none;
        std::vector<Marker> noMarks;
        undoEdit(g, h, none, noMarks);
        CHECK(g.cells == start.cells);
    }
    {  // a plant drowned by a water fill goes; lowering the lakebed after keeps it under water
        Grid g = field();
        EditHistory h;
        applyEdit(g, {.tool = Tool::Fill, .min = {6, 5, 6}, .max = {10, 7, 10}, .block = Water}, h);
        CHECK(g.get({8, 4, 8}) == Air);
        applyEdit(g, {.tool = Tool::Fill, .min = {1, 0, 1}, .max = {1, 1, 1}, .block = ShortGrass}, h);
        CHECK(g.get({1, 0, 1}) == Air && g.get({1, 1, 1}) == Air);  // plants need grass, at y 0 too
    }
    {  // models: every one has quads, none on the ground, and the far one is cheaper
        for (int m = 0; m < PlantModelCount; ++m) {
            std::vector<Quad> near = plantQuads(PlantModel(m), false), far = plantQuads(PlantModel(m), true);
            CHECK(!near.empty() && far.size() <= near.size());
            for (Quad q : near) CHECK(!(unpack(q).face == 3 && unpack(q).pos.y == 0));
        }
        CHECK(plantModel(TallGrass, {1, 2, 3}) >= TallTuft0 && plantModel(TallGrass, {1, 2, 3}) <= TallTuft2 &&
              plantModel(FlowerViolet, {1, 2, 3}) == Bellflowers);
    }
    {  // a prefab clears the plants it takes the grass from (a deck flush with the ground); removed, they're back
        Grid g = field();
        Prefab deck;
        CHECK(parsePrefab("y: -1\nP = Planks\nW = Wood\n\nlayer\nPPP\nPPP\nPPP\nlayer\nW  \n   \n   \n", deck).empty());
        std::vector<Instance> list;
        EditHistory h;
        EditResult r = placePrefab(g, deck, {.prefab = "deck", .pos = {8, 0, 8}}, std::nullopt, list, h);
        CHECK(!r.error && g.get({8, 3, 8}) == Planks && g.get({8, 4, 8}) == Air && g.get({6, 4, 8}) == ShortGrass);
        CHECK(!removePrefab(g, r.id, list, h).error && g.get({8, 3, 8}) == Grass && g.get({8, 4, 8}) == ShortGrass);
    }
}

// Fighters on the player together (ADR-034): two bandits that start on the same spot push apart and
// come at the player from opposite sides; one alone comes straight in.
static void packs() {
    Grid g({48, 16, 48});
    for (int z = 0; z < 48; ++z)
        for (int x = 0; x < 48; ++x)
            for (int y = 0; y < 4; ++y) g.set({x, y, z}, Stone);
    Player player;
    player.pos = {24.5f, 4, 24.5f};
    std::vector<Entity> es;
    spawnAhead(es, g, player, EntityKind::Bandit, 1, 6);
    spawnAhead(es, g, player, EntityKind::Bandit, 2, 6);  // the very same spot
    std::mt19937 rng(3);
    auto around = [&](const Entity& e) { glm::vec2 d(e.body.pos.x - player.pos.x, e.body.pos.z - player.pos.z); return std::atan2(d.x, d.y); };
    float closest = 99;
    for (int i = 0; i < 4 * 60; ++i) {
        updateEntities(es, g, player, 1 / 60.0f);
        separate(player, g, es);
        updateCombat(player, es, 1 / 60.0f, rng);
        player.health = float(maxHealth(sheet(player)));  // stays up: the fight goes on
        if (i > 30) closest = std::min(closest, glm::distance(es[0].body.pos, es[1].body.pos));
    }
    CHECK(es[0].pack == 2 && es[1].pack == 2 && es[0].angry() && es[1].angry());
    CHECK(std::abs(std::remainder(around(es[0]) - around(es[1]), 2 * std::numbers::pi_v<float>)) > 2.4f);  // opposite sides
    CHECK(closest > 0.9f);  // never inside each other once apart
    for (const Entity& e : es) CHECK(glm::distance(e.body.pos, player.pos) < 2.5f);  // both in to fight

    es.pop_back();  // alone: straight in, no circling
    updateEntities(es, g, player, 1 / 60.0f);
    CHECK(es[0].pack == 1);
}

// Enemies (ADR-042, ADR-043): attack tokens, strings of hits with delayed ones, and the answers to the
// player's attacks: a guard (with a shield; it breaks), a parry, a dodge; backing off after a string.
static void enemies() {
    namespace fs = std::filesystem;
    constexpr float kT = 1 / 60.0f;
    Grid g({48, 16, 48});
    for (int z = 0; z < 48; ++z)
        for (int x = 0; x < 48; ++x)
            for (int y = 0; y < 4; ++y) g.set({x, y, z}, Stone);
    // Test-only kinds that always answer one way: 8 guards, 9 parries (a knife), 10 dodges and backs off.
    fs::path dir = fs::temp_directory_path() / "flycoos_enemies_test";
    fs::create_directories(dir);
    fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
    std::ostringstream text;
    text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string creatures = text.str();
    creatures.erase(creatures.rfind(']'));
    std::ofstream(dir / "creatures.json") << creatures << R"(,
        {"id": "duelist", "save_id": 8, "copy_from": "guard", "guard": 1, "parry": null},
        {"id": "fencer", "save_id": 9, "copy_from": "bandit", "parry": 1, "dodge": null, "back_off": null},
        {"id": "dancer", "save_id": 10, "copy_from": "bandit", "dodge": 1, "parry": null, "back_off": 1}])";
    Defs test, shipped;
    std::string error;
    CHECK(loadDefs(dir, test, error) && loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
    if (!error.empty()) std::println(stderr, "FAIL enemies data: {}", error);
    fs::remove_all(dir);
    setDefs(std::move(test));
    const EntityKind kDuelist = EntityKind(8), kFencer = EntityKind(9), kDancer = EntityKind(10);

    auto fresh = [&] {
        Player p;
        p.pos = {24.5f, 4, 24.5f};
        p.onGround = true, p.offGround = 0;
        giveItem(p.inv, Shortsword);
        quickMove(p.inv, {SlotRef::Item, 0, 0});
        return p;
    };
    Player player = fresh();
    std::mt19937 rng(7);
    std::vector<Entity> es;
    // A tick: the player's buttons, the NPCs, the hits; the player stays up and stands still.
    auto tick = [&](MeleeInput in = {}) {
        Input still;
        updateMelee(player, in, still, kT);
        updateEntities(es, g, player, kT);
        separate(player, g, es);
        updateCombat(player, es, kT, rng);
        player.health = float(maxHealth(sheet(player)));
    };
    auto newest = [](const Entity& e) { return e.transitions ? e.history[(e.transitions - 1) % e.history.size()] : AiTransition{}; };
    auto facing = [&](const Entity& e) {  // the player turned to face `e`
        glm::vec2 d(e.body.pos.x - player.pos.x, e.body.pos.z - player.pos.z);
        player.yaw = std::atan2(d.x, d.y);
        return glm::normalize(d);
    };

    {  // tokens: of three bandits on the player one swings at a time (attackers 1), the others wait on
       // the ring; all get turns. With two tokens two swing at once.
        for (uint32_t i = 0; i < 3; ++i) {
            player.yaw = float(i) * 2.1f;
            spawnAhead(es, g, player, EntityKind::Bandit, 11 + i, 5);
        }
        // Counts the most swinging at once, who swung (and how often), and how long the waiting ones
        // (without a token for a second or more) stood on the ring, give or take a block.
        std::map<uint32_t, int> strings;
        uint32_t lastStarter = 0;
        int again = 0;  // strings started by the one that swung the last, while others waited
        auto run = [&](int seconds, int& most, int& waiting, int& onRing) {
            std::map<uint32_t, int> without;
            for (int i = 0; i < seconds * 60; ++i) {
                tick();
                if (i < 60) continue;  // all on the player by now
                most = std::max(most, int(std::ranges::count(es, AiState::Windup, &Entity::state)));
                for (const Entity& e : es) {
                    CHECK(e.pack == 3 && (e.token || e.state != AiState::Windup));
                    if (e.state == AiState::Windup && e.stateTime == 0 && newest(e).reason == std::string_view("in_reach")) {
                        ++strings[e.id];
                        again += e.id == lastStarter && gAi.attackers == 1;
                        lastStarter = e.id;
                    }
                    int& w = without[e.id];
                    w = e.token ? 0 : w + 1;
                    if (w > 60 && e.state == AiState::Chase && e.backOff <= 0) {
                        ++waiting;
                        onRing += std::abs(glm::distance(e.body.pos, player.pos) - gAi.waitRing) < 1;
                    }
                }
            }
        };
        gAi.attackers = 1;
        int most = 0, waiting = 0, onRing = 0;
        run(20, most, waiting, onRing);
        CHECK(most == 1 && strings.size() == 3);
        for (auto [id, n] : strings) CHECK(n >= 3);  // turns go round...
        CHECK(again == 0);  // ...always to one that waited
        CHECK(waiting > 60 && onRing > waiting * 3 / 4);  // the waiting ones stand out on the ring
        gAi.attackers = 2;
        most = 0;
        strings.clear();
        run(10, most, waiting, onRing);
        CHECK(most == 2 && strings.size() == 3);
        gAi = AiTuning{};
        es.clear();
    }

    {  // strings: a ghoul swings up to its combo of hits in a row, some of them delayed (held about delaySeconds)
        player = fresh();
        Entity& ghoul = spawnAhead(es, g, player, EntityKind::Ghoul, 3, 1.5f);
        const int combo = ghoul.type().combo;
        CHECK(combo >= 2 && ghoul.type().delay > 0);
        int run = 0, longest = 0, delayed = 0, strings = 0;
        uint32_t seen = 0;
        bool heldRight = true;
        for (int i = 0; i < 30 * 60; ++i) {
            tick();
            for (; seen < ghoul.transitions; ++seen) {
                const AiTransition& h = ghoul.history[seen % ghoul.history.size()];
                std::string_view why = h.reason;
                if (h.to != AiState::Windup) continue;
                if (why == "combo" || why == "delayed") ++run;
                else run = 0, ++strings;
                longest = std::max(longest, run + 1);
            }
            if (newest(ghoul).reason == std::string_view("delayed") && ghoul.stateTime == 0) {  // just started: its hold is the delay
                ++delayed;
                heldRight &= ghoul.body.hold >= 0.8f * gAi.delaySeconds - 0.02f && ghoul.body.hold <= 1.2f * gAi.delaySeconds;
            }
        }
        CHECK(strings > 5 && longest == combo && delayed > 0 && heldRight);
        es.clear();
    }

    {  // a guard behind its shield: the player's light is blocked (no damage), costs it stamina; it counters fast
        player = fresh();
        Entity& d = spawnAhead(es, g, player, kDuelist, 4, 1.4f);
        CHECK(d.body.inv.equipped[SlotOffHand] == defs().itemById.at("heater_shield") && d.type().guard == 1);
        setState(d, AiState::Chase, "forced");
        float health = d.body.health;
        bool blocked = false, countered = false;
        float spent = 0;
        for (int i = 0; i < 90; ++i) {
            tick({.attack = i == 0, .attackHeld = false, .aim = facing(d)});
            blocked |= d.body.made(Noise::Block);  // noises add up here (step() clears them in the game)
            spent = std::max(spent, gMelee.staminaMax - d.body.stamina);
            if (newest(d).reason == std::string_view("counter")) countered = true;
        }
        CHECK(blocked && d.body.health == health && spent > 5 && countered);
        // Out of stamina, the next blocked hit breaks its guard: it reels.
        es.clear();
        Entity& e = spawnAhead(es, g, player, kDuelist, 5, 1.4f);
        setState(e, AiState::Chase, "forced");
        e.body.stamina = 1, e.body.staminaWait = 99;
        bool broke = false;
        for (int i = 0; i < 40 && !broke; ++i) {
            tick({.attack = i == 0, .aim = facing(e)});
            broke = e.body.made(Noise::GuardBreak);
        }
        tick();
        CHECK(broke && e.body.stagger > 0 && !e.body.broken && e.state == AiState::Stagger);  // reels, but no riposte
        es.clear();
    }

    {  // a parry with a knife: the player's light is deflected and the player reels; a heavy can't be parried
        int parried = 0;
        for (uint32_t seed = 1; seed <= 5; ++seed) {
            player = fresh();
            Entity& f = spawnAhead(es, g, player, kFencer, seed, 1.4f);
            setState(f, AiState::Chase, "forced");
            float health = f.body.health;
            bool deflected = false, reeled = false;
            for (int i = 0; i < 30; ++i) {
                tick({.attack = i == 0, .aim = facing(f)});
                deflected |= f.body.made(Noise::Parry);
                reeled |= player.stagger > 0.8f * gAi.parriedSeconds;
            }
            parried += deflected && reeled && f.body.health == health;
            es.clear();
        }
        CHECK(parried == 5);
        player = fresh();
        Entity& f = spawnAhead(es, g, player, kFencer, 6, 1.4f);
        setState(f, AiState::Chase, "forced");
        bool deflected = false, blocked = false;
        for (int i = 0; i < 80; ++i) {
            tick({.heavy = i == 0, .aim = facing(f)});
            deflected |= f.body.made(Noise::Parry);
            blocked |= f.body.made(Noise::Block);
        }
        CHECK(!deflected && blocked);  // it guards a heavy instead (with its knife: part of the damage)
        es.clear();
        // Even a parry already in its window doesn't deflect a heavy: it's a weak guard.
        player = fresh();
        Entity& p = spawnAhead(es, g, player, kFencer, 7, 1.4f);
        setState(p, AiState::Chase, "forced");
        startMove(player, Move::Heavy1, facing(p));
        player.moveTime = impactTime(player) - 0.5f * kT;  // lands this tick
        startMove(p.body, Move::Parry, -facing(p));
        p.body.moveScale = parryOf(p.body);
        p.body.hitPending = false;
        p.body.moveTime = 0.2f * p.body.moveScale;  // in the window
        CHECK(parries(p.body, player.pos));
        updateCombat(player, es, kT, rng);
        CHECK(!p.body.made(Noise::Parry) && p.body.made(Noise::Block) && player.stagger == 0);
        es.clear();
    }

    {  // a dodge: it rolls through the player's light (no damage), and backs off after a string of its own
        int dodged = 0, throughIframes = 0;
        for (uint32_t seed = 1; seed <= 5; ++seed) {
            player = fresh();
            Entity& d = spawnAhead(es, g, player, kDancer, seed, 1.4f);
            setState(d, AiState::Chase, "forced");
            float health = d.body.health;
            bool rolled = false, avoided = false;
            for (int i = 0; i < 40; ++i) {
                bool pending = player.hitPending;
                tick({.attack = i == 0, .aim = facing(d)});
                rolled |= d.body.rolling();
                if (pending && !player.hitPending) {  // the hit's moment: rolled out of reach, or through it on i-frames
                    bool through = invulnerable(d.body) && inReach(player, d.body.pos);
                    avoided = through || !inReach(player, d.body.pos);
                    throughIframes += through;
                }
            }
            dodged += rolled && avoided && d.body.health == health;
            es.clear();
        }
        CHECK(dodged == 5 && throughIframes > 0);
        player = fresh();
        Entity& d = spawnAhead(es, g, player, kDancer, 7, 1.4f);
        float before = 0, after = 0;
        bool backedOff = false;
        for (int i = 0; i < 5 * 60 && !backedOff; ++i) {
            tick();
            if (d.backOff > 0 && before == 0) before = glm::distance(d.body.pos, player.pos);
            if (before > 0 && d.backOff == 0) after = glm::distance(d.body.pos, player.pos), backedOff = true;
        }
        CHECK(backedOff && after > before + 1);
        es.clear();
    }
    setDefs(std::move(shipped));
}

// Melee (ADR-035): the light chain, tap or hold, charging, stamina, the guard and guard counters,
// stance breaks and ripostes, flinching and poise, i-frames and hyperarmor, and the attacks that come
// from running, rolling and jumping.
static void melee() {
    Grid g = flatWorld(0);
    constexpr float dt = 1 / 60.0f;
    std::mt19937 rng(5);
    std::vector<Entity> none;
    auto fighter = [] {
        Player p;
        p.pos = {7.5f, 4, 7.5f};
        p.onGround = true, p.offGround = 0;  // standing (no physics in most of these)
        p.inv.equipped[SlotMainHand] = Shortsword;
        return p;
    };
    // One tick of the player: its buttons, then the combat clocks. Returns the move it's in.
    auto tick = [&](Player& p, const MeleeInput& in, std::vector<Entity>& es) {
        Input move;
        updateMelee(p, in, move, dt);
        updateCombat(p, es, dt, rng);
        return p.move;
    };
    MeleeInput tap{.attack = true};

    {  // Tapping through a chain: four different lights, then round again; each costs stamina.
        Player p = fighter();
        std::vector<Move> seen;
        for (int i = 0; i < 4 * 60 && seen.size() < 5; ++i) {
            Move before = p.move;
            if (Move m = tick(p, tap, none); m != before && m != Move::None) seen.push_back(m);
        }
        CHECK((seen == std::vector{Move::Light1, Move::Light2, Move::Light3, Move::Light4, Move::Light1}));
        CHECK(p.stamina < gMelee.staminaMax - 4 * 12);
        // A light after the chain has lapsed (chainGrace) starts it over.
        while (p.punching()) tick(p, {}, none);
        for (int i = 0; i < 30; ++i) tick(p, {}, none);
        CHECK(tick(p, tap, none) == Move::Light1);
    }
    {  // Tap or hold: let go before tapSeconds, a light; held longer, a heavy that charges while held.
        Player p = fighter();
        tick(p, {.attack = true, .attackHeld = true}, none);
        for (int i = 0; i < 5; ++i) tick(p, {.attackHeld = true}, none);
        CHECK(!p.punching());  // not yet a tap or a hold
        CHECK(tick(p, {}, none) == Move::Light1);  // let go: a light
        while (p.punching()) tick(p, {}, none);
        for (int i = 0; i < 30; ++i) tick(p, {}, none);
        tick(p, {.attack = true, .attackHeld = true}, none);
        int ticks = 1;
        while (!p.punching() && ticks++ < 60) tick(p, {.attackHeld = true}, none);
        CHECK(p.move == Move::Heavy1 && std::abs(float(ticks) * dt - gMelee.tapSeconds) < 2 * dt);
        for (int i = 0; i < 3 * 60 && !p.charged; ++i) tick(p, {.attackHeld = true}, none);
        CHECK(p.charged && p.charge >= gMelee.chargeSeconds && p.moveTime >= holdTime(p));
        tick(p, {.attackHeld = true}, none);
        CHECK(p.moveTime > holdTime(p));  // fully charged: it lets fly by itself
        // The heavy button: a heavy at once; let go before it's charged, it swings uncharged.
        Player q = fighter();
        CHECK(tick(q, {.heavy = true, .heavyHeld = true}, none) == Move::Heavy1);
        for (int i = 0; i < 40; ++i) tick(q, {.heavyHeld = true}, none);
        CHECK(q.holding && q.charge > 0);
        tick(q, {}, none);
        for (int i = 0; i < 3; ++i) tick(q, {}, none);
        CHECK(!q.charged && q.moveTime > holdTime(q));
        // Heavies chain too: the second is the overhead chop.
        Move next = Move::None;
        for (int i = 0; i < 2 * 60 && next != Move::Heavy2; ++i) next = tick(q, {.heavy = true}, none);
        CHECK(next == Move::Heavy2);
    }
    {  // Stamina: none left, no attack; it comes back after staminaDelay.
        Player p = fighter();
        p.stamina = 0;
        CHECK(tick(p, tap, none) == Move::None);
        CHECK(tick(p, {}, none) == Move::Light1);  // the press waited: the first bit of stamina back swings it
        while (p.punching()) tick(p, {}, none);
        for (int i = 0; i < int(gMelee.staminaDelay * 60) - 2; ++i) tick(p, {}, none);
        CHECK(p.stamina < 1);  // not while swinging, nor just after
        for (int i = 0; i < 60; ++i) tick(p, {}, none);
        CHECK(p.stamina > 30);
        // A power formula check: twice the power, twice the hit (before armor).
        Player a = fighter(), b;
        a.swingWeapon = NoItem;
        std::mt19937 r1(9), r2(9);
        CHECK(resolveHit(a, b, Attack{8, 8, 1}, {r1, kArmorK, 2}) == 2 * resolveHit(a, b, Attack{8, 8, 1}, {r2}));
    }
    // A bandit 1.5 blocks in front of the player, facing it.
    auto duel = [&](Player& p) {
        std::vector<Entity> es;
        es.reserve(4);
        spawnAhead(es, g, p, EntityKind::Bandit, 1, 1.5f);
        es[0].body.onGround = true;
        return es;
    };
    // Lets the bandit's swing land on `p`, the player holding `in` meanwhile. Returns the damage.
    auto struck = [&](Player& p, std::vector<Entity>& es, const MeleeInput& in) {
        Player& b = es[0].body;
        glm::vec2 at(p.pos.x - b.pos.x, p.pos.z - b.pos.z);
        b.swingTimer = 0;
        CHECK(startPunch(b, glm::normalize(at)));
        float before = p.health;
        for (int i = 0; i < 60 && b.hitPending; ++i) tick(p, in, es);
        return before - p.health;
    };
    {  // The guard: in front it stops most of a hit for stamina; a guard counter follows; from behind, nothing.
        Player p = fighter();
        for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) p.inv.equipped[itemType(id).slot] = id;  // poise: no flinch
        std::vector<Entity> es = duel(p);
        p.yaw = 0;  // the bandit is at +Z
        std::mt19937 keep = rng;
        float open = struck(p, es, {});
        rng = keep;
        p.health = 100;
        tick(p, {.guard = true}, es);
        CHECK(p.guarding);
        float stamina = p.stamina, blocked = struck(p, es, {.guard = true});
        CHECK(blocked < open && p.stamina < stamina - gMelee.blockStamina && p.blockTime < 0.1f && p.stagger == 0);
        // Right after: an attack is a guard counter (once the block's stun has passed).
        Move counter = Move::None;
        for (int i = 0; i < 30 && counter == Move::None; ++i) counter = tick(p, {.attack = true, .attackHeld = true, .guard = true}, es);
        CHECK(counter == Move::GuardCounter && p.blockTime >= gMelee.blockStun);
        while (p.punching()) tick(p, {}, es);
        p.yaw = std::numbers::pi_v<float>;  // its back to the bandit
        p.health = 100;
        CHECK(struck(p, es, {.guard = true}) > 0 && p.blockTime > 0.1f);
        // Too little stamina: the guard breaks, and it reels.
        p.yaw = 0;
        p.stamina = 1;
        tick(p, {.guard = true}, es);
        struck(p, es, {.guard = true});
        CHECK(p.stamina == 0 && p.stagger > 0 && p.broken && !p.guarding);
        CHECK(tick(p, tap, es) == Move::None);  // no acting while it reels
    }
    {  // Hits wear an NPC's stance down: a flinch cancels its swing; at 0 it breaks, open to a riposte.
        Player p = fighter();
        std::vector<Entity> es = duel(p);
        Entity& bandit = es[0];
        p.yaw = 0;
        bandit.body.swingTimer = 0;
        CHECK(startPunch(bandit.body, {0, -1}, 1.0f));  // a long windup
        setState(bandit, AiState::Windup, "test");
        float stance = bandit.body.stance;
        while (tick(p, tap, es) != Move::None && bandit.body.hitPending && p.hitPending) {}
        for (int i = 0; i < 30 && bandit.body.hitPending; ++i) tick(p, {}, es);
        CHECK(!bandit.body.hitPending && bandit.body.stance < stance && bandit.body.stagger > 0 && !bandit.body.broken);
        updateEntities(es, g, p, dt);
        CHECK(bandit.state == AiState::Stagger && bandit.history[(bandit.transitions - 1) % 8].reason == std::string_view("flinched"));
        CHECK(riposteTarget(es, p) == nullptr);
        staggerHit(bandit, bandit.body.stance, false);  // the rest of it
        CHECK(bandit.body.broken && bandit.body.stagger == gMelee.breakSeconds && bandit.body.stance == bandit.type().stance);
        CHECK(riposteTarget(es, p) == &bandit);
        // An attack on it now is a riposte: it can't be hit meanwhile, and the riposte hits three times as
        // hard, on the broken one (not a nearer one beside it).
        while (p.punching()) tick(p, {}, es);
        Entity& nearer = spawnAhead(es, g, p, EntityKind::Bandit, 2, 1.0f);  // (duel() reserved room: `bandit` stays put)
        nearer.body.pos.x += 0.3f;
        float nearHealth = nearer.body.health;
        float health = bandit.body.health;
        CHECK(tick(p, {.attack = true, .aim = {0, 1}, .riposte = true}, es) == Move::Riposte && invulnerable(p) && hyperarmor(p));
        while (p.hitPending) tick(p, {}, es);
        CHECK(health - bandit.body.health >= 3 * 6 && es[1].body.health == nearHealth);  // the sword rolls 6 at least
        CHECK(!bandit.body.broken && bandit.body.stagger > gMelee.downSeconds - 0.1f);  // knocked down instead
        es.pop_back();
        // Its stance comes back once it hasn't been hit for a while.
        staggerHit(bandit, 20, false);
        float worn = bandit.body.stance;
        for (int i = 0; i < 8 * 60; ++i) updateCombat(p, es, dt, rng);
        CHECK(worn < bandit.type().stance && bandit.body.stance == bandit.type().stance);
        // Poise: a guard shrugs off a light (10 < 12) but not the chain's finisher (14).
        Entity guard = es[0];
        guard.kind = EntityKind::Guard;
        guard.body.stagger = 0, guard.body.stance = guard.type().stance;
        staggerHit(guard, spec(Move::Light1).stance, false);
        CHECK(guard.body.stagger == 0);
        staggerHit(guard, spec(Move::Light4).stance, false);
        CHECK(guard.body.stagger > 0 && !guard.body.broken);
    }
    {  // The player flinches from an open hit unless its poise holds or a heavy's hyperarmor does;
       // a roll's i-frames let the hit pass through.
        Player p = fighter();  // no armor: no poise
        std::vector<Entity> es = duel(p);
        p.yaw = 0;
        CHECK(playerPoise(p) == 0);
        tick(p, tap, es);
        struck(p, es, {});
        CHECK(!p.punching() && p.stagger > 0 && !p.broken);
        for (int i = 0; i < 60; ++i) tick(p, {}, es);
        tick(p, {.heavy = true, .heavyHeld = true}, es);
        CHECK(hyperarmor(p) && struck(p, es, {.heavyHeld = true}) > 0 && p.move == Move::Heavy1 && p.stagger == 0);
        while (p.punching()) tick(p, {}, es);
        for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) p.inv.equipped[itemType(id).slot] = id;
        CHECK(playerPoise(p) > es[0].type().hitStance);
        tick(p, tap, es);
        CHECK(struck(p, es, {}) > 0 && p.stagger == 0);  // the armor's poise holds
        while (p.punching()) tick(p, {}, es);
        p.rollTime = 0.1f, p.rollLength = 0.45f;  // mid-roll
        CHECK(invulnerable(p) && struck(p, es, {}) == 0);
    }
    {  // What the body was doing picks the attack: a jump attack holds until it lands, running makes a
       // running attack, late in a roll a rolling attack (the roll ends there); a roll pressed mid-swing
       // waits for the swing's cancel point.
        Player p = fighter();
        p.onGround = false;
        CHECK(tick(p, tap, none) == Move::JumpLight);
        for (int i = 0; i < 60; ++i) tick(p, {}, none);
        CHECK(p.punching() && p.moveTime == holdTime(p) && p.hitPending);  // still falling
        p.onGround = true;
        Input landed{.move = {1, 0}, .run = true, .jump = true};
        updateMelee(p, {}, landed, dt);
        CHECK(landed.move == glm::vec2(0) && !landed.jump && !landed.run);  // landed, it commits
        for (int i = 0; i < 30; ++i) tick(p, {}, none);
        CHECK(!p.hitPending);
        while (p.punching()) tick(p, {}, none);

        Player r = fighter();
        r.vel = {0, 0, r.tuning.runSpeed};
        Input running{.move = {0, 1}, .run = true};
        CHECK(updateMelee(r, tap, running, dt) && r.move == Move::Running && !running.run);

        Player q = fighter();
        q.rollTime = 0.35f, q.rollLength = 0.45f;
        Input rolling;
        CHECK(updateMelee(q, tap, rolling, dt) && q.move == Move::Rolling && !q.rolling());

        Player s = fighter();
        tick(s, tap, none);
        Input roll{.roll = true};
        updateMelee(s, {}, roll, dt);
        CHECK(!roll.roll && s.want == Want::Roll && s.punching());
        bool rolled = false;
        for (int i = 0; i < 60 && !rolled; ++i) {
            Input in;
            updateMelee(s, {}, in, dt);
            rolled = in.roll;
            updateCombat(s, none, dt, rng);
        }
        CHECK(rolled && !s.punching());
    }
    // The bandit's swing lands on `p` this tick (the player holding `in`). Returns the damage.
    auto strikeNow = [&](Player& p, std::vector<Entity>& es, const MeleeInput& in) {
        Player& b = es[0].body;
        b.swingTimer = b.stagger = 0, b.broken = false;
        CHECK(startPunch(b, glm::normalize(glm::vec2(p.pos.x - b.pos.x, p.pos.z - b.pos.z))));
        b.hold = 0, b.moveTime = impactTime(b) - 1e-4f;
        float before = p.health;
        tick(p, in, es);
        CHECK(!b.hitPending);
        return before - p.health;
    };
    const ItemId buckler = defs().itemById.at("oak_buckler"), heater = defs().itemById.at("heater_shield"),
                 tower = defs().itemById.at("tower_shield");
    auto armored = [&] {  // poise: the bandit's hits don't make it flinch
        Player p = fighter();
        for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) p.inv.equipped[itemType(id).slot] = id;
        p.yaw = 0;  // duel(): the bandit is at +Z
        return p;
    };
    {  // Shields (ADR-036): a shield's guard stops more of a hit and of its stamina than the sword's; a
       // greatshield's makes a weak attack bounce off (its attacker recoils), a strong one not.
        Player p = armored();
        std::vector<Entity> es = duel(p);
        Entity& bandit = es[0];
        CHECK(!shieldOf(p) && guardOf(p).negation == gMelee.guardNegation && guardOf(p).boost == gMelee.guardBoost);
        auto blocked = [&](ItemId shield) {  // the same hit on a guard of `shield`: {damage, stamina spent}
            p.inv.equipped[SlotOffHand] = shield;
            p.stamina = gMelee.staminaMax, p.health = 100;
            for (int i = 0; i < 12; ++i) tick(p, {.guard = true}, es);  // up a while: not a perfect guard
            std::mt19937 keep = rng;
            float hurt = strikeNow(p, es, {.guard = true});
            rng = keep;
            CHECK(p.blockTime < 0.1f && p.guarding);
            return std::pair{hurt, gMelee.staminaMax - p.stamina};
        };
        auto [swordHurt, swordStamina] = blocked(NoItem);
        auto [heaterHurt, heaterStamina] = blocked(heater);
        CHECK(swordHurt > 0 && heaterHurt == 0 && heaterStamina < swordStamina && bandit.body.stagger == 0);  // no bounce
        CHECK(guardOf(p).negation == 1 && guardOf(p).boost == itemType(heater).shield.boost);
        auto [towerHurt, towerStamina] = blocked(tower);
        CHECK(towerHurt == 0 && towerStamina < heaterStamina && bandit.body.stagger > 0 && !bandit.body.broken);  // bounced
        updateEntities(es, g, p, dt);
        CHECK(bandit.state == AiState::Stagger);
        CHECK(!bounces(guardOf(p), typeOf(EntityKind::BanditChief).hitStance));  // the chief's hits are too heavy
        auto [bucklerHurt, bucklerStamina] = blocked(buckler);
        CHECK(bucklerHurt > 0 && bucklerHurt <= swordHurt && bucklerStamina > heaterStamina);  // small: not all of it
    }
    {  // The parry: a small or medium shield's, for stamina. A hit in its window deflects, the attacker
       // reels open to a riposte; caught before or after it, the parry is a weak guard; from behind, nothing.
        Player p = armored();
        std::vector<Entity> es = duel(p);
        Entity& bandit = es[0];
        p.inv.equipped[SlotOffHand] = tower;  // a greatshield can't
        CHECK(tick(p, {.parry = true}, es) == Move::None && p.want == Want::None && p.stamina == gMelee.staminaMax);
        p.inv.equipped[SlotOffHand] = buckler;
        CHECK(tick(p, {.parry = true}, es) == Move::Parry && p.stamina == gMelee.staminaMax - spec(Move::Parry).stamina);
        CHECK(p.moveScale == itemType(buckler).shield.parry && !p.hitPending);
        for (int i = 0; i < 60 && !parries(p, bandit.body.pos); ++i) tick(p, {}, es);
        CHECK(strikeNow(p, es, {}) == 0 && bandit.body.broken && bandit.body.stagger > 1 && p.stagger == 0);
        updateEntities(es, g, p, dt);
        CHECK(bandit.state == AiState::Stagger && riposteTarget(es, p) == &bandit);
        while (p.punching()) tick(p, {}, es);
        CHECK(tick(p, {.attack = true, .aim = {0, 1}, .riposte = true}, es) == Move::Riposte);
        while (p.punching()) tick(p, {}, es);

        auto parryUntil = [&](float clock) {  // seconds into the parry at its speed 1
            for (int i = 0; i < 60 && p.punching(); ++i) tick(p, {}, es);
            for (int i = 0; i < 60; ++i) tick(p, {}, es);  // stamina back
            tick(p, {.parry = true}, es);
            for (int i = 0; i < 60 && p.moveTime / p.moveScale < clock; ++i) tick(p, {}, es);
        };
        parryUntil(gMelee.parryTo + 0.05f);  // late: a weak guard
        float stamina = p.stamina;
        CHECK(!parries(p, bandit.body.pos) && strikeNow(p, es, {}) > 0 && p.stamina < stamina);  // blocked, a little
        CHECK(!bandit.body.broken && p.blockTime > 0.1f);  // no guard counter off a parry
        stamina = p.stamina;
        parryUntil(gMelee.parryFrom + 0.02f);
        p.yaw = std::numbers::pi_v<float>;  // its back to the bandit
        stamina = p.stamina;
        CHECK(strikeNow(p, es, {}) > 0 && p.stamina == stamina && !bandit.body.broken);  // not blocked at all
        p.yaw = 0;

        bandit.kind = EntityKind::Boar;  // a boar's tusks can't be parried: a weak guard
        parryUntil(gMelee.parryFrom + 0.02f);
        CHECK(parries(p, bandit.body.pos) && !bandit.type().parryable);
        stamina = p.stamina;
        CHECK(strikeNow(p, es, {}) >= 0 && p.stamina < stamina && !bandit.body.broken);  // the weak guard took it
        bandit.kind = EntityKind::BanditChief;  // the chief takes two parries: the first only rocks it back
        parryUntil(gMelee.parryFrom + 0.02f);
        CHECK(strikeNow(p, es, {}) == 0 && bandit.body.stagger > 0 && !bandit.body.broken && bandit.parried == 1);
        parryUntil(gMelee.parryFrom + 0.02f);
        CHECK(strikeNow(p, es, {}) == 0 && bandit.body.broken && bandit.parried == 0);
        bandit.kind = EntityKind::Bandit;
    }
    {  // A parry waits for the body: not mid-roll, in the air, in water or reeling; mid-swing it cuts the
       // swing at its cancel point. Without a shield that parries, Q does nothing (no cancel either).
        Player p = armored();
        p.inv.equipped[SlotOffHand] = buckler;
        auto parryNow = [&](Player& q) { return tick(q, {.parry = true}, none) == Move::Parry; };
        Player rolling = p, flying = p, wet = p, reeling = p;
        rolling.rollTime = 0.1f, rolling.rollLength = 0.45f;
        flying.onGround = false;
        wet.inWater = true;
        reeling.stagger = reeling.staggerLength = 1;
        CHECK(!parryNow(rolling) && !parryNow(flying) && !parryNow(wet) && !parryNow(reeling));
        tick(p, tap, none);
        CHECK(!parryNow(p) && p.move == Move::Light1 && p.want == Want::Parry);  // buffered
        Move m = p.move;
        for (int i = 0; i < 60 && m != Move::Parry; ++i) m = tick(p, {}, none);
        CHECK(m == Move::Parry);
        Player bare = armored();
        tick(bare, tap, none);
        tick(bare, {.parry = true}, none);
        CHECK(bare.want == Want::None);
        for (int i = 0; i < 30; ++i) tick(bare, {}, none);
        CHECK(bare.move == Move::Light1);  // no free cancel
    }
    {  // The shield is drawn on the left forearm: at the side, raised by the guard, swept out by a parry.
        CHECK(shieldModel(buckler) == ModelBuckler && shieldModel(heater) == ModelHeater && shieldModel(tower) == ModelTower);
        Player p = fighter();
        glm::mat4 held(0), shield(0);
        poseModel(p, ModelPlayer, &held, 1, &shield);
        CHECK(shield == glm::mat4(0));  // none
        p.inv.equipped[SlotOffHand] = heater;
        poseModel(p, ModelPlayer, &held, 1, &shield);
        glm::mat4 side = shield;
        CHECK(side != glm::mat4(0));
        p.guardBlend = 1;
        poseModel(p, ModelPlayer, &held, 1, &shield);
        glm::vec4 middle(4.5f, 6, 0, 1);  // the heater's, where it's held
        CHECK((shield * middle).y > (side * middle).y + 0.25f && (shield * middle).z > (side * middle).z + 0.25f);  // up in front
        p.guardBlend = 0;
        p.inv.equipped[SlotOffHand] = buckler;
        tick(p, {.parry = true}, none);
        while (moveProgress(p) < spec(Move::Parry).holdAt) tick(p, {}, none);  // wound up: across the body
        poseModel(p, ModelPlayer, &held, 1, &shield);
        glm::mat4 swept = shield;
        p.move = Move::None;
        poseModel(p, ModelPlayer, &held, 1, &shield);
        CHECK(swept[2].x < 0 && shield[2].x > 0);  // its face turned from out at the side (+x) across the front
    }
    {  // Through the session, with physics: a lunge steps the player forward into the hit.
        Player p = fighter();
        p.pos.y = 4.001f;
        float z = p.pos.z;
        for (int i = 0; i < 20; ++i) {
            Input move;
            updateMelee(p, i == 0 ? tap : MeleeInput{}, move, dt);
            updatePlayer(p, g, move, dt);
            updateCombat(p, none, dt, rng);
        }
        CHECK(p.pos.z > z + 0.1f);
    }
    {  // What a fight sounds like (ADR-037): each outcome is its own noise, made by the body it happened to.
        Player p = armored();
        std::vector<Entity> es = duel(p);
        Player& b = es[0].body;
        auto quiet = [&] { p.noises = b.noises = 0; };
        quiet();
        CHECK(strikeNow(p, es, {}) > 0 && p.made(Noise::Hurt) && p.hitWith == b.swingWeapon && p.hitWith != NoItem);
        CHECK(b.made(Noise::Windup) && !p.made(Noise::Block));  // it telegraphed the swing
        tick(p, {.guard = true}, es);
        CHECK(p.made(Noise::GuardUp));  // raising it
        quiet();
        tick(p, {.guard = true}, es);
        CHECK(!p.made(Noise::GuardUp));  // only then
        strikeNow(p, es, {.guard = true});
        CHECK(p.made(Noise::Block) && !p.made(Noise::Hurt) && !p.made(Noise::Bounce) && !p.made(Noise::GuardBreak));
        p.inv.equipped[SlotOffHand] = tower;
        quiet();
        strikeNow(p, es, {.guard = true});
        CHECK(p.made(Noise::Block) && p.made(Noise::Bounce));  // off the greatshield
        p.inv.equipped[SlotOffHand] = NoItem;
        p.stamina = 1;
        quiet();
        strikeNow(p, es, {.guard = true});
        CHECK(p.made(Noise::Block) && p.made(Noise::GuardBreak));
        p = armored(), es = duel(p);
        Player& c = es[0].body;
        p.inv.equipped[SlotOffHand] = buckler;
        tick(p, {.parry = true}, es);
        while (p.moveTime / p.moveScale < gMelee.parryFrom + 0.02f) tick(p, {}, es);
        p.noises = c.noises = 0;
        CHECK(strikeNow(p, es, {}) == 0 && p.made(Noise::Parry) && c.made(Noise::StanceBreak) && !p.made(Noise::Hurt));
        while (p.punching()) tick(p, {}, es);
        p.noises = c.noises = 0;
        CHECK(tick(p, {.attack = true, .aim = {0, 1}, .riposte = true}, es) == Move::Riposte);
        CHECK(!p.made(Noise::Swing));  // not out of its windup yet
        float swungAt = -1, windup = holdTime(p), hit = impactTime(p);
        for (int i = 0; i < 120 && p.punching(); ++i)
            if (tick(p, {}, es), p.made(Noise::Swing) && swungAt < 0) swungAt = p.moveTime;
        CHECK(swungAt >= windup && swungAt < hit);  // the blade moves between the windup and the hit
        CHECK(c.made(Noise::Crit) && (c.made(Noise::Hurt) || c.made(Noise::Death)) && c.hitWith == Shortsword && c.hitStance >= 20);
        Player heavy = fighter();
        tick(heavy, {.heavy = true, .heavyHeld = true}, none);
        for (int i = 0; i < 120 && !heavy.charged; ++i) tick(heavy, {.heavyHeld = true}, none);
        CHECK(heavy.charged && heavy.made(Noise::Charged));
    }
    {  // The backstep (ADR-038): standing still, the roll button hops back facing ahead, for less stamina
       // than a roll and with fewer i-frames; attacking out of it is its own lunging sweep.
        Player p = fighter();
        Input in;
        in.roll = true;
        updateMelee(p, {}, in, dt);  // the press waits for updatePlayer to roll
        updatePlayer(p, g, in, dt);
        CHECK(p.rolling() && p.backstep && p.yaw == 0 && p.made(Noise::Backstep) && p.rollDir.y < -0.99f);
        tick(p, {}, none);  // pays for it
        CHECK(p.stamina == gMelee.staminaMax - gMelee.backstepStamina && gMelee.backstepStamina < gMelee.rollStamina);
        p.rollTime = gMelee.backstepIframeFrom + 0.01f;
        CHECK(invulnerable(p));
        p.rollTime = gMelee.backstepIframeTo + 0.01f;
        CHECK(p.rolling() && !invulnerable(p));  // a roll would still be untouchable here
        p.backstep = false;
        CHECK(invulnerable(p));
        p.backstep = true, p.rollTime = 0.7f * p.rollLength;
        CHECK(tick(p, tap, none) == Move::Backstep && !p.rolling());  // takes over from the backstep
        while (p.punching()) tick(p, {}, none);
        p.backstep = false, p.rollLength = 0.45f;
        p.rollTime = 0.7f * p.rollLength;
        CHECK(tick(p, tap, none) == Move::Rolling);  // out of a roll: the rolling attack
    }
    {  // Equip load (ADR-039): the worn gear's weight against what vitality carries picks the roll: light
       // goes farther, heavy shorter and slower, with fewer i-frames, slower to roll again and to get its
       // stamina back; overloaded, no roll, no backstep and no running.
        Player p = fighter(), m = armored();
        const PlayerTuning& t = p.tuning;
        CHECK(maxEquipLoad(p, t) == t.loadBase + t.loadPerVitality * 10 && loadOf(p, t) == Load::Light);  // a sword, 3 of 55
        CHECK(equipWeight(m.inv) == 27 && loadOf(m, t) == Load::Medium);  // iron and a sword: half
        Player h = m;
        h.inv.equipped[SlotOffHand] = tower;
        CHECK(loadOf(h, t) == Load::Heavy);
        h.inv.equipped[SlotOffHand] = heater;
        CHECK(loadOf(h, t) == Load::Medium);
        h.inv.equipped[SlotOffHand] = tower;
        auto rollFrom = [&](Player q, glm::vec2 move) {  // how far a roll carries it, its length and its load
            q.pos = q.prevPos = {7.5f, 4.001f, 2.5f};
            q.vel = {}, q.yaw = 0;
            updatePlayer(q, g, {}, dt);
            Input go;
            go.move = move, go.roll = true;
            updatePlayer(q, g, go, dt);
            float length = q.rolling() ? q.rollLength : 0;
            Load load = q.rollLoad;
            for (int i = 0; i < 90; ++i) updatePlayer(q, g, {}, dt);
            return std::tuple{q.pos.z - 2.5f, length, load};
        };
        auto [light, lightLength, lightLoad] = rollFrom(p, {0, 1});
        auto [medium, mediumLength, mediumLoad] = rollFrom(m, {0, 1});
        auto [heavy, heavyLength, heavyLoad] = rollFrom(h, {0, 1});
        CHECK(lightLoad == Load::Light && mediumLoad == Load::Medium && heavyLoad == Load::Heavy);
        CHECK(light > medium + 0.4f && medium > heavy + 0.4f && heavyLength > mediumLength && lightLength == mediumLength);
        auto [back, backLength, backLoad] = rollFrom(h, {});
        CHECK(back < 0 && back > -2.5f && backLength > t.backstepSeconds);  // a heavy backstep: a little slower
        h.rollTime = gMelee.heavyIframeTo + 0.01f, h.rollLength = 1, h.backstep = false, h.rollLoad = Load::Heavy;
        CHECK(!invulnerable(h));
        h.rollLoad = Load::Medium;
        CHECK(invulnerable(h));
        h.rollTime = 99, h.rollLength = 0;
        // Rolls again only after a longer recovery.
        auto rollsAgainAfter = [&](Player q, float seconds) {
            q.pos = q.prevPos = {7.5f, 4.001f, 2.5f};
            updatePlayer(q, g, {}, dt);
            Input go;
            go.move = {0, 1}, go.roll = true;
            updatePlayer(q, g, go, dt);
            for (float s = 0; s < q.rollLength + seconds; s += dt) updatePlayer(q, g, {}, dt);
            updatePlayer(q, g, go, dt);
            return q.rollTime < 0.1f;
        };
        CHECK(rollsAgainAfter(m, 0.2f) && !rollsAgainAfter(h, 0.2f) && rollsAgainAfter(h, 0.35f));
        // Its stamina comes back slower.
        Player a = fighter(), b = fighter();
        a.stamina = b.stamina = 0, a.load = Load::Heavy;
        for (int i = 0; i < 30; ++i) tick(a, {}, none), tick(b, {}, none);
        CHECK(a.stamina > 0 && a.stamina < b.stamina * 0.75f);
        // Overloaded: no roll, no backstep, no running.
        Player o = h;
        o.tuning.loadBase = 0, o.tuning.loadPerVitality = 3;  // carries 30 of its 40
        CHECK(loadOf(o, o.tuning) == Load::Over);
        auto [over, overLength, overLoad] = rollFrom(o, {0, 1});
        CHECK(overLength == 0 && over < 0.1f);  // a tick of walking, no roll
        CHECK(std::get<1>(rollFrom(o, {})) == 0);
        o.pos = o.prevPos = {7.5f, 4.001f, 2.5f};
        Input run;
        run.move = {0, 1}, run.run = true;
        for (int i = 0; i < 60; ++i) updatePlayer(o, g, run, dt);
        CHECK(o.vel.z > 1 && o.vel.z < t.walkSpeed * t.overloadedSpeed + 0.1f);
        // Holding run or roll does nothing then: no stamina drain, the guard stays up.
        o.combatTimer = kCombatSeconds, o.stamina = 50;
        Input held;
        held.move = {0, 1}, held.run = true, held.roll = true;
        updateMelee(o, {.guard = true}, held, dt);
        CHECK(!held.run && !held.roll && o.want == Want::None && o.guarding && o.stamina >= 50);  // (guarding, it refills a little)
    }
    {  // The perfect guard (ADR-041): a hit as the guard goes up takes no damage and half the stamina, and
       // still opens a guard counter; a guard held up a while, or raised again too soon, is a plain one.
        Player p = armored();
        std::vector<Entity> es = duel(p);
        auto block = [&](int down, int up) {  // guard down `down` ticks, then up `up` ticks before the hit
            for (int i = 0; i < down; ++i) tick(p, {}, es);
            for (int i = 0; i < up; ++i) tick(p, {.guard = true}, es);
            p.stamina = gMelee.staminaMax, p.health = 100, p.noises = 0;
            std::mt19937 keep = rng;
            float hurt = strikeNow(p, es, {.guard = true});
            rng = keep;
            return std::tuple{hurt, gMelee.staminaMax - p.stamina, p.made(Noise::PerfectGuard), p.blockTime};
        };
        auto [held, heldStamina, heldPerfect, heldBlock] = block(40, 20);
        auto [fresh, freshStamina, freshPerfect, freshBlock] = block(40, 0);  // up the very tick it lands
        CHECK(held > 0 && !heldPerfect && fresh == 0 && freshPerfect && freshBlock < 0.1f);
        CHECK(std::abs(freshStamina - heldStamina * gMelee.perfectStamina) < 0.01f);
        auto [tapped, tappedStamina, tappedPerfect, tappedBlock] = block(5, 0);  // down only 5 ticks: raised again too soon
        CHECK(tapped > 0 && !tappedPerfect);
        auto [edge, edgeStamina, edgePerfect, edgeBlock] = block(40, 8);  // up 8 ticks (0.13 s): still in the window
        CHECK(edge == 0 && edgePerfect);
        auto [late, lateStamina, latePerfect, lateBlock] = block(40, 10);  // 0.17 s: past it
        CHECK(late > 0 && !latePerfect);
        auto [rested, restedStamina, restedPerfect, restedBlock] = block(20, 0);  // down 0.33 s: rested enough
        auto [hasty, hastyStamina, hastyPerfect, hastyBlock] = block(17, 0);     // 0.28 s: not
        CHECK(restedPerfect && !hastyPerfect);
    }
    {  // Parrying without a shield (ADR-041): a knife and bare fists parry; a sword can't, and a shield's
       // parry (or a greatshield's none) comes first.
        Player p = fighter();
        CHECK(parryOf(p) == 0 && tick(p, {.parry = true}, none) == Move::None);  // a sword
        p.inv.equipped[SlotMainHand] = Knife;
        CHECK(parryOf(p) == itemType(Knife).parry && itemType(Knife).parry > 0);
        p.inv.equipped[SlotOffHand] = tower;
        CHECK(parryOf(p) == 0);
        p.inv.equipped[SlotOffHand] = buckler;
        CHECK(parryOf(p) == itemType(buckler).shield.parry);
        p.inv.equipped[SlotOffHand] = p.inv.equipped[SlotMainHand] = NoItem;
        CHECK(parryOf(p) == gMelee.fistParry && tick(p, {.parry = true}, none) == Move::Parry);
        p = armored();
        p.inv.equipped[SlotMainHand] = Knife;
        std::vector<Entity> es = duel(p);
        tick(p, {.parry = true}, es);
        CHECK(p.move == Move::Parry && p.moveScale == itemType(Knife).parry);
        for (int i = 0; i < 60 && p.punching() && p.moveTime / p.moveScale < gMelee.parryFrom + 0.02f; ++i) tick(p, {}, es);
        p.noises = 0;
        CHECK(strikeNow(p, es, {}) == 0 && p.made(Noise::Parry) && es[0].body.broken);  // deflected: open to a riposte
    }
    {  // Damage types (ADR-040): a blade's swings slash and its thrusts pierce, fists strike, a beast's
       // own attacks have its kind's; creatures resist or are weak to each, shields stop each apart.
        Player p = fighter();
        p.swingWeapon = Shortsword, p.move = Move::Light1;
        CHECK(damageOf(p) == DamageType::Slash);
        p.move = Move::Light4;
        CHECK(damageOf(p) == DamageType::Pierce);
        p.swingWeapon = NoItem;
        CHECK(damageOf(p) == DamageType::Strike && damageOf(p, DamageType::Pierce) == DamageType::Pierce);
        p.move = Move::None;
        CHECK(typeOf(EntityKind::Wolf).damage == DamageType::Pierce && typeOf(EntityKind::Boar).damage == DamageType::Strike);
        p.inv.equipped[SlotOffHand] = buckler;
        CHECK(guardOf(p, DamageType::Strike).negation < guardOf(p, DamageType::Slash).negation);
        // The same roll on a ghoul (weak to strike, resists pierce) and on a bandit (neither).
        auto hitOn = [&](EntityKind kind, ItemId weapon, Move move) {
            Player q = fighter();
            q.inv.equipped[SlotMainHand] = weapon;
            std::vector<Entity> es;
            es.reserve(2);
            spawnAhead(es, g, q, kind, 1, 1.5f);
            startMove(q, move, {0, 1});
            q.moveTime = impactTime(q) - 1e-4f;
            float before = es[0].body.health;
            std::mt19937 same(9);
            updateCombat(q, es, dt, same);
            return before - es[0].body.health;
        };
        float fistsGhoul = hitOn(EntityKind::Ghoul, NoItem, Move::Light1), fistsBandit = hitOn(EntityKind::Bandit, NoItem, Move::Light1);
        float thrustGhoul = hitOn(EntityKind::Ghoul, Shortsword, Move::Light4), thrustBandit = hitOn(EntityKind::Bandit, Shortsword, Move::Light4);
        CHECK(fistsBandit > 0 && fistsGhoul > fistsBandit && thrustGhoul > 0 && thrustGhoul < thrustBandit);
    }
}

// Sound (ADR-037): what a session hears (footsteps from the gait, a jump and its landing, a roll, a
// splash, loot), what each noise sounds like, the mixer and placing, and takes for every sound played.
static void sounds() {
    Session s{.world = meadow(), .seed = 2};
    s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}}};
    startSession(s);
    auto count = [&](Noise n) { return std::ranges::count(s.sounds, n, &Sound::what); };
    for (int i = 0; i < 30; ++i) step(s, {}, kTick);  // settled on the grass
    s.sounds.clear();
    SimInput walk;
    walk.move.move = {0, 1};
    float z = s.player.pos.z;
    for (int i = 0; i < 120; ++i) step(s, walk, kTick);
    CHECK(std::abs(float(count(Noise::Step)) - 2 * (s.player.pos.z - z) / 1.7f) <= 1.5f);  // twice a stride (kStride)
    CHECK(count(Noise::Step) > 4 && std::ranges::all_of(s.sounds, [](const Sound& h) {
              return h.what == Noise::Step && h.ground == Grass && h.kind == -1 && h.amount > 0.5f;
          }));
    for (int i = 0; i < 30; ++i) step(s, {}, kTick);  // stopped
    s.sounds.clear();
    SimInput jump;
    jump.move.jump = true;
    step(s, jump, kTick);
    for (int i = 0; i < 90; ++i) step(s, {}, kTick);
    auto land = std::ranges::find(s.sounds, Noise::Land, &Sound::what);
    CHECK(count(Noise::Jump) == 1 && count(Noise::Land) == 1 && land->amount > 5 && land->ground == Grass && count(Noise::Step) == 0);
    s.sounds.clear();
    SimInput roll;
    roll.move.roll = true, roll.move.move = {0, 1};
    step(s, roll, kTick);
    while (s.player.rolling()) step(s, {}, kTick);
    CHECK(count(Noise::Roll) == 1 && count(Noise::Step) <= 1);  // a roll has no footsteps (but the foot it gets up on)
    for (int i = 0; i < 60; ++i) step(s, {}, kTick);
    s.sounds.clear();
    roll.move.move = {};
    step(s, roll, kTick);
    for (int i = 0; i < 60; ++i) step(s, {}, kTick);
    CHECK(count(Noise::Backstep) == 1 && count(Noise::Roll) == 0 && count(Noise::Step) == 0);  // standing: a backstep, a hop
    s.player.teleport({40.5f, 4.5f, 10.5f});  // the shallows (x >= 32)
    s.sounds.clear();
    for (int i = 0; i < 60; ++i) step(s, {}, kTick);
    CHECK(count(Noise::Splash) == 1);
    s.sounds.clear();
    for (int i = 0; i < 60; ++i) step(s, walk, kTick);
    CHECK(count(Noise::Step) > 2 && std::ranges::all_of(s.sounds, [](const Sound& h) { return h.ground == Water; }));
    {  // looting a corpse; an NPC's steps are its kind's
        Entity& bandit = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 1, 1.5f);
        CHECK(damage(bandit, 1000));
        bandit.loot = Knife;
        s.sounds.clear();
        SimInput loot;
        loot.interact = true;
        step(s, loot, kTick);
        CHECK(count(Noise::Loot) == 1 && s.sounds[0].kind == -1);
        s.entities.clear();
        s.player.teleport({10.5f, 4, 10.5f});  // on the grass: the boar won't wade
        Entity& boar = spawnAhead(s.entities, s.world, s.player, EntityKind::Boar, 1, 8);
        CHECK(damage(boar, 1));  // it charges
        s.sounds.clear();
        for (int i = 0; i < 90; ++i) step(s, {}, kTick);
        CHECK(std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Step && h.kind == int(EntityKind::Boar); }));
        s.loaded = [](glm::vec3) { return false; };  // frozen out of the loaded world: nothing heard, nothing saved up
        s.sounds.clear();
        boar.body.noise(Noise::Windup);
        step(s, {}, kTick);
        s.loaded = nullptr;
        step(s, {}, kTick);
        CHECK(!std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Windup; }));
    }
    for (int i = 0; i < 400 && s.sounds.size() < kMaxSounds; ++i) s.player.noise(Noise::Jump), step(s, {}, kTick);
    for (int i = 0; i < 10; ++i) s.player.noise(Noise::Jump), step(s, {}, kTick);
    CHECK(s.sounds.size() == kMaxSounds);  // nobody playing them: it stops growing

    // What each noise plays, in layers.
    using Names = std::vector<std::string>;
    auto names = [](const Sound& h) {
        Names n;
        for (const Layer& l : layersOf(h)) n.push_back(l.name);
        return n;
    };
    const ItemId buckler = defs().itemById.at("oak_buckler"), tower = defs().itemById.at("tower_shield");
    const int bandit = int(EntityKind::Bandit), boar = int(EntityKind::Boar), chief = int(EntityKind::BanditChief);
    CHECK(names({.what = Noise::Step, .ground = Sand}) == Names{"step_sand"} && names({.what = Noise::Step, .ground = Cobble}) == Names{"step_stone"});
    CHECK(names({.what = Noise::Step, .ground = Planks}) == Names{"step_wood"} && names({.what = Noise::Step, .ground = Dirt}) == Names{"step_dirt"});
    CHECK(names({.what = Noise::Hurt, .kind = bandit, .with = Shortsword, .amount = 30}) == (Names{"hit_blade", "hit_heavy", "human_hurt"}));
    CHECK(names({.what = Noise::Hurt, .kind = int(EntityKind::TrainingDummy), .with = Shortsword, .amount = 10}) == Names{"hit_wood"});  // no voice
    CHECK(names({.what = Noise::Hurt, .amount = 12}) == (Names{"hit_blunt", "player_hurt"}));  // the player, gored
    CHECK(names({.what = Noise::Crit, .kind = bandit}) == Names{"hit_crit"});
    CHECK(names({.what = Noise::Swing, .kind = int(EntityKind::Wolf), .move = Move::Light1}) == Names{"wolf_attack"});  // teeth, no blade
    CHECK(names({.what = Noise::Swing, .move = Move::Light2, .with = Shortsword}) == Names{"swing_light"});
    CHECK(names({.what = Noise::Swing, .move = Move::Heavy1, .with = Shortsword}) == (Names{"swing_heavy", "player_attack"}));
    CHECK(names({.what = Noise::Swing, .kind = bandit, .move = Move::Light1, .with = Knife}) == Names{"swing_fast"});
    CHECK(names({.what = Noise::Swing, .move = Move::Parry, .with = defs().itemById.at("oak_buckler")}) == Names{"swing_shield"});
    CHECK(names({.what = Noise::Swing, .move = Move::Parry, .with = Knife}) == Names{"swing_fast"});  // a knife's parry (ADR-041)
    CHECK(names({.what = Noise::Windup, .kind = boar}) == Names{"boar_windup"} && names({.what = Noise::Parry}) == Names{"parry"});
    CHECK(names({.what = Noise::Block, .with = buckler}) == Names{"block_shield"} && names({.what = Noise::Block, .with = Shortsword}) == Names{"block_metal"});
    CHECK(names({.what = Noise::Block}) == Names{"block_arms"} && layersOf({.what = Noise::Block, .with = tower})[0].pitch < 1);  // a deeper thud
    std::vector<Layer> death = layersOf({.what = Noise::Death, .kind = boar});
    CHECK(names({.what = Noise::Death, .kind = boar}) == (Names{"hit_blunt", "boar_death", "body_fall"}) && death[2].delay > 0.2f);
    CHECK(layersOf({.what = Noise::Hurt, .kind = chief}).back().pitch == typeOf(EntityKind::BanditChief).voicePitch && typeOf(EntityKind::BanditChief).voicePitch < 1);
    CHECK(layersOf({.what = Noise::Loot})[0].bus == BusInterface);
    CHECK(names({.what = Noise::Backstep, .ground = Grass}) == (Names{"step_grass", "step_grass", "jump"}));  // a hop: off, and down
    CHECK(names({.what = Noise::PerfectGuard}) == Names{"block_perfect"});
    CHECK(names({.what = Noise::Roll, .ground = Grass}) == (Names{"roll", "step_grass"}));
    CHECK(names({.what = Noise::Roll, .ground = Grass, .amount = float(Load::Heavy)}) == (Names{"roll", "step_grass", "land"}));  // thuds down

    // The mixer: a take plays once (panned), waits out its delay, loops, stops for room; mute is silence.
    {
        Audio a;
        a.pitchSpread = a.gainSpread = 0;
        Clip tone;
        tone.rate = Audio::kRate;
        tone.samples.assign(480, 16000);  // 10 ms
        a.groups["tone"].clips = {tone, tone};
        std::vector<float> out(2 * 960);
        auto loud = [&](int from, int to, int channel) {
            float most = 0;
            for (int f = from; f < to; ++f) most = std::max(most, std::abs(out[2 * size_t(f) + size_t(channel)]));
            return most;
        };
        CHECK(a.play("tone", 1, 1) > 0 && a.play("none") == 0 && a.missing == Names{"none"});
        a.mix(out.data(), 960);
        CHECK(loud(0, 470, 1) > 0.3f && loud(0, 960, 0) == 0 && loud(490, 960, 1) == 0 && a.playing() == 0);  // right only, then done
        a.play("tone", 1, 0, 1, BusEffects, false, 0.005f);
        a.mix(out.data(), 960);
        CHECK(loud(0, 230, 0) == 0 && loud(250, 700, 0) > 0.3f);  // 5 ms late
        int bed = a.play("tone", 0.5f, 0, 1, BusAmbience, true);
        a.mix(out.data(), 960), a.mix(out.data(), 960);
        CHECK(loud(900, 960, 0) > 0.1f && a.playing() == 1);  // still going
        a.setGain(bed, 0);
        for (int i = 0; i < 20; ++i) a.mix(out.data(), 960);
        CHECK(loud(0, 960, 0) < 1e-3f && a.playing() == 1);  // a silent loop stays, to come back
        for (int i = 0; i < 2 * Audio::kMaxVoices; ++i) a.play("tone", 4);
        CHECK(a.playing() == Audio::kMaxVoices && a.play("tone", 0.1f) == 0);  // full: a quieter one waits its turn
        a.mix(out.data(), 960);
        CHECK(loud(0, 400, 0) > 0.9f && loud(0, 400, 0) <= 1);  // squashed, never past full scale
        CHECK(a.playing() == 1);  // the tones are done; the (silent) loop was never what made room
        a.play("tone"), a.muted = true;
        a.mix(out.data(), 960);
        CHECK(loud(0, 960, 0) == 0);
        Listener ear{{0, 0, 0}, {1, 0}};
        Placed close = place(a, ear, {2, 0, 0}), left = place(a, ear, {-12, 0, 0}), far = place(a, ear, {100, 0, 0});
        CHECK(close.gain == 1 && close.pan > 0 && left.gain < 0.5f && left.gain > 0 && left.pan < -0.5f && far.gain == 0);
        CHECK(place(a, ear, {0, 1, 0}).pan == 0);  // overhead: centered
    }

    // Every sound the game asks for has takes in data/sounds.
    std::set<std::string> wanted{"bag_open", "bag_close", "lock_on", "equip", "quest", "level_up", "amb_wind", "amb_high", "amb_birds",
                                 "amb_night", "amb_sea", "amb_river", "amb_fire", "bird", "owl"};
    const Block grounds[] = {Grass, Dirt, Stone, Sand, Snow, Planks, Water};
    for (int n = 0; n < int(Noise::Count); ++n)
        for (int kind = -1; kind < int(defs().creatures.size()); ++kind)
            for (Block ground : grounds)
                for (Move move : {Move::Light1, Move::Heavy1, Move::Parry})
                    for (ItemId with : {NoItem, Shortsword, Knife, buckler, tower})
                        for (float amount : {0.0f, 30.0f})
                            for (const Layer& l : layersOf({Noise(n), {}, kind, ground, move, with, amount}))
                                if (!(kind < 0 && Noise(n) == Noise::Windup))  // only NPCs telegraph
                                if (kind < 0 || typeOf(EntityKind(kind)).attackable || l.name.find("_hurt") == std::string::npos) wanted.insert(l.name);
    Audio shipped;
    std::string error;
    CHECK(shipped.load(std::filesystem::path(FLYCOOS_SOURCE_DIR) / "data" / "sounds", error));
    for (const std::string& name : wanted)
        if (!shipped.has(name)) std::println(stderr, "FAIL no takes for the sound '{}' (data/sounds/{}_1.ogg)", name, name), ++failures;
    for (const auto& [name, g] : shipped.groups) {
        CHECK(!g.clips.empty() && g.clips[0].seconds() < 200);
        if (!wanted.contains(name)) std::println(stderr, "FAIL data/sounds/{}_*.ogg is never played", name), ++failures;
    }
}

// Lock-on (lockon.h): the pick nearest the middle of the view, switching by screen direction, and
// letting go on a kill (or passing the lock on), past the break range or long out of sight.
static void lockOn() {
    Grid g({48, 16, 48});
    for (int z = 0; z < 48; ++z)
        for (int x = 0; x < 48; ++x)
            for (int y = 0; y < 4; ++y) g.set({x, y, z}, Stone);
    std::vector<Entity> es;
    auto put = [&](float x, float z) {  // a bandit standing at (x, z)
        Player at;
        at.pos = {x, 4, z - 1};
        return spawnAhead(es, g, at, EntityKind::Bandit, uint32_t(es.size() + 1), 1).id;
    };
    // The camera behind the player looks along +Z: screen right is -X.
    glm::vec3 player(24.5f, 4, 10.5f);
    LockView view;
    view.eye = {24.5f, 6, 6};
    view.viewProj = glm::perspectiveRH_ZO(glm::radians(70.0f), view.aspect, 0.1f, 1000.0f) *
                    glm::lookAtRH(view.eye, glm::vec3(24.5f, 5, 30), glm::vec3(0, 1, 0));
    LockSettings s;
    uint32_t ahead = put(24.5f, 20.5f), right = put(20.5f, 20.5f), left = put(29.5f, 19.5f);
    put(24.5f, 40.5f);  // in the middle of the view but out of range
    CHECK(ahead && right && left && ahead != right && right != left);  // ids are unique
    CHECK(pickTarget(es, g, view, player, s) == ahead);
    LockOn lock{ahead};
    CHECK(switchTarget(lock, es, g, view, player, s, {1, 0}) == right);
    CHECK(switchTarget(lock, es, g, view, player, s, {-1, 0}) == left);
    CHECK(switchTarget(lock, es, g, view, player, s, {0, 1}) == ahead);  // nothing above in range: stays
    CHECK(switchTarget(LockOn{right}, es, g, view, player, s, {-1, 0}) == ahead);  // the nearer one that way

    for (int y = 4; y < 9; ++y)  // a wall hides the one ahead
        for (int x = 22; x < 27; ++x) g.set({x, y, 15}, Stone);
    uint32_t picked = pickTarget(es, g, view, player, s);
    CHECK(picked == right || picked == left);
    updateLock(lock, es, g, view, player, s, 1.5f);
    CHECK(lock.target == ahead && lock.unseen == 1.5f);
    updateLock(lock, es, g, view, player, s, 1.0f);  // over 2 s out of sight: lets go
    CHECK(lock.target == 0);
    for (int y = 4; y < 9; ++y)
        for (int x = 22; x < 27; ++x) g.set({x, y, 15}, Air);

    lock = {ahead};
    auto byId = [&](uint32_t id) -> Entity& { return *std::ranges::find(es, id, &Entity::id); };
    damage(byId(ahead), 10000);  // a kill passes the lock to the next one
    updateLock(lock, es, g, view, player, s, 0.016f);
    CHECK(lock.target == right || lock.target == left);
    s.autoSwitch = false;
    damage(byId(lock.target), 10000);
    updateLock(lock, es, g, view, player, s, 0.016f);
    CHECK(lock.target == 0);
    s.autoSwitch = true;
    lock = {byId(right).body.alive() ? right : left};  // the one still standing
    byId(lock.target).body.pos.z += 40;               // past the break range
    updateLock(lock, es, g, view, player, s, 0.016f);
    CHECK(lock.target == 0);

    {  // locked on, walking keeps facing the target: a sidestep, not a turn
        Grid flat = flatWorld(0);
        Player p;
        p.pos = {7.5f, 4.001f, 7.5f};
        Input in;
        in.move = {1, 0};
        in.face = {0, 1};
        for (int i = 0; i < 60; ++i) updatePlayer(p, flat, in, 1 / 120.0f);
        CHECK(std::abs(p.yaw) < 0.01f && p.pos.x > 9);  // faces +Z, moved along +X
        in.run = true;  // running turns to where it goes
        for (int i = 0; i < 60; ++i) updatePlayer(p, flat, in, 1 / 120.0f);
        CHECK(std::abs(p.yaw - std::numbers::pi_v<float> / 2) < 0.05f);
    }
}

int main() {
    // A temp folder of its own (temp_directory_path reads TMPDIR): runs in other worktrees at the
    // same time use the same file names (flycoos_*_test.flyw, flycoos/stale.png).
    std::string tmp = (std::filesystem::temp_directory_path() / "flycoos-core-test-XXXXXX").string();
    if (!mkdtemp(tmp.data()) || setenv("TMPDIR", tmp.c_str(), 1) != 0) {
        std::println(stderr, "FAIL cannot make a temp folder {}", tmp);
        return 1;
    }
    {  // every test reads the shipped item definitions, as the game does
        Defs shipped;
        std::string error;
        if (!loadDefs(std::filesystem::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error)) {
            std::println(stderr, "FAIL cannot load the shipped data: {}", error);
            return 1;
        }
        setDefs(std::move(shipped));
    }
    dataFiles();
    meshing();
    tones();
    chunkMeshes();
    packing();
    lighting();
    streamedGrids();
    streaming();
    physics();
    entities();
    lockOn();
    packs();
    enemies();
    melee();
    sounds();
    combat();
    fighters();
    items();
    protection();
    statSheets();
    generation();
    regions();
    worldFiles();
    instanceCells();
    trees();
    plants();
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
    npcAi();
    session();
    timedSpawns();
    ticks();
    playerSaves();
    quests();
    conditions();
    debugNotes();
    progression();
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    if (failures) std::println(stderr, "{} check(s) failed", failures);
    else std::println("all checks passed");
    return failures ? 1 : 0;
}
