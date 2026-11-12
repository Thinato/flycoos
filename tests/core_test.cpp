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

#include "arena.h"
#include "audio.h"
#include "commands.h"
#include "defs.h"
#include "edit.h"
#include "entity.h"
#include "fx.h"
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
    CHECK(opaque == 7703 && water == 27);  // a campfire (ADR-047; 7673 before it, 8201 before coasts, rivers and lakes)

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
    CHECK(fnv(a) == 0x0f26d3b4u);  // a campfire (ADR-047); 0x22c3a6e7 before it, 0x209bcf65 before coasts, rivers and lakes
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
    int n = g.size.x, forest = 0, rocky = 0, fires = 0;
    std::vector<int> sum(size_t(n + 1) * (n + 1), 0);  // 2D prefix sum of decorated columns
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x) {
            bool tree = false, rock = false;
            for (int y = kSeaLevel; y < g.size.y; ++y) {
                Block b = g.get({x, y, z});
                tree |= isTreeBlock(b);
                rock |= b == Rock;
                fires += b == Ember && g.get({x, y - 1, z}) == Ember;  // a campfire's top ember (ADR-047)
            }
            forest += tree;
            rocky += rock;
            sum[size_t(z + 1) * (n + 1) + x + 1] = (tree || rock) + sum[size_t(z) * (n + 1) + x + 1] +
                                                   sum[size_t(z + 1) * (n + 1) + x] - sum[size_t(z) * (n + 1) + x];
        }
    CHECK(fnv(g) == 0xc5b81750u);  // campfires (ADR-047); 0x73170551 before them, 0xd779f799 before coasts, rivers and lakes
    std::println("regions: {} campfires in 512x512", fires);
    CHECK(fires >= 4 && fires <= 40);  // rest points every hundred blocks or so
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
    CHECK(es.size() == 5 && es[4].kind == EntityKind::BanditArcher);
    Entity &dummy = es[0], &villager = es[1];

    CHECK(dummy.kind == EntityKind::TrainingDummy && dummy.name == "Training Dummy" && dummy.body.level == 1);
    CHECK(dummy.body.health == 100 && maxHealth(dummy.body.stats) == 100);
    CHECK(dummy.type().attackable && !dummy.type().wanders && !dummy.type().talks);
    CHECK(villager.kind == EntityKind::Villager && villager.body.level == 5 && !villager.name.empty());
    CHECK(villager.body.health == 250 && maxHealth(villager.body.stats) == 250);
    CHECK(villager.type().attackable && villager.type().wanders && villager.type().talks);  // (attackable: ADR-056)
    CHECK(spawnEntities(g, player.pos, 1)[1].name == villager.name);  // names are seeded
    for (size_t i = 0; i < es.size(); ++i)  // nobody spawns inside anybody
        for (size_t j = 0; j < i; ++j) CHECK(glm::distance(es[i].body.pos, es[j].body.pos) >= 1.5f);
    for (const Entity& e : es) CHECK(glm::distance(e.body.pos, player.pos) >= 1.5f);
    for (const Entity& e : es) CHECK(g.get(glm::ivec3(glm::floor(e.body.pos)) - glm::ivec3(0, 1, 0)) == Grass);

    // Damage: health stops at 0, and a corpse takes no more. The dummy is immortal (ADR-056): a hit shows its
    // number and takes nothing.
    Entity boar = es[2];
    CHECK(damage(boar, 30) && boar.body.health == 70);
    CHECK(damage(boar, 500) && boar.body.health == 0);
    CHECK(!damage(boar, 10));  // nothing left to hit
    CHECK(damage(dummy, 30) && dummy.body.health == 100 && dummy.body.lastDamage == 30 && dummy.body.hitTime == 0);
    CHECK(std::ranges::any_of(dummy.body.numbers, [](const DamageNumber& n) { return n.amount == 30 && n.age == 0; }));
    CHECK(damage(dummy, 500) && dummy.body.alive() && dummy.body.health == 100 && dummy.body.lastDamage == 500);
    CHECK(damage(villager, 10) && villager.body.health == 240);
    villager.body.health = 250, villager.events = 0;  // (else it runs from that hit below)

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
    // Attributes: 10 health per stamina; a punch does 1-4 + strength x speed, the sword 6-10 + its scaling's share (ADR-048).
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
    CHECK(attributeBonus(base, Attack{6, 10, 2.5f}) == 5);  // slow attacks get more per hit
    Stats quick = base;
    quick.agility = 50;
    CHECK(swingTime(quick, kPunch) < swingTime(base, kPunch));  // agility is attack speed...
    CHECK(attributeBonus(quick, kPunch) == attributeBonus(base, kPunch));  // ...and fists don't scale with it (ADR-048)
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
    CHECK(dummy.body.lastDamage == 0);  // the fist is still on its way
    run(0.3f);
    int dealt = dummy.body.lastDamage;  // (it takes none: immortal, ADR-056)
    CHECK(dealt >= 3 && dealt <= 6 && dummy.body.health == 100);
    CHECK(player.combatTimer > 0 && dummy.body.combatTimer > 0);
    run(1);
    // With the sword in the main hand the same swing hits for the sword's damage.
    giveItem(player.inv, Shortsword);
    CHECK(quickMove(player.inv, {SlotRef::Item, 0, 0}));
    dummy.body.lastDamage = 0;
    CHECK(startPunch(player, {-1, 0}));
    run(0.3f);
    CHECK(dummy.body.lastDamage >= 8 && dummy.body.lastDamage <= 12);
    player.inv = Inventory{};
    run(1);
    CHECK(startPunch(player, {-1, 0}));

    // Misses: facing away, out of reach. The villager can be hit now (ADR-056).
    player.yaw = std::numbers::pi_v<float> / 2;
    CHECK(punchTarget(es, player) == nullptr);
    standBeside(dummy, kPunchReach + 0.5f);
    CHECK(punchTarget(es, player) == nullptr);
    standBeside(villager, 1.2f);
    CHECK(punchTarget(es, player) == &villager);
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

    // The dummy never topples (ADR-056): any hit, however big, leaves it standing at full health to hit again.
    auto standing = poseModel(dummy.body, ModelDummy);
    CHECK(damage(dummy, 1000) && dummy.body.alive() && dummy.body.health == 100);
    standBeside(dummy, 1.2f);
    CHECK(punchTarget(es, player) == &dummy);
    run(1);
    CHECK(dummy.body.alive() && std::abs(poseModel(dummy.body, ModelDummy)[Head][3].y - standing[Head][3].y) < 0.1f);
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
    CHECK(attributeBonus(boar.body.stats, kPunch) == attributeBonus(player.stats, kPunch));
    CHECK(mainAttack(boar.body.inv).minDamage == kPunch.minDamage && mainAttack(boar.body.inv).maxDamage == kPunch.maxDamage);
    CHECK(bandit.kind == EntityKind::Bandit && bandit.body.level == 2 && bandit.type().temper == Temper::Aggressive);
    CHECK(maxHealth(bandit.body.stats) > maxHealth(player.stats) && bandit.body.stats.strength > player.stats.strength);
    CHECK(bandit.body.inv.equipped[SlotMainHand] == Knife);
    const Attack &knife = itemType(Knife).attack, &sword = itemType(Shortsword).attack;
    CHECK(knife.speed < sword.speed && knife.maxDamage < sword.maxDamage && knife.minDamage < sword.minDamage);
    // Its camp is out of sight of the spawn: even at the edge of its wandering the spawn is out of its aggro
    // range (its archer's too, ADR-055).
    float campDist = glm::distance(flat(bandit.home), flat(player.pos)), archerDist = glm::distance(flat(es[4].home), flat(player.pos));
    CHECK(campDist - bandit.type().wanderRadius > bandit.type().aggroRange);
    CHECK(es[4].kind == EntityKind::BanditArcher && archerDist - es[4].type().wanderRadius > es[4].type().aggroRange);

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
    // A punch, like the player's, or (re-pinned for ADR-057) its charge's x1.6: it had wandered 5 blocks off.
    CHECK(player.health < 100 && player.lastDamage >= 3 && player.lastDamage <= 10);
    CHECK(!bandit.angry());
    setState(boar, AiState::Wander, "test");

    // Aggressive: the bandit charges a player inside its aggro range and hits with the knife.
    player.health = 100;
    player.innate = {{Stat::BleedResist, ModKind::Flat, 1e5f}};  // its knife's bleeds aside (ADR-046): the hits alone
    player.pos = bandit.body.pos + glm::vec3(-8, 0, 0);
    sim(0.1f);
    CHECK(bandit.angry());
    sim(6);
    CHECK(player.health < 100 && player.lastDamage >= 5 && player.lastDamage <= 8);  // 3-6 + (strength 3 x 0.3 + agility 2 x 1.2) x speed 0.75
    CHECK(glm::distance(flat(bandit.body.pos), flat(player.pos)) < kPunchReach);

    // Out of its chase range (from it: no leash from home, ADR-055) it gives up (after the swing under way) and walks home.
    player.pos = bandit.body.pos + glm::vec3(-bandit.type().chaseRange - 1, 0, 0);
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
    // It lies still where it fell, for good (ADR-056: nothing comes back).
    boar.body.vel = {};  // (killed mid-stride it would slide to a stop first)
    glm::vec3 fell = boar.body.pos;
    boar.body.deadTime = 0;
    setState(boar, AiState::Chase, "test");
    sim(200);
    CHECK(!boar.body.alive() && boar.state == AiState::Dead && flat(boar.body.pos) == flat(fell) && boar.loot == NoItem);

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
    for (Model m : {ModelBoar, ModelBandit, ModelDagger}) {
        Grid part = buildPart(m, Head);
        CHECK(std::ranges::count(part.cells, Air) < std::ssize(part.cells));
    }
    CHECK(heldModel(Knife) == ModelDagger && heldModel(Shortsword) == ModelSword);
    glm::mat4 held(0);
    poseModel(bandit.body, ModelBandit, &held);
    CHECK(held != glm::mat4(0));
    held = glm::mat4(0);
    poseModel(boar.body, ModelBoar, &held);
    CHECK(held == glm::mat4(0));  // no hands
    Player b = spawnEntities(g, player.pos, 1)[2].body;  // a live boar (this one stays dead: ADR-056)
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
    player.innate = {{Stat::BleedResist, ModKind::Flat, 1e5f}};  // its knife's bleeds aside (ADR-046): the hits alone
    std::mt19937 rng(5);
    auto hits = [&](float k) {
        int lo = 99, hi = 0, n = 0;
        for (int i = 0; i < 20 * 60; ++i) {
            updateEntities(es, g, player, 1 / 60.0f);
            updateCombat(player, es, 1 / 60.0f, rng, k);
            if (player.hitTime == 0) {  // a random crit (ADR-045) does more: not counted
                if (!player.lastCrit) lo = std::min(lo, player.lastDamage), hi = std::max(hi, player.lastDamage), ++n;
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

// The shipped weapon classes (ADR-044), which every data folder needs, into `dir`.
static void copyWeaponClasses(const std::filesystem::path& dir) {
    std::filesystem::copy_file(std::filesystem::path(FLYCOOS_SOURCE_DIR) / "data" / "weapon_classes.json", dir / "weapon_classes.json",
                               std::filesystem::copy_options::overwrite_existing);
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
// and with them the rest of their record from there (the tactics a shield makes valid), and less their
// rare drops (the great weapons, ADR-049) and their bows (ADR-050, ADR-051: the archer fights with fists; ADR-052's crossbow).
static std::string codeItemCreatures() {
    std::ostringstream all;
    all << std::ifstream(std::filesystem::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string text = all.str();
    for (size_t at; (at = text.find(R"( "rare_loot": ")")) != std::string::npos;)
        text.erase(at, text.find(',', text.find("rare_loot_chance", at)) + 1 - at);
    for (const char* bow : {R"( "loot": "short_bow",)", R"( "loot": "light_crossbow",)"})  // the archer's bow (ADR-051), the guard's crossbow (ADR-052)
        for (size_t at; (at = text.find(bow)) != std::string::npos;) text.erase(at, text.find(',', text.find("loot_chance", at)) + 1 - at);
    if (size_t at = text.find(R"( "weapon": "short_bow",)"); at != std::string::npos)  // the archer's bow and all that goes with it: a bandit with fists
        text.erase(at, text.find(',', text.find("\"sidearm\"", at)) + 1 - at);
    if (size_t at = text.find(R"( "weapon": "barrow_greataxe",)"); at != std::string::npos)  // the boss's greataxe (ADR-059): a boss with fists
        text.erase(at, text.find(',', text.find("loot_chance", at)) + 1 - at);
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
    // The armor, then each iron piece's three status resists (ADR-046), the charm's spirit last.
    CHECK(mods.size() == 21 && std::string_view(mods[0].source) == "Iron Helm" && mods[0].stat == Stat::Armor && mods[0].value == 6);
    CHECK(std::string_view(mods[1].source) == "Iron Helm" && mods[1].stat == Stat::BleedResist && mods[1].value == 40);
    CHECK(std::string_view(mods[20].source) == "Bone Charm" && mods[20].stat == Stat::Spirit && mods[20].kind == Flat);
    StatSheet worn = sheet(p);
    CHECK(worn[Stat::Armor] == 37 && worn[Stat::Spirit] == 4 && healthRegen(worn) == 4 * kRegenPerSpirit && maxHealth(worn) == 100);
    CHECK(worn[Stat::BleedResist] == 350 && worn[Stat::FrostResist] == 350 && worn[Stat::PoisonResist] == 300);
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
    copyWeaponClasses(dir);
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
    CHECK(ok(c) && cat["entities"].size() == size_t(EntityKind::Count) + 1 && cat["entities"][6]["id"] == "bandit_chief" &&
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
    bad = poi, bad.entity = EntityKind(defs().creatures.size());
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

    // Lasting deaths (ADR-056): a spawn marker's dead NPC lies where it fell for good, and once its corpse has
    // gone its slot stays empty. (The player respawns at the player spawn: main.cpp uses playerSpawn.)
    sync();
    Entity& boar = npcs[0];
    uint32_t den = boar.spawn.id;
    int slot = boar.slot;
    std::mt19937 rng(1);
    Player far;
    far.pos = {60, 4, 60};
    damage(boar, 1000);
    boar.body.pos = {20, 4, 12};  // died elsewhere
    for (int i = 0; i < 121 * 60; ++i) updateCombat(far, npcs, 1 / 60.0f, rng);
    CHECK(!boar.body.alive() && boar.body.pos == glm::vec3(20, 4, 12));
    DeadSlots dead;
    size_t alive0 = npcs.size();
    buryDead(npcs, dead, g, far);
    CHECK(dead.size() == 1 && dead.at(den) == 1u << slot && npcs.size() == alive0);  // recorded; 62 blocks off, the corpse lies
    std::erase_if(npcs, [&](const Entity& e) { return e.spawn.id == den && e.slot == slot; });  // say it went
    syncSpawns(npcs, g, list, 1, {16, 4, 16}, 0.5f, dead);
    CHECK(npcs.size() == alive0 - 1 && std::ranges::none_of(npcs, [&](const Entity& e) { return e.spawn.id == den && e.slot == slot; }));
    syncSpawns(npcs, g, list, 1, {16, 4, 16}, 0.5f);  // (without the record it would be back)
    CHECK(npcs.size() == alive0);

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
             u32(1) + one(4, 1, "Den", {10, 4, 10}, 8, 10, 1),            // unknown entity (the shipped run 0..9)
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
    CHECK(defaults.size() == 5 && after.size() == before.size());
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
        {EntityKind::Guard, 4, Temper::Passive, true, ModelGuard, Shortsword, ItemId(20)},  // the light crossbow (ADR-052; the short bow before)
        {EntityKind::BanditChief, 3, Temper::Aggressive, false, ModelChief, Shortsword, IronHelm},
        {EntityKind::Ghoul, 3, Temper::Aggressive, false, ModelGhoul, NoItem, BoneCharm},
    };
    for (const Row& r : kRows) {
        const EntityType& t = typeOf(r.kind);
        CHECK(t.name && t.level == r.level && t.temper == r.temper && t.talks == r.talks && t.model == r.model);
        CHECK(t.weapon == r.weapon && t.loot == r.loot && t.attackable && t.wanders && !t.immortal);
        CHECK((t.loot == NoItem) == (t.lootChance == 0) && t.lootChance <= 1);
        if (r.temper == Temper::Aggressive) CHECK(t.aggroRange > 0 && t.aggroRange < t.chaseRange && t.chaseSpeed > 0);
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
    // (Another weapon in hand, ADR-049: it attacks when its light lands first.)
    // (Against a boss, ADR-059: a flask when below 40 health with no strike due, and a roll out of a sweep.)
    auto fight = [&](EntityKind kind, int count, bool geared, uint32_t seed, ItemId weapon = Shortsword, bool drinks = false) {
        Player player;
        player.pos = {20.5f, 4, 32.5f};
        player.onGround = true, player.offGround = 0;  // standing (no physics here)
        if (geared)
            for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, weapon}) {
                giveItem(player.inv, id);
                quickMove(player.inv, {SlotRef::Item, 0, 0});
            }
        CHECK(sheet(player)[Stat::Armor] == (geared ? 37 : 0) && player.inv.equipped[SlotMainHand] == (geared ? weapon : NoItem));
        float reach = kPunchReach - 0.1f, first = 0.35f;  // swings from a sword's length (a longer reach catches a foe backing off)
        if (weapon != Shortsword) {
            Player swing = player;
            startMove(swing, Move::Light1, {});
            first = impactTime(swing) + 0.07f;
        }
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
            bool safe = std::ranges::none_of(alive, [&](const Entity& e) { return due(e) < first; });  // a light lands first
            melee.attack = safe && glm::length(flat(*foe)) < reach && player.stamina > 15;
            if (drinks && safe && player.health < 40 && player.flasks > 0 && !player.punching()) melee.drink = true, melee.attack = false;
            if (Entity* broken = criticalTarget(es, player)) melee.riposte = true, melee.aim = flat(*broken);
            // A foe's own attack about to land that a guard can't take (a grab) or barely can (a charge, a sweep: ADR-057):
            // a careful player rolls through it, sideways, instead of guarding or attacking.
            auto incoming = [&](const Entity& e) {
                const Player& b = e.body;
                if (e.attack < 0 || !attackMove(b.move) || !b.hitPending) return false;
                const CreatureAttack& a = e.type().attacks[size_t(e.attack)];
                float go = b.hold + std::max(0.0f, holdTime(b) - b.moveTime), dist = glm::length(flat(e));
                switch (a.type) {
                case AttackType::Charge: return go <= 0 && (dist - gAi.chargeContact) / a.speed < 0.2f;
                case AttackType::Grab: return go <= 0.05f && dist < a.reach + 1;
                case AttackType::Area: return drinks && go <= 0.05f && dist < a.radius + 0.5f;  // (against a boss it rolls out of a sweep too)
                default: return false;  // a guard takes a leap or a sweep (as a swing: guarded above when in reach)
                }
            };
            Input still;
            if (auto it = std::ranges::find_if(alive, incoming); it != alive.end()) {
                glm::vec2 from = glm::normalize(flat(*it));
                still.roll = true, still.move = {from.y, -from.x};
                melee.attack = melee.guard = false;
            }
            updateMelee(player, melee, still, 1 / 60.0f);
            if (still.roll || player.rollTime < player.rollLength + 0.5f) updatePlayer(player, g, still, 1 / 60.0f);  // a roll moves it (else no physics, as before)
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
    // About a quarter to a third. Over 48 seeds the chief's mean is 31 (30 without random crits, ADR-045):
    // 12 seeds swing it by about 4 either way.
    CHECK(chiefMean >= 20 && chiefMean <= 35);
    CHECK(wolvesMean >= 25 && wolvesMean <= 45 && ghoulsMean >= 20 && ghoulsMean <= 40);  // a quarter to two fifths
    // The heavier weapons in the same fights (ADR-049): the axe and the mace, and the great weapons, slower
    // but harder hits that make a ghoul flinch (its poise is 12) and break stances. A foe with few openings
    // (the chief, ghouls) goes better than with the sword; quick wolves worse, the slowest weapons winning
    // only about half.
    for (const char* id : {"iron_hand_axe", "iron_mace", "iron_greatsword", "barrow_greataxe"}) {
        ItemId w = defs().itemById.at(id);
        float sum[3]{};
        int won[3]{};
        for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
            float r[3] = {fight(EntityKind::BanditChief, 1, true, seed, w), fight(EntityKind::Wolf, 2, true, seed, w), fight(EntityKind::Ghoul, 2, true, seed, w)};
            for (int k = 0; k < 3; ++k) sum[k] += r[k], won[k] += r[k] > 0;
        }
        std::println("story fights with the {}: health left on average chief {:.0f}, wolves {:.0f}, ghouls {:.0f}; won {} {} {} of {}", itemType(w).name,
                     sum[0] / kSeeds, sum[1] / kSeeds, sum[2] / kSeeds, won[0], won[1], won[2], kSeeds);
        CHECK(won[0] >= 10 && won[1] >= 6 && won[2] >= 10);
    }
    CHECK(fight(EntityKind::Wolf, 3, true, 1) < 0);  // a pack of three is too much alone
    {  // The first boss, Ketil (ADR-059), in Iron with the sword: standing and trading he always wins; drinking the three flasks
       // and rolling out of his sweeps it's won now and then, narrowly. A player who also rolls his leaps and charges does better.
        float sum = 0;
        int won = 0, wonDry = 0;
        for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
            float r = fight(EntityKind(9), 1, true, seed, Shortsword, true);
            sum += r, won += r > 0, wonDry += fight(EntityKind(9), 1, true, seed) > 0;
        }
        std::println("boss fight: Ketil the Unburied, health left on average {:.0f}, won {} of {} (without flasks {})", sum / kSeeds, won, kSeeds, wonDry);
        CHECK(wonDry == 0 && won >= 2 && won <= 8 && sum / kSeeds < 30);
    }
    // Their hits on that player: punch or sword + the attributes by its scaling x speed, less armor / (armor + 40 x level).
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
    far.pos = guard.body.pos + glm::vec3(guard.type().chaseRange + 2, 0, 0);
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
    same("EntityKind", "Count", int(EntityKind::Count), 9);
    for (auto [kind, id, name] : {std::tuple{EntityKind::TrainingDummy, 0, "dummy"}, {EntityKind::Villager, 1, "villager"},
                                  {EntityKind::Boar, 2, "boar"}, {EntityKind::Bandit, 3, "bandit"}, {EntityKind::Wolf, 4, "wolf"},
                                  {EntityKind::Guard, 5, "guard"}, {EntityKind::BanditChief, 6, "bandit_chief"},
                                  {EntityKind::Ghoul, 7, "ghoul"}, {EntityKind::BanditArcher, 8, "bandit_archer"}}) {
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
        float aggroRange, wanderRadius, chaseSpeed; ItemId weapon, loot; float lootChance; bool immortal;
    };
    constexpr WantType kWantTypes[] = {
        // name            lvl  sta str agi int spi   attackable wanders talks model
        //                 temper              aggro wander chase weapon      loot       chance immortal  (aggro: ADR-055's, in sight)
        {"Training Dummy", 1, {10, 0, 0, 0, 10}, true,  false, false, ModelDummy,
                           Temper::Harmless,   0,    0,     0,    NoItem,     NoItem,    0,     true },
        {nullptr,          5, {25, 5, 5, 5, 5},  true,  true,  true,  ModelVillager,
                           Temper::Harmless,   0,    12,    6,    NoItem,     NoItem,    0,     false},  // (runs from a hit at 6: ADR-056)
        {"Boar",           1, {10, 2, 2, 2, 2},  true,  true,  false, ModelBoar,
                           Temper::Passive,    0,    10,    6,    NoItem,     BoarSkin,  0.75f, false},
        {"Bandit",         2, {12, 3, 2, 2, 2},  true,  true,  false, ModelBandit,
                           Temper::Aggressive, 24,   5,     5.5f, Knife,      Knife,     0.4f,  false},
        {"Wolf",           1, {8, 6, 70, 2, 2},   true,  true,  false, ModelWolf,
                           Temper::Aggressive, 28,   10,    7.5f, NoItem,     WolfPelt,  0.6f,  false},
        {"Guard",          4, {16, 5, 3, 2, 4},  true,  true,  true,  ModelGuard,
                           Temper::Passive,    0,    6,     6,    Shortsword, ItemId(20), 0.5f, false},  // loot: the light crossbow (save id 20, ADR-052; the short bow before)
        {"Bandit Chief",   3, {11, 3, 2, 2, 2},  true,  true,  false, ModelChief,
                           Temper::Aggressive, 24,   6,     5.5f, Shortsword, IronHelm,  1,     false},
        {"Barrow Ghoul",   3, {7, 4, 2, 2, 2},   true,  true,  false, ModelGhoul,
                           Temper::Aggressive, 16,   4,     4,    NoItem,     BoneCharm, 0.3f,  false},
        {"Bandit Archer",  2, {9, 2, 4, 2, 2},   true,  true,  false, ModelBandit,  // ADR-051: the short bow (19) in hand and as loot
                           Temper::Aggressive, 30,   5,     5,    ItemId(19), ItemId(19), 0.25f, false},
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
        SAME(aggroRange), SAME(wanderRadius), SAME(chaseSpeed), SAME(weapon), SAME(loot), SAME(lootChance), SAME(immortal);
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
        // The Bone Charm (+2 spirit), the knife (+5% crit chance, ADR-045; bleed, ADR-046), the iron set (status resists, ADR-046).
        same(row, "mods.size()", t.mods.size(), size_t(i == BoneCharm ? 1 : i == Knife ? 2 : i >= IronHelm && i <= IronSabatons ? 3 : 0));
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
    // Re-pinned for ADR-044: the knife swings the dagger's moveset, whose hits come a little earlier in
    // the swing (a frame sooner), so two more of them land before the player's (6, was 4) and the
    // damage rolls shift by those draws; it still dies at frame 712.
    // Re-pinned for ADR-045: every hit first rolls for a random critical, so the damage rolls shift
    // (the 13 at frame 359 is a crit: x1.5); the same hits land on the same frames.
    const std::vector<std::tuple<int, char, int>> want{
        {65, 'b', 11},  {121, 'b', 8},  {170, 'p', 6},  {182, 'b', 8},  {241, 'b', 9},  {297, 'p', 5},  {298, 'b', 12}, {346, 'p', 4},  {359, 'b', 13},
        {417, 'b', 10}, {474, 'b', 11}, {535, 'b', 11}, {587, 'p', 4},  {594, 'b', 10}, {651, 'b', 9},  {711, 'p', 5},  {712, 'b', 8},
    };
    CHECK(hits == want);
    if (hits != want)
        for (auto [frame, who, amount] : hits) std::println(stderr, "  got {{{}, '{}', {}}}", frame, who, amount);
    same("fightReplay", "deathFrame", deathFrame, 712);
    if (std::abs(player.health - 76) >= 0.01f) same("fightReplay", "player.health", player.health, 76.0f);  // 82 before ADR-045's crit rolls
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

// Characterization of the passive temper and giving up, pinned before the AI became a state machine
// (ADR-020) and re-pinned only where that changed it on purpose (noted at the value). A
// bare-handed player stands by a boar (seed 2, fixed 60 fps): it ignores them until punched once, then
// chases and fights back; the player doesn't swing again. Then the player steps beyond its chase range
// (was its leash from home, ADR-055): the boar calms down and walks home.
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
    // One punch, no regen in combat. ADR-045's crit rolls shifted the damage rolls: 97 and 76 before.
    same("boarReplay", "boar health", boar.body.health, 95.0f);
    same("boarReplay", "player health", player.health, 73.0f);  // was 71, then 76: its windup telegraphs now (ADR-035)
    player.pos = boar.home + glm::vec3(0, 0, 15);  // backs off, still in its territory
    run(3 * 60, false);  // it runs after them and keeps biting (and charges: ADR-057)
    CHECK(boar.angry());
    for (int i = 0; i < 5 * 60 && boar.attack >= 0; ++i) run(1, false);  // not mid-charge: one goes on to its end (ADR-057)
    CHECK(boar.attack < 0);
    auto near = [](float got, float want) { return std::abs(got - want) < 0.01f; };
    // Was 13.75: the boar used to keep running through a swing; a windup now stands committed. Then
    // 13.65, before the windup telegraphed (ADR-035). Then 12.47, before it charged (ADR-057): it charges the player
    // backing off, stops on the hit and fights on from there.
    if (!near(glm::distance(boar.body.pos, boar.home), 15.06f))
        same("boarReplay", "chase distance from home", glm::distance(boar.body.pos, boar.home), 15.06f);
    player.pos = boar.body.pos + glm::vec3(0, 0, boar.type().chaseRange + 5);  // out of its chase range: gives up once its swing is done
    for (int i = 0; i < 90 && boar.angry(); ++i) run(1, false);
    CHECK(!boar.angry());
    run(10 * 60, false);  // walks back home, both heal out of combat
    CHECK(!boar.angry());
    if (!near(glm::distance(boar.body.pos, boar.home), 0.067f))
        same("boarReplay", "distance from home", glm::distance(boar.body.pos, boar.home), 0.067f);
    same("boarReplay", "boar health", boar.body.health, 100.0f);
    if (!near(player.health, 70.82f)) same("boarReplay", "player health", player.health, 70.82f);  // 84.84 before ADR-045, 81.84 before its charge (ADR-057)
    const std::vector<std::tuple<int, char, int>> want{  // fewer, later gores since ADR-035's telegraphed windup; rolls shifted by ADR-045
        {132, 'b', 5}, {180, 'p', 5}, {241, 'p', 6}, {299, 'p', 6}, {370, 'p', 6}, {431, 'p', 4},
        {661, 'p', 10},  // its charge as the player backs off (ADR-057): x1.6
    };
    CHECK(hits == want);
    if (hits != want)
        for (auto [f, who, amount] : hits) std::println(stderr, "  got {{{}, '{}', {}}}", f, who, amount);
}

// The AI state machine (ADR-020): giving up past the chase range, the committed windup, passive until hit, giving
// up, Return, fleeing, and the transition log.
static void npcAi() {
    namespace fs = std::filesystem;
    auto flat = [](glm::vec3 v) { return glm::vec2(v.x, v.z); };
    auto newest = [](const Entity& e, uint32_t back = 0) -> const AiTransition& {
        return e.history[(e.transitions - 1 - back) % e.history.size()];
    };
    auto reason = [&](const Entity& e, uint32_t back = 0) { return std::string_view(newest(e, back).reason); };
    // The shipped kinds: the default chase range (ADR-055), nobody flees; a readable windup (ADR-035); only an archer's
    // shots have a cooldown (ADR-051: it strafes between them).
    for (const EntityType& t : defs().creatures)
        CHECK(t.chaseRange == std::max(32.0f, 2 * t.aggroRange) && t.reaggroCooldown == 3 && t.windup >= 0.28f &&
              (t.cooldown == 0 || t.keepMax > 0) && t.fleeHealth == 0);
    CHECK(typeOf(EntityKind::Bandit).windup == kDefaultWindup && typeOf(EntityKind::Bandit).recover == kDefaultRecover);
    // The state machine on its own: the bandit here has no tactics (strings, answers, backing off:
    // ADR-042, ADR-043; the enemies test has those) and no leap (ADR-057; the creatureAttacks test). A test-only kind joins it below.
    fs::path dir = fs::temp_directory_path() / "flycoos_ai_test";
    fs::create_directories(dir);
    copyWeaponClasses(dir);
    fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
    std::ostringstream text;
    text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string creatures = text.str();
    size_t tactics = creatures.find(R"("combo")", creatures.find(R"("id": "bandit",)"));
    CHECK(tactics < creatures.find(R"("id": "wolf")"));
    size_t from = creatures.rfind(',', tactics);
    creatures.erase(from, creatures.rfind('}', creatures.find(R"("id": "wolf")")) - from);  // its tactics and its leap (ADR-057), to the record's end
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

    // Giving up: a bandit chases the player, who then stands past its chase range (from the bandit, not
    // its home: ADR-055), stepping back and forth across it every tick for 10 s. It gives up once and walks
    // home, and the player out there doesn't pull it back (the aggro range is well inside the chase range).
    player.pos = home + glm::vec3(-8, 0, 0);
    Entity* bandit = spawn(EntityKind::Bandit);
    sim(1, none);
    CHECK(bandit->state == AiState::Chase && reason(*bandit) == "saw_target");
    uint32_t before = bandit->transitions;
    float chase = bandit->type().chaseRange;
    glm::vec3 edge = bandit->body.pos + glm::vec3(-chase, 0, 0);
    sim(10 * 60, [&](int i) {
        player.pos = edge + glm::vec3(i % 2 ? 0.05f : -0.05f, 0, 0);
        player.health = 100;
    });
    CHECK(bandit->transitions - before <= 3 && !bandit->angry() && reason(*bandit, 1) == "target_far");  // Return, then Wander at home
    CHECK(glm::distance(flat(bandit->body.pos), flat(home)) < 3 + 1);
    // Height counts: the same spot 30 blocks up (a cliff, another floor) is out of its aggro range.
    player.pos = bandit->body.pos + glm::vec3(-6, 30, 0);
    sim(1, none);
    CHECK(!bandit->angry());
    // Re-aggro: within its aggro range (in sight) it attacks again.
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

    // Return ignores the player beside it, then heals and stays calm for its re-aggro cooldown even with
    // the player beside it. A hit on the way brings it back (ADR-055: no leash to flap at any more).
    sim(60, [&](int) { player.pos = home + glm::vec3(-12, 0, 0); });  // draws it away from home
    damage(*bandit, 20);  // (read as it gives up: entering Return drops it)
    player.pos = bandit->body.pos + glm::vec3(-chase - 2, 0, 0);
    for (ticks = 0; bandit->state != AiState::Return && ticks < 60; ++ticks) sim(1, none);  // finishes a windup it started
    CHECK(bandit->state == AiState::Return && reason(*bandit) == "target_far");
    sim(30, none);
    player.pos = bandit->body.pos + glm::vec3(-2, 0, 0);  // behind it: it walks toward home, +x
    float hurtHealth = bandit->body.health;
    sim(1, none);
    CHECK(bandit->state == AiState::Return && hurtHealth < float(maxHealth(bandit->body.stats)));
    {
        Entity probe = *bandit;  // a hit on the way home: it turns on the player
        std::vector<Entity> one{probe};
        damage(one[0], 1);
        updateEntities(one, g, player, kTick);
        CHECK(one[0].state == AiState::Chase && reason(one[0]) == "damaged");
    }
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

    // A player behind a wall (no pathfinding) is out of sight: it searches where it last saw them (it
    // can't get there), then gives up (ADR-055; the pursuit test has the rest).
    for (int z = 20; z < 45; ++z)
        for (int y = 4; y < 7; ++y) g.set({20, y, z}, Stone);
    bandit->body.teleport(home);
    player.pos = home + glm::vec3(-9, 0, 0);
    setState(*bandit, AiState::Chase, "test");
    bandit->lastSeen = player.pos;
    sim(int((gAi.sightGrace + gAi.searchSeconds) * 60) + 120, none);
    CHECK(bandit->state != AiState::Chase && bandit->state != AiState::Search);
    CHECK(reason(*bandit, bandit->state == AiState::Return ? 0 : 1) == "gave_up_search");
    g = meadow();

    // Up a pillar beside it the player is out of reach (a body height above): it doesn't swing, and gives up,
    // though it walks round the pillar for a way up (ADR-055's detours): a free step only winds the clock back.
    bandit->body.teleport(home);
    for (int y = 4; y < 7; ++y) g.set({24, y, 32}, Stone);
    player.pos = {24.5f, 7, 32.5f};
    player.health = 100;
    setState(*bandit, AiState::Chase, "test");
    for (ticks = 0; bandit->state == AiState::Chase && ticks < 30 * 60; ++ticks) sim(1, [&](int) { player.pos = {24.5f, 7, 32.5f}; });
    CHECK(player.health == 100 && bandit->state != AiState::Chase && reason(*bandit, bandit->state == AiState::Return ? 0 : 1) == "lost_target");
    {  // given up for want of a way to the player, a hit (an arrow from up there) doesn't bring it back: no cheese;
       // but a hit from the player come down beside it does
        Entity probe = *bandit;
        probe.body.teleport(home + glm::vec3(8, 0, 0));
        setState(probe, AiState::Chase, "test");
        setState(probe, AiState::Return, "lost_target");
        std::vector<Entity> one{probe}, two{probe};
        damage(one[0], 1);
        updateEntities(one, g, player, kTick);
        CHECK(one[0].state == AiState::Return);
        Player beside;
        beside.pos = two[0].body.pos + glm::vec3(1.2f, 0, 0);
        damage(two[0], 1);
        updateEntities(two, g, beside, kTick);
        CHECK(two[0].state != AiState::Return && reason(two[0], two[0].state == AiState::Chase ? 0 : 1) == "damaged");
    }
    CHECK(ticks >= int(bandit->type().giveUpSeconds * 60) && ticks < 20 * 60);
    g = meadow();

    // A hit while chasing, then the player leaves its chase range: home already, it turns back once and
    // stays calm (the hit is dropped on entering Return), and only a Return that walked heals.
    player.pos = home + glm::vec3(-2, 0, 0);
    bandit->body.teleport(home);
    bandit->body.swingTimer = 1;  // no swing this time
    setState(*bandit, AiState::Chase, "test");
    damage(*bandit, 10);
    player.pos = home + glm::vec3(-chase - 2, 0, 0);
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
    setDefs(plainDefs(R"(,{"id": "coward", "save_id": 10, "copy_from": "boar", "flee_health": 0.5})"));
    fs::remove_all(dir);
    player.pos = home + glm::vec3(-3, 0, 0);
    Entity* coward = spawn(EntityKind(10));
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

// Pursuit (ADR-055): seen from afar but only in sight, chased as far as the player goes while it sees
// them (no leash from home), given up past its chase range; out of sight it searches where it last saw
// them, finds them again or gives up and goes home. On a flat field 160 blocks across.
static void pursuit() {
    auto flat = [](glm::vec3 v) { return glm::vec2(v.x, v.z); };
    auto field = [] {
        Grid g({160, 16, 160});
        for (int z = 0; z < 160; ++z)
            for (int x = 0; x < 160; ++x) {
                for (int y = 0; y < 3; ++y) g.set({x, y, z}, Dirt);
                g.set({x, 3, z}, Grass);
            }
        return g;
    };
    Grid g = field();
    auto wall = [&](int x, int z0, int z1) {  // 4 high across z0..z1 at x
        for (int z = z0; z <= z1; ++z)
            for (int y = 4; y < 8; ++y) g.set({x, y, z}, Stone);
    };
    Player player;
    std::mt19937 rng(5);
    std::vector<Entity> es;
    const glm::vec3 home{80.5f, 4, 80.5f};
    auto spawn = [&](EntityKind kind) {
        es.clear();
        syncSpawns(es, g, {{.id = 1, .kind = MarkerKind::Spawn, .pos = home, .radius = 3, .entity = kind}}, 1, player.pos, 0.5f);
        CHECK(es.size() == 1);
        return &es[0];
    };
    auto reason = [](const Entity& e, uint32_t back = 0) {
        return std::string_view(e.history[(e.transitions - 1 - back) % e.history.size()].reason);
    };
    auto sim = [&](int ticks, auto&& each) {
        for (int i = 0; i < ticks; ++i) {
            each(i);
            player.health = 100;  // (its hits aside)
            updateEntities(es, g, player, kTick);
            separate(player, g, es);
            updateCombat(player, es, kTick, rng);
        }
    };
    auto none = [](int) {};
    const EntityType& bt = typeOf(EntityKind::Bandit);
    CHECK(bt.aggroRange >= 20 && bt.chaseRange >= 2 * bt.aggroRange);  // was 10, with a leash 20 from home

    // Seen from afar: 20 blocks off in the open (twice the old aggro range), it comes.
    player.pos = home + glm::vec3(-20, 0, 0);
    Entity* b = spawn(EntityKind::Bandit);
    CHECK(canSee(g, b->body, player));
    sim(1, none);
    CHECK(b->state == AiState::Chase && reason(*b) == "saw_target" && b->lastSeen == player.pos && b->unseen == 0);
    // Not through a wall: the same spot behind one, for a minute.
    wall(70, 60, 100);
    b = spawn(EntityKind::Bandit);
    CHECK(!canSee(g, b->body, player));
    sim(60 * 60, [&](int) { player.pos = b->body.pos + glm::vec3(-20, 0, 0); });
    CHECK(!b->angry() && b->transitions <= 1);  // (wanders)
    // Nor beyond its aggro range in the open.
    g = field();
    b = spawn(EntityKind::Bandit);
    sim(60 * 60, [&](int) { player.pos = b->body.pos + glm::vec3(-bt.aggroRange - 0.5f, 0, 0); });
    CHECK(!b->angry());

    // Kiting: the player runs off 6 blocks a second (the bandit runs 5.5) for 10 s, 60 blocks past its home;
    // it chases all the way without turning back (the old leash gave up 20 from home).
    b = spawn(EntityKind::Bandit);
    player.pos = home + glm::vec3(-10, 0, 0);
    sim(1, none);
    CHECK(b->state == AiState::Chase);
    bool gaveUp = false;
    sim(10 * 60, [&](int) {
        player.pos.x -= 6 * kTick;
        gaveUp |= b->state == AiState::Return || b->state == AiState::Search;
    });
    CHECK(!gaveUp && b->angry() && glm::distance(flat(b->body.pos), flat(home)) > 40);
    // Outrun past its chase range it gives up, walks home and heals there.
    damage(*b, 10);
    player.pos = b->body.pos + glm::vec3(0, 0, bt.chaseRange + 1);
    sim(1, none);
    CHECK(b->state == AiState::Return && reason(*b) == "target_far");
    sim(30 * 60, none);
    CHECK(b->state == AiState::Wander && reason(*b) == "home" && b->body.health == float(maxHealth(sheet(b->body))));

    // Out of sight: the player ducks behind a wall. A moment later (round a corner) it's still a chase,
    // then it searches: runs to where it last saw them and looks round there, then gives up and goes home.
    g = field();
    wall(70, 60, 100);
    b = spawn(EntityKind::Bandit);
    player.pos = home + glm::vec3(-6, 0, 0);
    sim(1, none);
    CHECK(b->state == AiState::Chase);
    glm::vec3 seenAt = player.pos;
    player.pos = {66.5f, 4, 80.5f};  // behind the wall
    sim(int(gAi.sightGrace * 60) - 2, none);
    CHECK(b->state == AiState::Chase && b->lastSeen == seenAt && b->unseen > 0);
    sim(3, none);
    CHECK(b->state == AiState::Search && reason(*b) == "lost_sight");
    sim(3 * 60, none);
    CHECK(b->state == AiState::Search && glm::distance(flat(b->body.pos), flat(seenAt)) < 1.5f);
    float yaw = b->body.yaw;
    sim(60, none);
    CHECK(b->state == AiState::Search && std::abs(std::remainder(b->body.yaw - yaw, 2 * std::numbers::pi_v<float>)) > 0.5f);  // looks round
    int ticks = 0;
    while (b->state == AiState::Search && ticks++ < 30 * 60) sim(1, none);
    CHECK(b->state == AiState::Return && reason(*b) == "gave_up_search");
    CHECK(std::abs(b->history[(b->transitions - 1) % b->history.size()].after - gAi.searchSeconds) < 2 * kTick);
    sim(30 * 60, none);
    CHECK(b->state == AiState::Wander && reason(*b) == "home");

    // Searching, it finds the player again when they step back into view, and a hit sends it back too.
    b->body.teleport(seenAt);
    player.pos = seenAt + glm::vec3(-3, 0, 0);  // (out of reach: no swing in the same update)
    setState(*b, AiState::Search, "test");
    b->lastSeen = seenAt;
    b->unseen = 1;
    b->spawn.lines = {"There you are!"};  // a marker's lines: shouted on a new fight, not on finding the player again
    sim(1, none);
    CHECK(b->state == AiState::Chase && reason(*b) == "found_target" && b->speech() == nullptr);
    player.pos = {66.5f, 4, 80.5f};
    for (int i = 0; i < 120 && b->state != AiState::Search; ++i) sim(1, none);
    CHECK(b->state == AiState::Search);
    damage(*b, 1);
    sim(1, none);
    CHECK(b->state == AiState::Chase && reason(*b) == "damaged" && b->lastSeen == player.pos);
    // Searching, a player in sight but past its chase range isn't found: it searches on, then gives up.
    player.pos = {66.5f, 4, 80.5f};
    for (int i = 0; i < 120 && b->state != AiState::Search; ++i) sim(1, none);
    CHECK(b->state == AiState::Search);
    player.pos = b->body.pos + glm::vec3(0, 0, bt.chaseRange + 2);
    CHECK(canSee(g, b->body, player));
    sim(60, none);
    CHECK(b->state == AiState::Search);

    // A grace of 0 (the debug window's slider) doesn't flap between chasing and searching while it sees the player.
    gAi.sightGrace = 0;
    b->body.teleport(home);
    setState(*b, AiState::Wander, "test");
    player.pos = home + glm::vec3(-6, 0, 0);
    sim(30, none);
    CHECK(b->angry());
    gAi.sightGrace = AiTuning{}.sightGrace;
    // A search forced from the debug window starts where it stands, not at a stale spot.
    setState(*b, AiState::Wander, "test");
    b->lastSeen = {};
    setState(*b, AiState::Search, "forced");
    CHECK(b->lastSeen == b->body.pos);

    // No flapping at a wall's end: seen every other tick for 3 s (in front of the wall, then behind it), it
    // never turns to searching.
    b->body.teleport(home);
    setState(*b, AiState::Wander, "test");
    player.pos = home + glm::vec3(-6, 0, 0);
    sim(1, none);
    bool searched = false;
    sim(3 * 60, [&](int i) {
        player.pos = i % 2 ? glm::vec3(66.5f, 4, 80.5f) : home + glm::vec3(-4, 0, 0);
        searched |= b->state == AiState::Search;
    });
    CHECK(!searched && b->angry());

    // A crate stack in the way (2 by 2, 2 high: no step, no sight over it): it goes round, not stuck on it.
    g = field();
    for (int x = 70; x < 72; ++x)
        for (int z = 80; z < 82; ++z)
            for (int y = 4; y < 6; ++y) g.set({x, y, z}, Planks);
    player.pos = {62.5f, 4, 80.5f};
    b = spawn(EntityKind::Bandit);
    CHECK(!canSee(g, b->body, player));
    damage(*b, 1);  // (it can't see the player behind the crates, but knows where the hit came from...)
    staggerHit(*b, 20, false);  // (...even one it reels from: every hit, for a bandit)
    sim(1, none);
    CHECK(b->state == AiState::Stagger && b->lastSeen == player.pos && b->unseen == 0);
    int reached = -1;
    sim(8 * 60, [&](int i) {
        if (reached < 0 && glm::distance(flat(b->body.pos), flat(player.pos)) < kPunchReach) reached = i;
    });
    CHECK(reached > 0 && reached < 5 * 60 && b->state != AiState::Return);

    // A doorway a body just fits (1 wide) in a long wall, come at slantwise: the width check doesn't keep it out.
    g = field();
    wall(70, 40, 120);
    g.set({70, 4, 80}, Air), g.set({70, 5, 80}, Air);  // the doorway, 2 high
    player.pos = {66.5f, 4, 77.5f};
    b = spawn(EntityKind::Bandit);
    b->body.teleport({73.5f, 4, 83.5f});
    damage(*b, 1);  // (the doorway's edge hides the player at that slant: it searches where the hit came from)
    reached = -1;
    sim(8 * 60, [&](int i) {
        if (reached < 0 && glm::distance(flat(b->body.pos), flat(player.pos)) < kPunchReach) reached = i;
    });
    CHECK(reached > 0 && reached < 6 * 60);

    // A night-time marker's NPC searching for the player when the night ends stays till the search is
    // over (as one still fighting does); then it leaves.
    {
        Marker night{.id = 1, .kind = MarkerKind::Spawn, .pos = home, .radius = 3, .entity = EntityKind::Bandit, .count = 1,
                     .activeFrom = 0.8f, .activeTo = 0.2f};
        std::vector<Entity> ns;
        syncSpawns(ns, g, {night}, 1, player.pos, 0.9f);
        CHECK(ns.size() == 1);
        setState(ns[0], AiState::Search, "test");
        syncSpawns(ns, g, {night}, 1, player.pos, 0.5f);
        CHECK(ns.size() == 1);
        setState(ns[0], AiState::Return, "test");
        syncSpawns(ns, g, {night}, 1, player.pos, 0.5f);
        CHECK(ns.empty());
    }

    // A passive boar hit once chases too, 40 blocks from its home and on.
    g = field();
    player.pos = home + glm::vec3(-3, 0, 0);
    Entity* boar = spawn(EntityKind::Boar);
    damage(*boar, 1);
    sim(1, none);
    CHECK(boar->state == AiState::Chase);
    gaveUp = false;
    sim(8 * 60, [&](int) {
        player.pos.x -= 6 * kTick;
        gaveUp |= !boar->angry();
    });
    CHECK(!gaveUp && glm::distance(flat(boar->body.pos), flat(home)) > 30);
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
    p.flasks = 1;
    a.restPoint = glm::vec3(22.5f, 4, 14.5f);  // ADR-047
    p.twoHanded = true;  // ADR-048
    p.arrows = 7;        // ADR-050
    const ItemId throwingKnife = defs().itemById.at("throwing_knife"), bomb = defs().itemById.at("clay_bomb");
    p.pouch[throwingKnife] = 4, p.readied = bomb;  // ADR-053
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
    CHECK(b.player.flasks == 1 && b.restPoint == glm::vec3(22.5f, 4, 14.5f));
    CHECK(b.player.arrows == 7);  // the quiver (ADR-050)
    CHECK(b.player.pouch[throwingKnife] == 4 && b.player.pouch[bomb] == 3 && b.player.readied == bomb);  // the pouch (ADR-053)
    CHECK(b.player.twoHanded && twoHanding(b.player) && b.player.gripBlend == 1);  // already in both hands, not taking hold
    CHECK(b.player.inv.equipped == p.inv.equipped && b.player.inv.bags == p.inv.bags && b.player.inv.containers == p.inv.containers);
    Entity *c0 = npc(b, 3, 0), *c1 = npc(b, 3, 1);
    CHECK(c0 && c0->body.pos == glm::vec3(10.5f, 4, 50.5f) && c0->body.yaw == -2 && c0->body.health == 5 && c0->body.alive());
    CHECK(c1 && !c1->body.alive() && c1->state == AiState::Dead && c1->body.deadTime == 7 && c1->loot == Knife && c1->body.pos == corpse);
    // It stays dead (ADR-056), its corpse holding the knife.
    for (int i = 0; i < int(60 / kTick); ++i) step(b, {}, kTick);
    CHECK(npc(b, 3, 1) && !npc(b, 3, 1)->body.alive() && npc(b, 3, 1)->loot == Knife && b.dead.at(3) == 2);

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
        craft({{"HEAD", head}, {"REST", {2, 1, 0, 0}}}),  // a rest point cut short
        craft({{"HEAD", head}, {"REST", {2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}}}),  // "rested" must be 0 or 1
        craft({{"HEAD", head}, {"GRIP", {2}}}),  // two-handed must be 0 or 1 (ADR-048)
        craft({{"HEAD", head}, {"GRIP", {}}}),   // cut short
        craft({{"HEAD", head}, {"GRIP", {1, 0}}}),  // a byte too many
        craft({{"HEAD", head}, {"ARRW", {uint8_t(kQuiver + 1)}}}),  // more than a quiver holds (ADR-050)
        craft({{"HEAD", head}, {"ARRW", {}}}),      // cut short
        craft({{"HEAD", head}, {"ARRW", {3, 0}}}),  // a byte too many
        craft({{"HEAD", head}, {"THRW", {21, 2, 21}}}),  // cut short (ADR-053)
        craft({{"HEAD", head}, {"THRW", {21, 0, 7}}}),   // a byte too many
        [&] {  // a rest point far past any world
            std::vector<uint8_t> rest{2, 1};
            for (float f : {1e30f, 40.0f, 0.0f}) le(rest, std::bit_cast<uint32_t>(f), 4);
            return craft({{"HEAD", head}, {"REST", rest}});
        }(),
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
        CHECK(t.player.flasks == kBaseFlasks && !t.restPoint && t.player.arrows == kQuiver);  // an older save: full flasks and quiver, never rested
        CHECK(t.player.pouch[throwingKnife] == 10 && t.player.pouch[bomb] == 3 && t.player.readied == throwingKnife);  // and a full pouch (ADR-053)
        // A pouch with an item that isn't a throwable (the helm, 5), more knives than a pouch holds, a readied helm.
        write(bad, craft({{"HEAD", head}, {"THRW", {5, 3, 5, 2, 21, 50, 22, 0}}}));
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && t.player.pouch[5] == 0 && t.player.pouch[throwingKnife] == 10);
        CHECK(t.player.pouch[bomb] == 0 && t.player.pouch[defs().itemById.at("poison_pot")] == 0 && t.player.readied == throwingKnife);  // (missing: none)
        CHECK(r.notes.size() == 1 && r.notes[0].contains("no such throwable"));
        CHECK(!t.player.twoHanded && t.player.gripBlend == 0);  // and one-handed (t was a: two-handed)
        write(bad, craft({{"HEAD", head}, {"GRIP", {1}}}));  // two-handed with nothing in hand: not holding anything
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && t.player.twoHanded && !twoHanding(t.player) && t.player.gripBlend == 0);
        auto& charm = const_cast<ItemType&>(itemType(BoneCharm));  // full by the gear's Flasks stat
        std::vector<Modifier> keep = charm.mods;
        charm.mods.push_back({Stat::Flasks, ModKind::Flat, 2, "test"});
        std::vector<uint8_t> inv{uint8_t(EquipCount)};
        for (int i = 0; i < EquipCount; ++i) inv.push_back(i == SlotTrinket ? uint8_t(BoneCharm) : 0);
        inv.push_back(0), inv.push_back(0);  // no bags, no containers
        write(bad, craft({{"HEAD", head}, {"INVT", inv}}));
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && t.player.inv.equipped[SlotTrinket] == BoneCharm && t.player.flasks == kBaseFlasks + 2);
        write(bad, craft({{"HEAD", head}, {"INVT", inv}, {"REST", {9, 0}}}));  // saved with more than the gear allows now
        CHECK(loadPlayer(bad.c_str(), t, 0x1234abcd, r).empty() && t.player.flasks == kBaseFlasks + 2);
        charm.mods = keep;
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
    s.player.teleport({5.5f, 4, 5.5f});  // beyond its chase range: it gives up, and goes
    int ticks = 0;  // (re-pinned for ADR-057: within 4 s, not 2: a grab under way plays out and recovers first)
    for (; ticks < 240 && !s.entities.empty(); ++ticks) step(s, {}, kTick);
    CHECK(s.entities.empty() && ticks < 240);

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
    // it's looted; none come back by day, and the dead never again (ADR-056).
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
    int slain = s.entities[0].slot;
    for (int i = 0; i < 4; ++i) step(s, {}, kTick);
    CHECK(s.entities.empty() && s.dead.at(night.id) == 1u << slain);
    s.timeOfDay = 0.9f;
    step(s, {}, kTick);
    CHECK(s.entities.size() == 1 && s.entities[0].slot == 1 - slain);  // only the living one's slot fills
    CHECK(damage(s.entities[0], 10000));
    s.entities[0].loot = BoneCharm;
    s.timeOfDay = 0.5f;
    for (int i = 0; i < int(kCorpseLinger * 60) + 2; ++i) step(s, {}, kTick);
    CHECK(s.entities.size() == 1);  // its loot keeps it
    s.player.teleport(s.entities[0].body.pos + glm::vec3(1, 0, 0));
    SimInput loot;
    loot.interact = true;
    step(s, loot, kTick);  // looted past kCorpseLinger: it goes at once
    CHECK(s.entities.empty() && s.player.inv.containers[0][2] == BoneCharm && s.dead.at(night.id) == 3);
    s.timeOfDay = 0.9f;
    for (int i = 0; i < 60; ++i) step(s, {}, kTick);
    CHECK(s.entities.empty());  // both dead for good: the night brings nobody

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
    copyWeaponClasses(dir);
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
                         {"id": "sword_pommel", "save_id": 14, "copy_from": "iron_shortsword", "name": "Pommel", "slot": null, "attack": null, "class": null,
                          "weight": null, "scaling": null}])", grown));
    if (!error.empty()) std::println(stderr, "FAIL grown data: {}", error);
    CHECK(grown.items.size() == 15 && grown.items[12].slot == SlotHead && grown.items[12].armor == 4 &&
          std::string_view(grown.items[12].name) == "Dented Helm" && grown.itemIds[12] == "dented_helm");
    const Attack& quick = grown.items[13].attack;
    CHECK(quick.minDamage == 6 && quick.maxDamage == 10 && quick.speed == 0.5f && grown.items[13].slot == SlotMainHand);
    CHECK(quick.scaling == grown.items[Shortsword].attack.scaling && quick.scaling[0] > 0);  // inherited (ADR-048)
    CHECK(grown.items[14].slot == SlotNone && grown.items[14].attack.speed == 0);
    std::string chain = base;  // 16 copies deep is fine, 17 isn't
    for (int i = 0; i <= 16; ++i)
        chain += std::format(R"(,{{"id": "c{}", "save_id": {}, "copy_from": "{}", "name": "C"}})", i, 12 + i, i ? std::format("c{}", i - 1) : "boar_skin");
    std::string chain16 = chain.substr(0, chain.rfind(",{")) + "]";
    CHECK(load(chain16, grown) && grown.items.size() == 28);
    {  // weapon_classes.json (ADR-044): one record per class, every attack move, its points in order
        std::ostringstream shipped;
        shipped << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "weapon_classes.json").rdbuf();
        std::string wc = shipped.str();
        auto edited = [&](std::string_view from, std::string_view to) {
            std::string t = wc;
            size_t at = t.find(from);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) t.replace(at, from.size(), to);
            return t;
        };
        std::string mace = wc.substr(wc.find(R"(,
  {"id": "mace")"));
        for (auto [text, want] : std::initializer_list<std::pair<std::string, std::string_view>>{
                 {edited(mace, "\n]\n"), "no record for the weapon class 'mace'"},
                 {edited(R"("id": "axe")", R"("id": "spear")"), "record 3 'spear': field 'id': must be one of fists, dagger, sword, axe, mace"},
                 {edited(R"("light_1": {"seconds": 0.7, "hold": 0.3, "hit": 0.4,)", R"("light_1": {"seconds": 0.7, "hold": 0.4, "hit": 0.4,)"),
                  "record 0 'sword': field 'moves.light_1.hit': 0.4 is out of range"},
                 {edited(R"("light_1": {"seconds": 0.7, "hold": 0.3, "hit": 0.4, "chain": 0.5, "cancel": 0.62,)",
                         R"("light_1": {"seconds": 0.7, "hold": 0.3, "hit": 0.4, "chain": 0.5, "cancel": 0.45,)"),
                  "record 0 'sword': field 'moves.light_1.cancel': 0.45 is out of range"},
                 {edited(R"("light_2": {"seconds": 0.65,)", R"("parry": {"seconds": 0.65,)"),
                  "record 0 'sword': field 'moves.parry': not an attack move (none, parry, drink and throw are code's)"},
                 {edited(R"("riposte": {"seconds": 1.4,)", R"("ripost": {"seconds": 1.4,)"),
                  "record 0 'sword': field 'moves.ripost': not an attack move"},
                 {edited(R"("light_1": {"seconds": 0.7, "hold": 0.3,)", R"("light_1": {"seconds": 0.7, "heavy": 1, "hold": 0.3,)"),
                  "record 0 'sword': field 'moves.light_1.heavy': must be true or false"},
                 {edited(R"("light_1": {"seconds": 0.7, "hold": 0.3,)", R"("light_1": {"seconds": 0.7, "pose": 1, "hold": 0.3,)"),
                  "record 0 'sword': field 'moves.light_1.pose': unknown field"},
                 {edited(R"("speed": 0.75)", R"("speed": 0)"), "record 1 'fists': field 'speed': 0 is out of range"},
                 // Great weapons (ADR-049): held in both hands, their reach and arc.
                 {edited(R"("hit_stop": 1.35, "two_handed": true)", R"("hit_stop": 1.35, "two_handed": 1)"),
                  "record 5 'greatsword': field 'two_handed': must be true or false"},
                 {edited(R"("id": "fists", "name": "Fists",)", R"("id": "fists", "name": "Fists", "two_handed": true,)"),
                  "record 1 'fists': field 'two_handed': bare hands have nothing to hold"},
                 {edited(R"("reach": 2.6)", R"("reach": 5)"), "record 5 'greatsword': field 'reach': 5 is out of range"},
                 {edited(R"("arc": 70)", R"("arc": 200)"), "record 6 'greataxe': field 'arc': 200 is out of range"},
                 // Bows (ADR-050): ranged classes take both hands and have only the shot; only they shoot.
                 {edited(R"("id": "sword", "name": "Straight Sword",)", R"("id": "sword", "name": "Straight Sword", "ranged": true,)"),
                  "record 0 'sword': field 'ranged': a ranged class takes both hands"},
                 {edited(R"("two_handed": true, "ranged": true)", R"("two_handed": true, "ranged": 1)"), "record 7 'bow': field 'ranged': must be true or false"},
                 {edited(R"("light_1": {"seconds": 0.7, "hold": 0.3,)", R"("shot": {"seconds": 1}, "light_1": {"seconds": 0.7, "hold": 0.3,)"),
                  "record 0 'sword': field 'moves.shot': only a ranged class shoots"},
                 {edited(R"("shot": {"seconds": 1.0,)", R"("light_1": {"seconds": 1}, "shot": {"seconds": 1.0,)"),
                  "record 7 'bow': field 'moves.light_1': a ranged class has only the shot"},
                 {edited(R"("shot": {"seconds": 1.0,)", R"("shoot": {"seconds": 1.0,)"), "record 7 'bow': field 'moves.shoot': not an attack move"},
                 // Crossbows (ADR-052): a ranged class that loads, and only it, reloads.
                 {edited(R"("ranged": true, "loads": true)", R"("ranged": true, "loads": 1)"), "record 8 'crossbow': field 'loads': must be true or false"},
                 {edited(R"("id": "sword", "name": "Straight Sword",)", R"("id": "sword", "name": "Straight Sword", "loads": true,)"),
                  "record 0 'sword': field 'loads': only a ranged class loads"},
                 {edited(R"("ranged": true, "moves")", R"("ranged": true, "loads": true, "moves")"), "record 7 'bow': field 'moves.reload': missing (a class that loads reloads)"},
                 {edited(R"("shot": {"seconds": 1.0,)", R"("reload": {"seconds": 1}, "shot": {"seconds": 1.0,)"),
                  "record 7 'bow': field 'moves.reload': only a class that loads reloads"},
                 {edited(R"("reload": {"seconds": 1.6,)", R"("light_1": {"seconds": 1}, "reload": {"seconds": 1.6,)"),
                  "record 8 'crossbow': field 'moves.light_1': a ranged class has only the shot (and the reload if it loads)"},
                 {[&] {
                      std::string t = wc;
                      size_t at = t.find(R"("shot": {)");
                      t.erase(at, t.find('}', at) + 1 - at);
                      return t;
                  }(),
                  "record 7 'bow': field 'moves.shot': missing (a ranged class has the shot)"},
             }) {
            std::ofstream(dir / "weapon_classes.json", std::ios::trunc) << text;
            Defs d;
            bool ok = load(base + "]", d);
            CHECK(!ok && error.contains(want) && error.contains("weapon_classes.json"));
            if (ok || !error.contains(want)) std::println(stderr, "FAIL weapon classes error: got \"{}\", want \"{}\"", error, want);
        }
        // A class missing a move (here the dagger's backstep) names it.
        std::string noBackstep = wc;
        size_t dagger = noBackstep.find(R"("id": "dagger")"), back = noBackstep.find(R"(,
     "backstep")", dagger);
        noBackstep.erase(back, noBackstep.find('}', back) + 1 - back);
        std::ofstream(dir / "weapon_classes.json", std::ios::trunc) << noBackstep;
        Defs d;
        CHECK(!load(base + "]", d) && error.contains("record 2 'dagger': field 'moves.backstep': missing (every class has every attack move)"));
        copyWeaponClasses(dir);
    }
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
             // Throwables (ADR-053): no slot, a look, a speed and a pouch; a knife sticks, a bomb or pot bursts.
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "slot": "trinket", "throw": {"look": "knife", "speed": 30, "pouch": 5}}])", "field 'throw': a throwable isn't worn (no slot)"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "rock", "speed": 30, "pouch": 5}}])", "field 'throw.look': must be one of"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "knife", "speed": 30, "pouch": 5, "radius": 2}}])",
              "field 'throw.radius': a knife sticks (no radius); a bomb or a pot bursts (a radius)"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "bomb", "speed": 30, "pouch": 5}}])", "field 'throw.radius': a knife sticks"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "knife", "speed": 30}}])", "field 'throw.pouch': missing"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "knife", "pouch": 5}}])", "field 'throw.speed': missing"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "knife", "speed": 30, "pouch": 5, "fuse": 1}}])", "field 'throw.fuse': unknown field"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "pot", "speed": 30, "pouch": 5, "radius": 2, "lob": 1}}])", "field 'throw.lob': must be true or false"},
             {base + R"(,)" + R"({"id": "a", "save_id": 12, "name": "A", "attack": {"min": 1, "max": 2, "speed": 1}, "throw": {"look": "knife", "speed": 30, "pouch": 5, "bleed": -1}}])", "field 'throw.bleed': -1 is out of range"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "throw": {"look": "knife", "speed": 30, "pouch": 5}}])",
              "field 'attack': missing (a throwable has its hit's damage roll)"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "main_hand"}])", "field 'attack': missing"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "attack": {"min": 1, "max": 2, "speed": 1}}])",
              "field 'attack': only main_hand and off_hand items and throwables attack"},
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
             // Weapon classes (ADR-044): every main-hand weapon names one, and only those.
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "main_hand", "attack": {"min": 1, "max": 2, "speed": 1}}])",
              "field 'class': missing (a main-hand weapon has a weapon class)"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_shortsword", "name": "A", "class": "spear"}])",
              "field 'class': must be one of dagger, sword, axe, mace"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_shortsword", "name": "A", "class": "fists"}])",
              "field 'class': must be one of dagger, sword, axe, mace"},
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "class": "axe"}])",
              "field 'class': only main_hand weapons have a weapon class"},
             // Scaling (ADR-048): weapons only, by the four attributes, 0..5 each.
             {base + R"(,{"id": "a", "save_id": 12, "name": "A", "slot": "head", "scaling": {"strength": 1}}])",
              "field 'scaling': only weapons (with an 'attack') scale"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_shortsword", "name": "A", "scaling": {"stamina": 1}}])",
              "field 'scaling.stamina': unknown attribute (strength, agility, intellect or spirit)"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_shortsword", "name": "A", "scaling": {"agility": 6}}])",
              "field 'scaling.agility': 6 is out of range 0..5"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_shortsword", "name": "A", "scaling": {"spirit": -0.5}}])",
              "field 'scaling.spirit': -0.5 is out of range 0..5"},
             {base + R"(,{"id": "a", "save_id": 12, "copy_from": "iron_shortsword", "name": "A", "scaling": 1}])",
              "field 'scaling': must be an object {strength, agility, intellect, spirit}"},
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
    CHECK(shipped.creatures.size() == size_t(EntityKind::Count) + 1 && shipped.creatureById.at("ghoul") == EntityKind::Ghoul &&
          shipped.creatures[1].name == nullptr && shipped.creatureIds[1] == "villager");
    writeCreatures(creatures + R"(,{"id": "townsfolk", "save_id": 10, "copy_from": "villager", "chase_range": null}])");
    CHECK(loadDefs(dir, grown, error));  // null is no AI number: fine on a harmless kind
    if (!error.empty()) std::println(stderr, "FAIL null AI key: {}", error);
    writeCreatures(creatures + R"(,{"id": "boar_sow", "save_id": 10, "copy_from": "boar", "name": null, "stats": {"stamina": 14}, "loot": "wolf_pelt"}])");
    CHECK(loadDefs(dir, grown, error) && grown.creatures.size() == 11 && grown.creatureById.at("boar_sow") == EntityKind(10));
    if (!error.empty()) std::println(stderr, "FAIL grown creatures: {}", error);
    const EntityType& sow = grown.creatures[10];
    CHECK(sow.name == nullptr && sow.stats.stamina == 14 && sow.stats.strength == 2 && sow.model == ModelBoar &&
          sow.temper == Temper::Passive && sow.loot == WolfPelt && sow.lootChance == 0.75f && !sow.immortal);
    for (auto [text, want] : std::initializer_list<std::pair<std::string, std::string_view>>{
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "loot": "boar_hide"}])",
              "record 10 'a': field 'loot': unknown item \"boar_hide\""},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "weapon": 3}])", "field 'weapon': unknown item 3"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "weapon": "boar_skin"}])",
              "field 'weapon': 'boar_skin' is not a main_hand item"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "model": "dragon"}])", "record 10 'a': field 'model': must be one of"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "model": "sword"}])", "field 'model': must be one of"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "temper": "angry"}])",
              "field 'temper': must be one of harmless, passive, aggressive"},
             {creatures + R"(,{"id": "a", "save_id": 3, "copy_from": "boar"}])", "field 'save_id': 3 is already 'bandit'"},
             {creatures + R"(,{"id": "a", "save_id": -1, "copy_from": "boar"}])", "field 'save_id': -1 is out of range 0..255"},
             {creatures + R"(,{"id": "a", "save_id": 11, "copy_from": "boar"}])", "no record has save_id 10 (save ids run 0..11"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "aggro_range": 5}])",
              "field 'aggro_range': only aggressive creatures attack on sight"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "loot": null}])", "field 'loot_chance': no 'loot' to drop"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "rare_loot_chance": 0.5}])", "field 'rare_loot_chance': no 'rare_loot' to drop"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "rare_loot": "wolf_pelt"}])", "field 'rare_loot': needs a rare_loot_chance"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "loot_chance": 1, "rare_loot": "wolf_pelt", "rare_loot_chance": 0.5}])",
              "field 'rare_loot': never dropped: loot_chance is 1"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "rare_loot": "boar_hide", "rare_loot_chance": 0.5}])",
              "field 'rare_loot': unknown item \"boar_hide\""},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "rare_loot": "wolf_pelt", "rare_loot_chance": 2}])",
              "field 'rare_loot_chance': 2 is out of range"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "stats": {"luck": 3}}])", "field 'stats.luck': unknown field"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "stats": {"stamina": null}}])", "field 'stats.stamina': missing"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "attackable": 1}])", "field 'attackable': must be true or false"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "stats": {"stamina": 0}}])", "field 'stats.stamina': 0 is out of range 1..1000"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "chase_speed": null}])",
              "field 'chase_speed': missing (a creature that fights chases)"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "loot_chance": 1.5}])", "field 'loot_chance': 1.5 is out of range"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "health": 50}])", "record 10 'a': field 'health': unknown field"},
             {creatures + R"(,{"id": "a", "save_id": 10, "model": "boar", "temper": "passive"}])", "field 'level': missing"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "respawn_seconds": 30}])", "field 'respawn_seconds': unknown field"},  // (ADR-056)
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "immortal": 1}])", "field 'immortal': must be true or false"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "dummy", "attackable": false}])", "field 'immortal': only attackable creatures are hit"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "immortal": true}])", "field 'immortal': it never dies to drop its loot"},
             {creatures + R"(,{"id": "wolf", "save_id": 10, "copy_from": "boar"}])", "record 10 'wolf': field 'id': duplicate of record 4"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "chase_range": 24}])",
              "field 'chase_range': must be more than aggro_range"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "leash_radius": 20}])", "field 'leash_radius': unknown field"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "flee_health": 1}])", "field 'flee_health': 1 is out of range 0..0.95"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "attack_windup": 0}])", "field 'attack_windup': 0 is out of range"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "villager", "chase_range": 40}])",
              "field 'chase_range': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "give_up_seconds": "soon"}])", "field 'give_up_seconds': must be a number"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "villager", "attackable": false, "stance": 10}])",
              "field 'stance': only attackable creatures are hit"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "dummy", "hit_stance": 10}])",
              "field 'hit_stance': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "dummy", "parries": 2}])", "field 'parries': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "parries": 2}])", "field 'parries': it can't be parried"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "damage": "pierce"}])",
              "field 'damage': its weapon decides (only creatures without one have their own)"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "dummy", "damage": "slash"}])", "field 'damage': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "wolf", "damage": "bite"}])", "field 'damage': must be one of slash, strike, pierce"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "blood": "green"}])", "field 'blood': must be red, splinters or dust"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "villager", "attackable": false, "resist": 0.2}])", "field 'resist': only attackable creatures are hit"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "wolf", "resist": {"slash": 2}}])", "field 'resist.slash': 2 is out of range -1..0.9"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "parryable": 1}])", "field 'parryable': must be true or false"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "villager", "combo": 2}])", "field 'combo': harmless creatures don't fight"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "combo": 5}])", "field 'combo': 5 is out of range 1..4"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "combo": null, "delay": 0.3}])",
              "field 'delay': only a hit after the first is delayed"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "shield": "boar_skin"}])", "field 'shield': 'boar_skin' is not a shield"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "wolf", "shield": "boar_skin"}])", "field 'shield': only creatures with hands hold one"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "ghoul", "guard": 0.5}])", "field 'guard': bare-handed creatures don't guard"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit_chief", "parry": 0.2}])", "field 'parry': nothing it holds parries"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "bandit", "guard": 0.5, "parry": 0.5}])",
              "field 'dodge': guard + parry + dodge are chances of one answer"},
             {creatures + R"(,{"id": "a", "save_id": 10, "copy_from": "boar", "wanders": false}])",
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
    writeCreatures(creatures + R"(,{"id": "boar_sow", "save_id": 10, "copy_from": "boar"}])");
    CHECK(reloadDefs(dir, error) && defs().creatures.size() == 11 && std::string_view(typeOf(EntityKind(10)).name) == "Boar");
    writeCreatures(creatures + R"(,{"id": "boar_runt", "save_id": 10, "copy_from": "boar"}])");
    CHECK(!reloadDefs(dir, error) && error.find("creature save_id 10 is now 'boar_runt', was 'boar_sow'") != std::string::npos);
    writeCreatures(creatures + "]");
    CHECK(!reloadDefs(dir, error) && error.find("10 creatures, fewer than the 11 loaded") != std::string::npos);
    CHECK(defs().creatures.size() == 11 && defs().creatureIds[10] == "boar_sow");
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
    CHECK(r["ok"] == true && marks.size() == 1 && marks[0].entity == EntityKind(10) && r["result"]["marker"]["entity"] == "boar_sow" &&
          marks[0].radius == 10);
    r = run(R"({"cmd": "catalog"})");
    CHECK(r["result"]["entities"].size() == 11 && r["result"]["entities"][10]["id"] == "boar_sow");
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
    CHECK(sows.size() == 1 && sows[0].kind == EntityKind(10) && std::string_view(sows[0].name) == "Boar");
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
    // Light splits 78 more faces than unlit (7703): the campfire's embers glow (1 before it). (The story world, with houses and the barrow, went
    // from 1772666 to 1863601 opaque quads with light, +5.1%.)
    CHECK(opaque == 7781 && water == 27 && shaded == 628);
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
    for (const char* f : {"items.json", "creatures.json", "skills.json", "talents.json", "weapon_classes.json"})
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
    // Who died is the dead record's (ADR-056): step() fills it each tick (buryDead); loot keeps the corpses lying.
    CHECK(damage(s.entities[0], 1000) && !is({.kind = C::NpcDead, .name = "Raiders"}));
    s.entities[0].loot = BoarSkin;
    buryDead(s.entities, s.dead, s.world, s.player);
    CHECK(!is({.kind = C::NpcDead, .name = "Raiders"}));
    CHECK(damage(s.entities[1], 1000) && !is({.kind = C::NpcDead, .name = "Raiders"}));
    s.entities[1].loot = BoarSkin;
    buryDead(s.entities, s.dead, s.world, s.player);
    CHECK(is({.kind = C::NpcDead, .name = "Raiders"}) && s.entities.size() == 2);
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
    awardHitXp(hero, villager, true, true);  // harmless, as the dummy: a quarter, and no kill XP (it can be hurt since ADR-056)
    CHECK(std::abs(bladeXp() - sword * (1 + 2 * kTrainingRate)) < 1e-4f && hero.progress.xp == 0 && villager.xpWear == 1);
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
        if (foe.boss) {  // a boss (ADR-059) is meant to be a long fight that trading blows loses: it takes reading its moves
            CHECK(toKill > 16 && downTime < killTime);
            continue;
        }
        CHECK(toKill >= 6 && toKill <= 16);  // never a one-shot, never a slog
        // A same-level fight alone is won. The exception: guards are the village's watch, meant to be
        // tougher than anything a player of their level should take on (passive: they never start it).
        if (d.creatureIds[k] == "guard") CHECK(downTime < killTime && foe.temper == Temper::Passive);
        else CHECK(downTime > killTime);
    }
    // Kill XP wears too: killing the same guard again and again (brought back by hand) gives less each time.
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
            updateCombat(slayer, gs, 60, rng);  // some wear fades...
            guard.body.health = float(maxHealth(sheet(guard.body)));  // ...and it's brought back by hand (nothing respawns: ADR-056)
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
    for (const char* f : {"items.json", "creatures.json", "weapon_classes.json"}) fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / f, data / f);
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
        CHECK(withXp(R"("id": "villager", "save_id": 1,)", R"( "xp": 5,)").empty());  // it can be hurt (ADR-056)
        {
            std::string c = creatures;
            c.replace(c.find(R"("attackable": true, "wanders": true, "talks": true, "model": "villager")"), 19, R"("xp": 5, )");
            std::ofstream(data / "creatures.json") << c;
            Defs out;
            CHECK(!loadDefs(data, out, error) && error.find("'villager': field 'xp': a creature that can't be hurt") != std::string::npos);
        }
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
    copyWeaponClasses(dir);
    fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
    std::ostringstream text;
    text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string creatures = text.str();
    creatures.erase(creatures.rfind(']'));
    std::ofstream(dir / "creatures.json") << creatures << R"(,
        {"id": "duelist", "save_id": 10, "copy_from": "guard", "guard": 1, "parry": null},
        {"id": "fencer", "save_id": 11, "copy_from": "bandit", "parry": 1, "dodge": null, "back_off": null},
        {"id": "dancer", "save_id": 12, "copy_from": "bandit", "dodge": 1, "parry": null, "back_off": 1}])";
    Defs test, shipped;
    std::string error;
    CHECK(loadDefs(dir, test, error) && loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
    if (!error.empty()) std::println(stderr, "FAIL enemies data: {}", error);
    fs::remove_all(dir);
    setDefs(std::move(test));
    const EntityKind kDuelist = EntityKind(10), kFencer = EntityKind(11), kDancer = EntityKind(12);

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
// Weapon classes (ADR-044): each weapon's class picks its moveset (data/weapon_classes.json), its poses
// and its model. The sword's is the moveset all weapons shared before; fists are the sword's, faster.
static void weaponClasses() {
    const Defs& d = defs();
    ItemId axe = d.itemById.at("iron_hand_axe"), mace = d.itemById.at("iron_mace");
    CHECK(classOf(NoItem) == WeaponClass::Fists && classOf(Shortsword) == WeaponClass::Sword && classOf(Knife) == WeaponClass::Dagger &&
          classOf(axe) == WeaponClass::Axe && classOf(mace) == WeaponClass::Mace);
    CHECK(heldModel(Shortsword) == ModelSword && heldModel(Knife) == ModelDagger && heldModel(axe) == ModelAxe && heldModel(mace) == ModelMace);
    // The sword's moveset is the one before ADR-044, number for number (the replays and the story fights pin it too).
    const MoveSpec &l1 = spec(WeaponClass::Sword, Move::Light1), &rip = spec(WeaponClass::Sword, Move::Riposte);
    CHECK(l1.seconds == 0.70f && l1.holdAt == 0.30f && l1.impact == 0.40f && l1.chain == 0.50f && l1.cancel == 0.62f && l1.stance == 10 &&
          l1.stamina == 12 && l1.lunge == 2.5f && !l1.heavy && !l1.thrust);
    CHECK(rip.damage == 3.0f && rip.thrust && spec(WeaponClass::Sword, Move::Heavy1).heavy && spec(WeaponClass::Sword, Move::GuardCounter).heavy);
    CHECK(weaponClass(WeaponClass::Fists).speed == 0.75f && weaponClass(WeaponClass::Sword).speed == 1);
    for (size_t m = 0; m < size_t(Move::Count); ++m) {  // fists swing the sword's moves; every class parries alike
        const MoveSpec &f = spec(WeaponClass::Fists, Move(m)), &sw = spec(WeaponClass::Sword, Move(m));
        CHECK(f.seconds == sw.seconds && f.impact == sw.impact && f.stance == sw.stance && f.heavy == sw.heavy);
        for (size_t c = 0; c < size_t(WeaponClass::Count); ++c)
            if (Move(m) == Move::Parry) CHECK(spec(WeaponClass(c), Move::Parry).seconds == kParrySpec.seconds);
    }
    // Their character: daggers quick and light on stance and stamina, then swords, axes, maces the most stance.
    auto stance = [](WeaponClass c) {
        float sum = 0;
        for (Move m : {Move::Light1, Move::Light2, Move::Light3, Move::Light4, Move::Heavy1, Move::Heavy2}) sum += spec(c, m).stance;
        return sum;
    };
    CHECK(stance(WeaponClass::Dagger) < stance(WeaponClass::Sword) && stance(WeaponClass::Sword) < stance(WeaponClass::Axe) &&
          stance(WeaponClass::Axe) < stance(WeaponClass::Mace));
    CHECK(spec(WeaponClass::Dagger, Move::Light1).stamina < spec(WeaponClass::Sword, Move::Light1).stamina &&
          spec(WeaponClass::Sword, Move::Light1).stamina < spec(WeaponClass::Axe, Move::Light1).stamina);
    CHECK(spec(WeaponClass::Dagger, Move::Riposte).damage > spec(WeaponClass::Sword, Move::Riposte).damage);  // good for crits
    // The weapon's speed scales its class's moves: the knife's (0.75) lights are the quickest, the mace's (1.15) the slowest.
    auto light = [](ItemId w) {
        Player p;
        p.inv.equipped[SlotMainHand] = w;
        startMove(p, Move::Light1, {});
        return moveLength(p);
    };
    CHECK(light(Knife) < light(NoItem) && light(NoItem) < light(Shortsword) && light(Shortsword) < light(axe) && light(axe) < light(mace));
    CHECK(std::abs(light(NoItem) - 0.75f * light(Shortsword)) < 1e-5f);  // fists: the sword's at 0.75 (the old fist scale)
    for (ItemId w : {Knife, axe, mace}) {  // a swing spends its class's stamina
        Player p;
        p.onGround = true, p.offGround = 0;
        p.inv.equipped[SlotMainHand] = w;
        Input move;
        updateMelee(p, {.attack = true}, move, 1 / 60.0f);
        updateMelee(p, {}, move, 1 / 60.0f);
        CHECK(p.move == Move::Light1 && p.stamina == gMelee.staminaMax - spec(classOf(w), Move::Light1).stamina);
    }

    // What their hits do (ADR-040): a dagger's stabs pierce and its slashes slash, an axe never pierces,
    // a mace always strikes.
    Grid g = flatWorld(0);
    std::mt19937 rng(9);
    auto swingWith = [](ItemId w, Move m) {
        Player p;
        p.inv.equipped[SlotMainHand] = w;
        startMove(p, m, {});
        return damageOf(p);
    };
    CHECK(swingWith(Knife, Move::Light1) == DamageType::Slash && swingWith(Knife, Move::Light3) == DamageType::Pierce &&
          swingWith(Shortsword, Move::Light4) == DamageType::Pierce);
    for (size_t m = 1; m < size_t(Move::Count); ++m) {
        if (Move(m) == Move::Parry) continue;
        CHECK(swingWith(axe, Move(m)) == DamageType::Slash && swingWith(mace, Move(m)) == DamageType::Strike);
    }

    // Every class's light chain lands all four hits on a dummy in reach, each with its class's stance
    // damage and hit-stop.
    for (ItemId w : {NoItem, Knife, Shortsword, axe, mace}) {
        Player p;
        p.pos = {7.5f, 4, 7.5f};
        p.onGround = true, p.offGround = 0;
        p.inv.equipped[SlotMainHand] = w;
        std::vector<Entity> es;
        es.reserve(2);
        spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 1.6f);
        Player& dummy = es[0].body;
        WeaponClass c = classOf(w);
        int hits = 0;
        Move want = Move::Light1;
        for (int i = 0; i < 6 * 60 && hits < 4; ++i) {
            Input move;
            MeleeInput in{.attack = !p.punching() || moveProgress(p) >= spec(p).chain};
            updateMelee(p, in, move, 1 / 60.0f);
            dummy.lastDamage = 0;  // (the dummy is immortal: its last hit's number tells, ADR-056)
            dummy.stance = 1000;  // never broken: no riposte
            Move m = p.move;
            updateCombat(p, es, 1 / 60.0f, rng);
            if (dummy.lastDamage > 0) {
                CHECK(m == want);
                if (m != want) std::println(stderr, "  {}: hit {} with {}, want {}", kWeaponClassIds[size_t(c)], hits, kMoveNames[size_t(m)], kMoveNames[size_t(want)]);
                float stop = std::min(0.04f + 0.003f * spec(c, m).stance, 0.12f) * weaponClass(c).hitStop;
                CHECK(std::abs(p.freeze - stop) < 1e-5f);
                ++hits, want = Move(int(want) + 1);
                dummy.health = float(maxHealth(sheet(dummy)));
                p.stamina = gMelee.staminaMax;
            }
        }
        same("weaponClasses", std::format("{} chain hits", kWeaponClassIds[size_t(c)]), hits, 4);
    }
    {  // A riposte whose broken stance is gone lands as a plain hit: its class's light 1 stance damage.
        Player p;
        p.pos = {7.5f, 4, 7.5f};
        p.onGround = true, p.offGround = 0;
        p.inv.equipped[SlotMainHand] = Knife;
        std::vector<Entity> es;
        es.reserve(2);
        spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 1.6f);
        startMove(p, Move::Riposte, {});
        for (int i = 0; i < 3 * 60 && p.hitPending; ++i) {
            advanceMove(p, 1 / 60.0f);
            updateCombat(p, es, 1 / 60.0f, rng);
        }
        CHECK(!p.hitPending && es[0].body.hitStance == spec(WeaponClass::Dagger, Move::Light1).stance);
    }
    CHECK(weaponClass(WeaponClass::Dagger).hitStop < 1 && weaponClass(WeaponClass::Mace).hitStop > weaponClass(WeaponClass::Axe).hitStop &&
          weaponClass(WeaponClass::Axe).hitStop > 1);
}

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
        CHECK(criticalTarget(es, p) == nullptr);
        staggerHit(bandit, bandit.body.stance, false);  // the rest of it
        CHECK(bandit.body.broken && bandit.body.stagger == gMelee.breakSeconds && bandit.body.stance == bandit.type().stance);
        CHECK(criticalTarget(es, p) == &bandit);
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
        staggerHit(guard, spec(WeaponClass::Sword, Move::Light1).stance, false);
        CHECK(guard.body.stagger == 0);
        staggerHit(guard, spec(WeaponClass::Sword, Move::Light4).stance, false);
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
        // Two-handing (ADR-048): the shield goes on the back and guards nothing; the weapon in both hands
        // guards better than in one, short of a shield.
        p.twoHanded = true;
        auto [twoHurt, twoStamina] = blocked(heater);
        CHECK(!shieldOf(p) && guardOf(p).negation == gMelee.twoHandNegation && guardOf(p).boost == gMelee.twoHandBoost);
        CHECK(twoHurt > 0 && twoHurt <= swordHurt && twoStamina < swordStamina && twoStamina > heaterStamina);
        CHECK(gMelee.twoHandNegation > gMelee.guardNegation && gMelee.twoHandBoost > gMelee.guardBoost);
        p.twoHanded = false;
    }
    {  // The parry: a small or medium shield's, for stamina. A hit in its window deflects, the attacker
       // reels open to a riposte; caught before or after it, the parry is a weak guard; from behind, nothing.
        Player p = armored();
        std::vector<Entity> es = duel(p);
        Entity& bandit = es[0];
        p.inv.equipped[SlotOffHand] = tower;  // a greatshield can't
        CHECK(tick(p, {.parry = true}, es) == Move::None && p.want == Want::None && p.stamina == gMelee.staminaMax);
        p.inv.equipped[SlotOffHand] = buckler;
        CHECK(tick(p, {.parry = true}, es) == Move::Parry && p.stamina == gMelee.staminaMax - kParrySpec.stamina);
        CHECK(p.moveScale == itemType(buckler).shield.parry && !p.hitPending);
        for (int i = 0; i < 60 && !parries(p, bandit.body.pos); ++i) tick(p, {}, es);
        CHECK(strikeNow(p, es, {}) == 0 && bandit.body.broken && bandit.body.stagger > 1 && p.stagger == 0);
        updateEntities(es, g, p, dt);
        CHECK(bandit.state == AiState::Stagger && criticalTarget(es, p) == &bandit);
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
        for (int i = 0; i < 120 && moveProgress(p) < kParrySpec.holdAt; ++i) tick(p, {}, none);  // wound up: across the body
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

// Criticals (ADR-045): crit chance and damage in the stat pipeline, the backstab's geometry and when
// it's open, a backstab that holds its NPC until it lands, random crits both ways, the camera trauma.
static void criticals() {
    Grid g = flatWorld(0);
    constexpr float dt = 1 / 60.0f;
    constexpr float kPi = std::numbers::pi_v<float>;
    std::mt19937 rng(7);
    StatSheet base = compute(Stats{});
    CHECK(base[Stat::CritChance] == kBaseCritChance && base[Stat::CritDamage] == kBaseCritDamage);
    CHECK(percentStat(Stat::CritChance) && percentStat(Stat::CritDamage) && percentStat(Stat::Haste) && !percentStat(Stat::Armor));
    Modifier sharp{Stat::CritChance, ModKind::Flat, 0.1f}, brutal{Stat::CritDamage, ModKind::Increased, 1};
    StatSheet modded = compute(Stats{}, std::array{sharp, brutal});
    CHECK(std::abs(modded[Stat::CritChance] - 0.15f) < 1e-6f && std::abs(modded[Stat::CritDamage] - 1.0f) < 1e-6f);
    Player armed;
    armed.inv.equipped[SlotMainHand] = Knife;
    CHECK(std::abs(sheet(armed)[Stat::CritChance] - 0.10f) < 1e-6f);  // the knife's +5%: daggers are for crits

    auto fighter = [] {
        Player p;
        p.pos = {7.5f, 4, 7.5f};
        p.onGround = true, p.offGround = 0;
        p.inv.equipped[SlotMainHand] = Shortsword;
        return p;
    };
    // One tick in step()'s order: the player's buttons (an attack at an NPC open to a critical is the
    // riposte move, aimed at it), the hold, the NPCs think, then combat.
    auto tick = [&](Player& p, MeleeInput in, std::vector<Entity>& es) {
        if (const Entity* e = criticalTarget(es, p, p.move == Move::Riposte && p.hitPending))
            in.riposte = true, in.aim = {e->body.pos.x - p.pos.x, e->body.pos.z - p.pos.z};
        Input move;
        updateMelee(p, in, move, dt);
        holdCritical(es, p);
        updateEntities(es, g, p, dt);
        updateCombat(p, es, dt, rng);
        return p.move;
    };

    {  // Behind: within 60 degrees of its back; open: and not attacking, rolling, reeling or just hurt.
        Player p = fighter();
        p.yaw = 0;  // facing +Z
        std::vector<Entity> es;
        Entity& b = spawnAhead(es, g, p, EntityKind::Bandit, 1, 1.5f);
        b.body.yaw = 0;  // facing away from p
        CHECK(behind(b, p) && opensToCritical(b, p) && criticalTarget(es, p) == &b);
        b.body.yaw = kPi;  // facing p
        CHECK(!behind(b, p) && !opensToCritical(b, p) && criticalTarget(es, p) == nullptr);
        b.body.yaw = kPi / 2;  // side on
        CHECK(!behind(b, p));
        b.body.yaw = glm::radians(50.0f);
        CHECK(behind(b, p));
        b.body.yaw = glm::radians(70.0f);
        CHECK(!behind(b, p));
        b.body.yaw = 0;
        Entity e = b;
        e.body.hitTime = gMelee.backstabWait - 0.1f;  // just hurt: it's turning
        CHECK(behind(e, p) && !opensToCritical(e, p));
        e = b, e.body.stagger = 0.3f;
        CHECK(!opensToCritical(e, p));
        e = b, e.body.swingTimer = 0;
        CHECK(startPunch(e.body, {0, 1}) && !opensToCritical(e, p));  // attacking
        e = b, e.body.broken = true, e.body.yaw = kPi;
        CHECK(opensToCritical(e, p));  // a broken stance from the front: a riposte
        p.yaw = kPi;  // the player turned away: out of reach
        CHECK(criticalTarget(es, p) == nullptr);
    }
    // The backstab: the riposte move; its NPC can't turn or act until it lands, then it's down. On a
    // boar that hasn't noticed (passive), and on a bandit turned away while it recovers from a swing.
    auto backstab = [&](EntityKind kind, bool recovering) {
        Player p = fighter();
        p.yaw = 0;
        std::vector<Entity> es;
        Entity& b = spawnAhead(es, g, p, kind, 1, 1.5f);
        b.body.yaw = 0, b.body.onGround = true;
        if (recovering) setState(b, AiState::Recover, "test"), b.body.swingTimer = 1;
        float health = b.body.health, yaw = b.body.yaw;
        CHECK(tick(p, {.attack = true}, es) == Move::Riposte);
        bool held = true;
        for (int i = 0; i < 120 && p.hitPending; ++i) {
            tick(p, {}, es);
            held &= !p.hitPending || (b.body.stagger > 0 && std::abs(b.body.yaw - yaw) < 1e-4f && !b.body.punching());
        }
        CHECK(held && !p.hitPending);
        CHECK(health - b.body.health > 12 && b.body.lastCrit && b.body.made(Noise::Crit));  // more than any plain sword hit
        CHECK(!b.body.broken && b.body.stagger > gMelee.downSeconds - 0.1f && b.body.hitStance == 100);  // knocked down
        CHECK(p.trauma > gMelee.hitShake - 0.1f);  // a full critical's shake
        CHECK(!opensToCritical(b, p));  // down...
        Entity up = b;
        up.body.stagger = 0, up.body.yaw = yaw;
        CHECK(behind(up, p) && !opensToCritical(up, p));  // ...and, up again with its back turned, just hurt
        for (int i = 0; i < 60; ++i) tick(p, {}, es);
        CHECK(p.trauma == 0);  // a second wears it off
    };
    backstab(EntityKind::Boar, false);
    backstab(EntityKind::Bandit, true);
    {  // An aggressive bandit that has seen the player turns and swings first: no backstab.
        Player p = fighter();
        p.yaw = 0;
        std::vector<Entity> es;
        Entity& b = spawnAhead(es, g, p, EntityKind::Bandit, 1, 1.5f);
        b.body.yaw = 0, b.body.onGround = true;
        updateEntities(es, g, p, dt);  // it notices
        CHECK(tick(p, {.attack = true}, es) != Move::Riposte && b.body.punching());
    }
    {  // Two boars: the downed one nearer, back still turned, isn't caught again; the open one further off is.
        Player p = fighter();
        p.yaw = 0;
        std::vector<Entity> es;
        es.reserve(2);  // `down` must outlive the second spawn
        Entity& down = spawnAhead(es, g, p, EntityKind::Boar, 1, 1.0f);
        down.body.yaw = 0;
        staggerHit(down, 0, true);
        down.body.hitTime = 0.2f;  // just backstabbed
        p.pos.x += 0.4f;
        Entity& open = spawnAhead(es, g, p, EntityKind::Boar, 2, 1.4f);
        p.pos.x -= 0.4f;
        open.body.yaw = 0;
        CHECK(behind(down, p) && behind(open, p) && criticalTarget(es, p) == &open && criticalTarget(es, p, true) == &open);
        float downHealth = down.body.health, openHealth = open.body.health;
        CHECK(tick(p, {.attack = true}, es) == Move::Riposte);
        for (int i = 0; i < 120 && p.hitPending; ++i) tick(p, {}, es);
        CHECK(down.body.health == downHealth && open.body.health < openHealth && open.body.lastCrit);
    }
    {  // Facing it: an attack is a plain light, no critical.
        Player p = fighter();
        p.yaw = 0;
        std::vector<Entity> es;
        Entity& b = spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 1.5f);
        b.body.yaw = kPi;
        CHECK(tick(p, {.attack = true}, es) == Move::Light1);
    }
    {  // Random crits: the player's on the dummy at the knife's 10%, for +50%; a plain light doesn't shake the camera.
        Player p = fighter();
        p.inv.equipped[SlotMainHand] = Knife;
        p.yaw = 0;
        std::vector<Entity> es;
        Entity& d = spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 1.5f);
        d.body.yaw = kPi;  // facing p: no backstabs
        d.body.innate = {{Stat::BleedResist, ModKind::Flat, 1e5f}};  // and no bleeds (ADR-046): the crits alone
        int hits = 0, crits = 0;
        float plain = 0, critical = 0, most = 0;
        for (int i = 0; i < 60 * 60 * 15 && hits < 600; ++i) {
            d.body.lastDamage = 0;  // (immortal: the hit's number tells, ADR-056)
            d.body.stance = d.type().stance;  // never broken: no ripostes
            tick(p, {.attack = !p.punching()}, es);
            if (d.body.lastDamage > 0) {
                ++hits;
                (d.body.lastCrit ? critical : plain) += float(d.body.lastDamage);
                crits += d.body.lastCrit;
                if (!d.body.lastCrit) most = std::max(most, p.trauma);
            }
            d.body.health = float(maxHealth(sheet(d.body)));
        }
        float rate = float(crits) / float(hits), ratio = (critical / float(crits)) / (plain / float(hits - crits));
        std::println("criticals: {} of {} knife hits critical ({:.1f}%), x{:.2f} damage", crits, hits, double(rate * 100), double(ratio));
        CHECK(hits == 600 && rate > 0.06f && rate < 0.14f && ratio > 1.35f && ratio < 1.65f);
        CHECK(most == 0);  // a light's stance (under 20) doesn't shake
    }
    {  // ...and an NPC's on the player, at its 10% (a bandit's knife): it shakes the camera by the share of health taken.
        Player p = fighter();
        p.yaw = 0;
        std::vector<Entity> es;
        Entity& b = spawnAhead(es, g, p, EntityKind::Bandit, 1, 1.5f);
        b.body.onGround = true;
        p.innate = {{Stat::BleedResist, ModKind::Flat, 1e5f}};  // its knife's bleeds aside (ADR-046)
        int hits = 0, crits = 0;
        bool shook = true;
        for (int i = 0; i < 60 * 60 * 20 && hits < 400; ++i) {
            if (!b.body.punching() && b.body.swingTimer <= 0) startPunch(b.body, {0, -1});
            float before = p.health;
            Input still;
            updateMelee(p, {}, still, dt);
            updateCombat(p, es, dt, rng);
            if (p.health < before) ++hits, crits += p.lastCrit, shook &= p.trauma > 0;
            p.health = float(maxHealth(sheet(p)));
            p.trauma = 0, p.stagger = 0;
        }
        float rate = float(crits) / float(hits);
        std::println("criticals: {} of {} bandit hits on the player critical ({:.1f}%)", crits, hits, double(rate * 100));
        CHECK(hits == 400 && rate > 0.05f && rate < 0.16f && shook);
    }
}

// Healing flasks and rest points (ADR-047): the drink is a move that heals at its drink frame, a hit
// before it cancels it, an empty flask only gestures; resting by a fire fills the flasks, heals, clears
// statuses and is where the player gets back up.
static void flasks() {
    Grid g = flatWorld(0);
    constexpr float dt = 1 / 60.0f;
    constexpr float kHalfPi = std::numbers::pi_v<float> / 2;
    auto xzOf = [](glm::vec3 v) { return glm::vec2(v.x, v.z); };
    std::mt19937 rng(5);
    StatSheet base = compute(Stats{});
    CHECK(maxFlasks(base) == kBaseFlasks && base[Stat::FlaskHealing] == kBaseFlaskHealing);
    CHECK(percentStat(Stat::FlaskHealing) && !percentStat(Stat::Flasks) && std::string_view(kStatIds[size_t(Stat::Flasks)]) == "flasks");
    CHECK(maxFlasks(compute(Stats{}, std::array{Modifier{Stat::Flasks, ModKind::Flat, 2}})) == kBaseFlasks + 2);  // a talent's
    CHECK(maxFlasks(compute(Stats{}, std::array{Modifier{Stat::Flasks, ModKind::Flat, -10}})) == 0);  // never below none
    for (int c = 0; c < int(WeaponClass::Count); ++c) CHECK(spec(WeaponClass(c), Move::Drink).seconds == kDrinkSpec.seconds);
    CHECK(Player{}.flasks == kBaseFlasks && codeMove(Move::Drink) && !codeMove(Move::Light1));

    auto drinker = [] {
        Player p;
        p.pos = {7.5f, 4, 7.5f};
        p.onGround = true, p.offGround = 0;
        p.inv.equipped[SlotMainHand] = Shortsword;
        p.health = 20;
        p.combatTimer = 60;  // no regeneration muddling the numbers
        return p;
    };
    std::vector<Entity> none;
    auto tick = [&](Player& p, MeleeInput in = {}, Input move = {}) {
        updateMelee(p, in, move, dt);
        updatePlayer(p, g, move, dt);
        updateCombat(p, none, dt, rng);
        return move;
    };
    const int most = maxHealth(sheet(drinker()));

    {  // The drink: it commits; the heal (40% of max health) comes at the drink frame, a charge spent.
        Player p = drinker();
        tick(p, {.drink = true});
        CHECK(p.move == Move::Drink && p.hitPending && p.moveScale == 1 && !hyperarmor(p));
        int frames = 1;
        while (p.health == 20 && frames < 200) {
            p.noises = 0;
            tick(p), ++frames;
        }
        CHECK(std::abs(float(frames) * dt - impactTime(p)) <= dt + 1e-4f);  // at the drink frame, not before
        CHECK(p.health == 20 + kBaseFlaskHealing * float(most) && p.flasks == kBaseFlasks - 1 && p.made(Noise::Drink) && !p.made(Noise::Empty));
        CHECK(p.move == Move::Drink && !p.hitPending);  // lowering the flask
        while (p.move == Move::Drink && frames < 400) tick(p), ++frames;
        CHECK(std::abs(float(frames) * dt - kDrinkSpec.seconds) <= dt + 1e-4f && p.health == 20 + kBaseFlaskHealing * float(most));
        p.health = float(most) - 5;  // never past max health, not even on the drink frame
        tick(p, {.drink = true});
        for (int i = 0; i < 120 && p.flasks == kBaseFlasks - 1; ++i) tick(p);
        CHECK(p.health == float(most) && p.flasks == kBaseFlasks - 2);
    }
    {  // It walks slowly and turns; it can't attack, guard or roll until its last quarter.
        Player p = drinker();
        tick(p, {.drink = true});
        Input walk{.move = {1, 0}, .run = true};
        Input m = tick(p, {}, walk);
        CHECK(m.speed == gMelee.drinkSpeed && !m.run && m.move == walk.move);
        for (int i = 0; i < 30; ++i) m = tick(p, {.guard = true}, walk);
        CHECK(std::abs(glm::length(glm::vec2(p.vel.x, p.vel.z)) - gMelee.drinkSpeed) < 0.05f && !p.guarding && std::abs(p.yaw - kHalfPi) < 0.2f);
        CHECK(tick(p, {.attack = true}).roll == false && p.move == Move::Drink);
        CHECK(tick(p, {}, {.roll = true}).roll == false && p.move == Move::Drink);
        while (moveProgress(p) < kDrinkSpec.cancel && p.move == Move::Drink) tick(p);
        CHECK(p.move == Move::Drink);
        Player q = p;
        CHECK(tick(p, {}, {.roll = true}).roll && p.move == Move::None);  // cut short by a roll at the end...
        CHECK(tick(q, {}, walk).speed == 0 && q.move == Move::None);       // ...or walking on at full speed
    }
    {  // A hit that makes it flinch before the drink frame cancels it: no heal, the charge kept.
        Player p = drinker();
        tick(p, {.drink = true});
        for (int i = 0; i < 20; ++i) tick(p);
        interrupt(p, gMelee.flinchSeconds, false);  // what updateCombat does to a flinch (the drink has no hyperarmor)
        for (int i = 0; i < 120; ++i) tick(p);
        CHECK(p.health == 20 && p.flasks == kBaseFlasks && p.move == Move::None);
    }
    {  // An NPC in front doesn't take a drink for an attack: it guards, parries or dodges only those (ADR-043).
        Player p = drinker();
        std::vector<Entity> es;
        Entity& b = spawnAhead(es, g, p, EntityKind::Guard, 4, 2);
        setState(b, AiState::Chase, "test");
        Input m;
        updateMelee(p, {.drink = true}, m, dt);
        updateEntities(es, g, p, dt);
        CHECK(p.move == Move::Drink && p.hitPending && !b.threatened);
        p = drinker();
        updateMelee(p, {.attack = true}, m, dt), updateMelee(p, {}, m, dt);
        updateEntities(es, g, p, dt);
        CHECK(p.move == Move::Light1 && b.threatened);  // as an attack is
    }
    {  // Empty: the gesture plays, nothing heals.
        Player p = drinker();
        p.flasks = 0;
        tick(p, {.drink = true});
        CHECK(p.move == Move::Drink);
        bool empty = false;
        for (int i = 0; i < 120; ++i) p.noises = 0, tick(p), empty |= p.made(Noise::Empty);
        CHECK(empty && p.health == 20 && p.flasks == 0);
    }
    {  // Not in the air or in the water: the press waits (buffered) for the ground.
        Player p = drinker();
        p.onGround = false, p.offGround = 1;
        Input m;
        updateMelee(p, {.drink = true}, m, dt);
        CHECK(p.move == Move::None && p.want == Want::Drink);
        p.onGround = true, p.offGround = 0;
        updateMelee(p, {}, m, dt);
        CHECK(p.move == Move::Drink);
        Player w = drinker();
        w.inWater = true;
        updateMelee(w, {.drink = true}, m, dt);
        CHECK(w.move == Move::None);
    }
    {  // Mid-attack, a drink press waits for the attack's cancel point.
        Player p = drinker();
        tick(p, {.attack = true}), tick(p);
        CHECK(p.move == Move::Light1);
        tick(p, {.drink = true});
        CHECK(p.move == Move::Light1);
        for (int i = 0; i < 60 && p.move != Move::Drink; ++i) tick(p);
        CHECK(p.move == Move::Drink);
    }

    // Rest points: an Ember within kRestReach, out of combat, nothing angry near.
    Session s{.world = flatWorld(0), .seed = 3};
    startSession(s);
    s.player.teleport({7.5f, 4, 7.5f});
    CHECK(!canRest(s));  // no fire
    s.world.set({10, 4, 7}, Ember);
    CHECK(canRest(s));
    s.world.set({10, 4, 7}, Air), s.world.set({11, 4, 7}, Ember);
    CHECK(!canRest(s));  // 3.5 blocks off
    s.world.set({11, 4, 7}, Air), s.world.set({10, 4, 7}, Ember);
    s.player.hitTime = 1;  // hit a second ago
    CHECK(!canRest(s));
    s.player.hitTime = 99;
    Entity& wolf = spawnAhead(s.entities, s.world, s.player, EntityKind::Wolf, 9);
    setState(wolf, AiState::Chase, "test");
    CHECK(!canRest(s));  // something angry near
    setState(wolf, AiState::Search, "test");
    CHECK(!canRest(s));  // or searching for the player (ADR-055): ducking out of sight isn't a rest
    s.entities.clear();
    Player& p = s.player;
    p.health = 30, p.flasks = 0, p.stamina = 10;
    p.buildup[0] = 50;
    afflict(p, Status::Poison, "Venom");
    for (int i = 0; i < 150; ++i) step(s, {}, kTick);  // its ticks keep the combat timer up, but don't stop a rest
    CHECK(p.effect(Effect::Poisoned) && p.combatTimer > 0 && canRest(s));
    StepResult r = step(s, {.interact = true}, kTick);
    CHECK(r.rested && p.health == float(maxHealth(sheet(p))) && p.flasks == kBaseFlasks && p.stamina == gMelee.staminaMax);
    CHECK(p.effects.empty() && p.buildup[0] == 0 && s.restPoint && glm::distance(*s.restPoint, p.pos) < 0.1f);
    CHECK(std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Rest && h.kind < 0; }));
    // Knocked out, it gets back up there, flasks full; never rested, at the player spawn.
    glm::vec3 restAt = *s.restPoint;
    p.teleport({3.5f, 4, 3.5f});
    p.flasks = 0, p.health = 0, p.deadTime = kPlayerRespawnSeconds;
    step(s, {}, kTick);
    CHECK(p.alive() && glm::distance(p.pos, restAt) < 0.2f && p.flasks == kBaseFlasks);
    s.restPoint.reset();
    p.health = 0, p.deadTime = kPlayerRespawnSeconds;
    step(s, {}, kTick);
    CHECK(glm::distance(xzOf(p.pos), xzOf(playerSpawn(s.world, s.markers))) < 0.2f);
}

// Weapon scaling (ADR-048): each weapon's hits add its share of each attribute, graded E..S; the
// Scaling stat multiplies it. Two-handing: a toggle that waits for a free body, strength x1.5 through
// the stat pipeline, no shield guard or parry, the left hand on the hilt and the shield on the back.
static void scaling() {
    constexpr float dt = 1 / 60.0f;
    std::mt19937 rng(5);
    // Grades: none, then E (a little) up to S.
    for (auto [k, grade] : {std::pair{0.0f, '-'}, {0.1f, 'E'}, {0.25f, 'D'}, {0.5f, 'C'}, {0.8f, 'B'}, {1.1f, 'A'}, {1.4f, 'S'}, {3.0f, 'S'}})
        if (scalingGrade(k) != grade) std::println(stderr, "FAIL scalingGrade({}) = {}, want {}", k, scalingGrade(k), grade), ++failures;
    CHECK(scalingGrade(0.24f) == 'E' && scalingGrade(0.49f) == 'D' && scalingGrade(0.79f) == 'C' && scalingGrade(1.09f) == 'B' && scalingGrade(1.39f) == 'A');
    // The bonus: each attribute x its scaling x the Scaling stat x speed, summed, then rounded.
    Stats st{10, 4, 6, 8, 10};
    StatSheet sh = compute(st);
    CHECK(sh[Stat::Scaling] == 1 && std::string_view(kStatIds[size_t(Stat::Scaling)]) == "scaling");
    for (size_t i = 0; i < 4; ++i) {
        Attack a{1, 2, 2.0f, {}};
        a.scaling[i] = 0.5f;
        CHECK(attributeShare(sh, a, i) == sh[kScalingStats[i]] * 0.5f * 2 && attributeBonus(sh, a) == int(sh[kScalingStats[i]]));
        for (size_t j = 0; j < 4; ++j) CHECK(j == i || attributeShare(sh, a, j) == 0);
    }
    Attack mixed{1, 2, 1.0f, {0.25f, 0.25f, 0, 0}};  // 1 + 1.5 = 2.5: rounded once, at the end (not 1 + 2)
    CHECK(attributeBonus(sh, mixed) == 3);
    CHECK(attributeBonus(compute(st, std::array{Modifier{Stat::Scaling, ModKind::Increased, 0.5f}}), mixed) == 4);  // a talent's: x1.5 = 3.75
    CHECK(kPunch.scaling == (Scaling{1, 0, 0, 0}) && (Attack{1, 2, 1}).scaling == kPunch.scaling);  // fists and creatures: strength
    CHECK(hitBeforeArmor(sh, mixed, 5) == 5 + 3);
    // The shipped weapons: the knife leans on agility, the mace and axe on strength, the sword on both.
    const Attack &sword = itemType(Shortsword).attack, &knife = itemType(Knife).attack,
                 &axe = defs().items[defs().itemById.at("iron_hand_axe")].attack, &mace = defs().items[defs().itemById.at("iron_mace")].attack;
    CHECK(knife.scaling[1] > knife.scaling[0] && scalingGrade(knife.scaling[1]) == 'A');
    CHECK(mace.scaling[0] > 0 && mace.scaling[1] == 0 && scalingGrade(mace.scaling[0]) == 'S' && axe.scaling[0] > axe.scaling[1]);
    CHECK(sword.scaling[0] > 0 && sword.scaling[1] > 0);
    for (const Attack* a : {&sword, &knife, &axe, &mace}) CHECK(a->scaling[2] == 0 && a->scaling[3] == 0);  // nothing scales with magic yet
    // So the build picks the weapon: agility adds to the knife and not the mace, strength the other way round.
    StatSheet quick = compute({10, 2, 40, 2, 2}), strong = compute({10, 40, 2, 2, 2});
    CHECK(attributeBonus(quick, knife) > attributeBonus(quick, mace) && attributeBonus(strong, mace) > attributeBonus(strong, knife));
    // At the starting attributes the sword and knife hit as before scaling (the story's balance holds).
    CHECK(attributeBonus(Stats{}, sword) == 2 && attributeBonus(Stats{}, knife) == 2);

    // Two-handing: the toggle.
    auto holder = [] {
        Player p;
        p.pos = {7.5f, 4, 7.5f};
        p.onGround = true, p.offGround = 0;
        p.inv.equipped[SlotMainHand] = defs().itemById.at("iron_mace");
        return p;
    };
    auto tick = [&](Player& p, MeleeInput in = {}) {
        Input move;
        updateMelee(p, in, move, dt);
        std::vector<Entity> none;
        updateCombat(p, none, dt, rng);
    };
    {
        Player p = holder();
        CHECK(!twoHanding(p) && p.gripBlend == 0);
        float one = sheet(p)[Stat::Strength];
        tick(p, {.grip = true});
        CHECK(p.twoHanded && twoHanding(p) && p.made(Noise::Grip) && p.want == Want::None);
        CHECK(sheet(p)[Stat::Strength] == one * gMelee.twoHandStrength);  // x1.5, in the sheet (and the UIs)
        std::vector<Modifier> mods = collect(p);
        CHECK(std::ranges::count_if(mods, [](const Modifier& m) { return std::string_view(m.source) == "Two-handed" && m.stat == Stat::Strength; }) == 1);
        CHECK(attributeBonus(sheet(p), mace) > attributeBonus(sheet(holder()), mace));
        for (int i = 0; i < 30; ++i) tick(p);
        CHECK(p.gripBlend > 0.99f);  // the left hand went to the hilt
        p.noises = 0;
        tick(p, {.grip = true});
        CHECK(!p.twoHanded && p.made(Noise::Grip) && sheet(p)[Stat::Strength] == one);
        // Damage: the same roll hits harder two-handed.
        Player dummy;
        p.twoHanded = true;
        std::mt19937 keep = rng;
        int two = resolveHit(p, dummy, mace, {rng});
        rng = keep, p.twoHanded = false;
        int oneHit = resolveHit(p, dummy, mace, {rng});
        CHECK(two > oneHit);
    }
    {  // Bare hands have nothing to hold in two: the press does nothing. Putting the weapon away lets go.
        Player p = holder();
        p.inv.equipped[SlotMainHand] = NoItem;
        tick(p, {.grip = true});
        CHECK(!p.twoHanded && p.want == Want::None && !p.made(Noise::Grip));
        p.twoHanded = true;  // (a stale flag): no weapon, not two-handing, and the next tick clears it
        CHECK(!twoHanding(p) && sheet(p)[Stat::Strength] == compute(Stats{})[Stat::Strength]);
        tick(p);
        CHECK(!p.twoHanded);
        p.inv.equipped[SlotMainHand] = Shortsword;
        CHECK(!twoHanding(p));  // the next weapon starts one-handed
        p.twoHanded = true;
        p.inv.equipped[SlotMainHand] = Knife;  // swapped straight for another: still in both hands
        tick(p);
        CHECK(twoHanding(p));
    }
    {  // Put away mid-swing: the swing keeps its grip (and strength) to the end, a press queued meanwhile is dropped.
        Player p = holder();
        p.twoHanded = true;
        float two = sheet(p)[Stat::Strength];
        tick(p, {.attack = true});
        tick(p);
        CHECK(p.punching());
        p.inv.equipped[SlotMainHand] = NoItem;
        tick(p, {.grip = true});
        CHECK(twoHanding(p) && sheet(p)[Stat::Strength] == two && p.want == Want::Grip);
        p.noises = 0;
        while (p.punching()) tick(p);
        tick(p);
        CHECK(!p.twoHanded && p.want == Want::None && !p.made(Noise::Grip));
    }
    {  // It waits for a free body: mid-swing (until its cancel point) and mid-roll.
        Player p = holder();
        tick(p, {.attack = true});
        tick(p);
        while (p.punching() && moveProgress(p) < spec(p).cancel - 0.2f) tick(p);  // pressed a while before the swing can be cut
        CHECK(p.punching());
        float left = (1 - moveProgress(p)) * moveLength(p);  // until the swing would end by itself
        tick(p, {.grip = true});
        CHECK(!p.twoHanded && p.want == Want::Grip);
        int frames = 0;
        while (!p.twoHanded && frames < 120) tick(p), ++frames;
        CHECK(p.twoHanded && !p.punching() && float(frames) * dt < left - 0.1f);  // at the cancel point: the press cut the recovery
        Player r = holder();
        r.rollTime = 0.1f, r.rollLength = 0.6f;
        CHECK(r.rolling());
        tick(r, {.grip = true});
        CHECK(!r.twoHanded && r.want == Want::Grip);
    }
    {  // No parry two-handed, not even a knife's (or a shield's: it's on the back).
        Player p = holder();
        p.inv.equipped[SlotMainHand] = Knife;
        CHECK(parryOf(p) > 0);
        p.twoHanded = true;
        CHECK(parryOf(p) == 0);
        tick(p, {.parry = true});
        CHECK(p.move == Move::None && p.want == Want::None);
        p.inv.equipped[SlotOffHand] = defs().itemById.at("oak_buckler");
        CHECK(parryOf(p) == 0 && !shieldOf(p));
        p.twoHanded = false;
        CHECK(parryOf(p) > 0 && shieldOf(p));
    }
    {  // The pose: two-handed, the left hand on the hilt just below the right, in every pose; the shield on the back.
        auto center = [](const glm::mat4& m) { return glm::vec3(m * glm::vec4(1.5f, 1.5f, 1.5f, 1)); };  // a hand's middle
        Player p = holder();
        p.yaw = 0;
        p.inv.equipped[SlotOffHand] = defs().itemById.at("heater_shield");
        glm::mat4 held(0), shield(0);
        auto one = poseModel(p, ModelPlayer, &held, 1, &shield);
        CHECK(glm::distance(center(one[HandL]), center(one[HandR])) > 0.8f);
        glm::vec3 shieldAt(shield[3]);
        p.twoHanded = true, p.gripBlend = 1;
        for (int pose = 0; pose < 3; ++pose) {  // at rest, guarding, mid-swing
            if (pose == 1) p.guardBlend = 1;
            if (pose == 2) p.guardBlend = 0, startMove(p, Move::Light1, {}), p.moveTime = impactTime(p);
            glm::mat4 h(0), sh(0);
            auto two = poseModel(p, ModelPlayer, &h, 1, &sh);
            float apart = glm::distance(center(two[HandL]), center(two[HandR]));
            if (std::abs(apart - 0.27f) > 0.03f) std::println(stderr, "FAIL two-handed pose {}: hands {} apart", pose, apart), ++failures;
            // The left hand is down the weapon's length from the grip (toward the pommel), not off to a side.
            glm::vec3 up = glm::normalize(glm::vec3(h * glm::vec4(0, 1, 0, 0)));
            CHECK(glm::dot(glm::normalize(center(two[HandL]) - center(two[HandR])), -up) > 0.95f);
            glm::vec3 torso(two[Torso] * glm::vec4(4, 3.5f, 3, 1));
            CHECK(sh != glm::mat4(0) && glm::vec3(sh[3]).z < torso.z && glm::vec3(sh[3]) != shieldAt);  // behind the torso (the body faces +z)
            float back = glm::vec3(two[Torso] * glm::vec4(4, 3.5f, 0, 1)).z, board = glm::vec3(sh * glm::vec4(4.5f, 6, 0.5f, 1)).z;
            CHECK(board < back);  // its board outside the torso's back, not through it
        }
        p.move = Move::None, p.hitPending = false;
        {  // mid-roll the weapon goes where it does one-handed (tucked), not held out in front
            Player r = p, one = p;
            r.rollTime = one.rollTime = 0.3f, r.rollLength = one.rollLength = 0.6f;
            one.twoHanded = false, one.gripBlend = 0;
            CHECK(glm::distance(center(poseModel(r, ModelPlayer)[HandR]), center(poseModel(one, ModelPlayer)[HandR])) < 0.05f);
        }
        startMove(p, Move::Drink, {});
        p.moveTime = holdTime(p);  // the flask at the lips: the left hand left the hilt for it
        glm::mat4 h(0);
        auto drinking = poseModel(p, ModelPlayer, &h);
        CHECK(glm::distance(center(drinking[HandL]), center(drinking[HandR])) > 0.5f);
    }
}

// Great weapons (ADR-049): greatswords and greataxes, always in both hands (strength x1.5, no shield or
// parry, the grip button does nothing), slow and heavy on stance, hyperarmor on their heavies, a longer
// and wider reach; dropped rarely by bandits and ghouls; held with both hands on the long grip.
static void greatWeapons() {
    constexpr float dt = 1 / 60.0f;
    std::mt19937 rng(7);
    const Defs& d = defs();
    ItemId gs = d.itemById.at("iron_greatsword"), ga = d.itemById.at("barrow_greataxe"), mace = d.itemById.at("iron_mace");
    CHECK(classOf(gs) == WeaponClass::Greatsword && classOf(ga) == WeaponClass::Greataxe);
    CHECK(heldModel(gs) == ModelGreatsword && heldModel(ga) == ModelGreataxe);
    for (size_t c = 0; c < size_t(WeaponClass::Count); ++c) {  // only the great classes (and the bows, ADR-050/052) are two-handed; they reach farther and wider
        const WeaponClassDef& w = weaponClass(WeaponClass(c));
        bool great = WeaponClass(c) == WeaponClass::Greatsword || WeaponClass(c) == WeaponClass::Greataxe;
        CHECK(w.twoHanded == (great || w.ranged) && (w.reach > 2.4f) == great && (w.arc > 65) == great);
        CHECK(great || (w.reach == 2 && w.arc == 60 && w.arcCos == 0.5f));  // the one-handed reach is the old kPunchReach's, 60 degrees (exactly the old 0.5)
        CHECK(std::abs(w.arcCos - std::cos(glm::radians(w.arc))) < 1e-6f);
    }
    // Strength weapons, heavier than any one-handed one; the greataxe the slowest and the most strength.
    const Attack &a = itemType(gs).attack, &x = itemType(ga).attack;
    CHECK(scalingGrade(a.scaling[0]) == 'A' && scalingGrade(x.scaling[0]) == 'S' && a.scaling[1] > x.scaling[1]);
    CHECK(itemType(gs).weight > itemType(mace).weight && itemType(ga).weight > itemType(gs).weight && itemType(gs).damage == DamageType::Slash);
    auto stance = [](WeaponClass c) {
        float sum = 0;
        for (Move m : {Move::Light1, Move::Light2, Move::Light3, Move::Light4, Move::Heavy1, Move::Heavy2}) sum += spec(c, m).stance;
        return sum;
    };
    CHECK(stance(WeaponClass::Mace) < stance(WeaponClass::Greatsword) && stance(WeaponClass::Greatsword) < stance(WeaponClass::Greataxe));
    CHECK(weaponClass(WeaponClass::Greatsword).hitStop > weaponClass(WeaponClass::Mace).hitStop &&
          weaponClass(WeaponClass::Greataxe).hitStop > weaponClass(WeaponClass::Greatsword).hitStop);
    for (WeaponClass c : {WeaponClass::Greatsword, WeaponClass::Greataxe}) {  // hyperarmor on the heavies, none on the lights
        CHECK(spec(c, Move::Heavy1).heavy && spec(c, Move::Heavy2).heavy && spec(c, Move::JumpHeavy).heavy);
        CHECK(!spec(c, Move::Light1).heavy && !spec(c, Move::Light4).heavy);
        CHECK(spec(c, Move::Light1).stamina > spec(WeaponClass::Mace, Move::Light1).stamina);
    }
    auto holder = [&](ItemId w) {
        Player p;
        p.pos = {7.5f, 4, 7.5f};
        p.onGround = true, p.offGround = 0;
        p.inv.equipped[SlotMainHand] = w;
        return p;
    };
    auto tick = [&](Player& p, MeleeInput in = {}, std::vector<Entity>* es = nullptr) {
        Input move;
        updateMelee(p, in, move, dt);
        std::vector<Entity> none;
        updateCombat(p, es ? *es : none, dt, rng);
    };
    auto light = [&](ItemId w) {
        Player p = holder(w);
        startMove(p, Move::Light1, {});
        return std::pair{moveLength(p), impactTime(p)};
    };
    CHECK(light(mace).first < light(gs).first && light(gs).first < light(ga).first && light(mace).second < light(gs).second);
    {  // Always in both hands: no flag needed, strength x1.5, the shield on the back, no parry; the grip button does nothing.
        Player p = holder(gs);
        p.inv.equipped[SlotOffHand] = d.itemById.at("oak_buckler");
        CHECK(!p.twoHanded && twoHanding(p) && !shieldOf(p) && parryOf(p) == 0);
        CHECK(sheet(p)[Stat::Strength] == sheet(Player{})[Stat::Strength] * gMelee.twoHandStrength);
        CHECK(guardOf(p, DamageType::Slash).negation == gMelee.twoHandNegation);
        tick(p, {.grip = true});
        CHECK(!p.twoHanded && twoHanding(p) && p.want == Want::None && !p.made(Noise::Grip));
        for (int i = 0; i < 30; ++i) tick(p);
        CHECK(p.gripBlend > 0.99f);
        tick(p, {.parry = true});
        CHECK(p.move == Move::None);
        tick(p, {.attack = true});  // nor does it cut a swing's recovery short, as a press that did something would
        tick(p);
        while (p.punching() && moveProgress(p) < spec(p).cancel + 0.02f) tick(p);
        tick(p, {.grip = true});
        CHECK(p.punching() && p.want == Want::None);
        while (p.punching()) tick(p);
        p.inv.equipped[SlotMainHand] = Shortsword;  // swapped for a one-handed weapon: one-handed again, the shield back in hand
        CHECK(!twoHanding(p) && shieldOf(p) && parryOf(p) > 0);
        p.twoHanded = true;  // two-handing the sword, through a great weapon and back: the sword's grip is kept
        p.inv.equipped[SlotMainHand] = ga;
        tick(p, {.grip = true});
        p.inv.equipped[SlotMainHand] = Shortsword;
        CHECK(p.twoHanded && twoHanding(p));
        // Swapped for the greatsword mid-swing, a grip press cutting the swing's recovery doesn't flip the sword's grip.
        tick(p, {.attack = true});
        tick(p);
        CHECK(p.punching());
        p.inv.equipped[SlotMainHand] = gs;
        while (p.punching() && moveProgress(p) < spec(p).cancel - 0.05f) tick(p);
        p.noises = 0;
        for (int i = 0; i < 20; ++i) tick(p, {.grip = i == 0});
        p.inv.equipped[SlotMainHand] = Shortsword;
        CHECK(p.twoHanded && !p.made(Noise::Grip) && p.want == Want::None);
    }
    {  // Hyperarmor: an NPC hit in a heavy's windup doesn't make it flinch; in a light's it does.
        for (Move m : {Move::Light1, Move::Heavy1}) {
            Player p = holder(gs);
            startMove(p, m, {});
            CHECK(hyperarmor(p) == (m == Move::Heavy1));
        }
    }
    // Reach: a dummy 2.3 blocks ahead is out of a sword's reach, in a greatsword's and a greataxe's; one
    // 70 degrees off the facing (close) is in the greatsword's arc (75) only. Each lands a light (or not).
    Grid g = flatWorld(0);
    auto lands = [&](ItemId w, float dist, float degrees) {
        Player p = holder(w);
        std::vector<Entity> es;
        es.reserve(2);
        p.yaw = glm::radians(degrees);
        spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, dist);
        p.yaw = 0;
        tick(p, {.attack = true}, &es);
        for (int i = 0; i < 120 && p.punching(); ++i) tick(p, {}, &es);
        return es[0].body.lastDamage > 0;  // (immortal: the hit's number, ADR-056)
    };
    CHECK(lands(Shortsword, 1.6f, 0) && !lands(Shortsword, 2.3f, 0) && lands(gs, 2.3f, 0) && lands(ga, 2.3f, 0) && !lands(gs, 2.8f, 0));
    CHECK(!lands(Shortsword, 1.5f, 70) && lands(gs, 1.5f, 70) && !lands(ga, 1.5f, 72));
    CHECK(lands(gs, 1.5f, -70));  // either side
    {  // An NPC's swing reaches by its weapon too: a greatsword-wielding guard's at 2.3, a sword's not.
        Player npc = holder(gs);
        npc.yaw = 0;
        CHECK(inReach(npc, npc.pos + glm::vec3(0, 0, 2.3f)));
        npc.inv.equipped[SlotMainHand] = Shortsword;
        CHECK(!inReach(npc, npc.pos + glm::vec3(0, 0, 2.3f)));
    }
    {  // An NPC sees a swing coming by its weapon (ADR-043's answers): 74 degrees off, or 3.8 blocks ahead,
       // a greatsword's swing threatens it and a sword's doesn't.
        for (auto [degrees, dist] : {std::pair{74.0f, 2.4f}, std::pair{0.0f, 3.8f}})
            for (ItemId w : {gs, Shortsword}) {
                Player p = holder(w);
                std::vector<Entity> es;
                es.reserve(2);
                p.yaw = glm::radians(degrees);
                Entity& b = spawnAhead(es, g, p, EntityKind::Guard, 4, dist);
                p.yaw = 0;
                setState(b, AiState::Chase, "test");
                Input m;
                updateMelee(p, {.attack = true}, m, dt), updateMelee(p, {}, m, dt);
                updateEntities(es, g, p, dt);
                CHECK(p.punching() && b.threatened == (w == gs));
            }
    }
    {  // The swing reaches by the weapon it swings: a sword's swing, the greatsword swapped in mid-windup, misses at 2.3.
        Player p = holder(Shortsword);
        p.yaw = 0;
        std::vector<Entity> es;
        es.reserve(2);
        spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 2.3f);
        tick(p, {.attack = true}, &es);
        tick(p, {}, &es);
        p.inv.equipped[SlotMainHand] = gs;
        for (int i = 0; i < 120 && p.punching(); ++i) tick(p, {}, &es);
        CHECK(es[0].body.lastDamage == 0);
    }
    {  // A backstab reaches as far: a dummy 2.3 blocks ahead with its back turned is open to the greatsword's, not the sword's.
        Player p = holder(gs);
        p.yaw = 0;
        std::vector<Entity> es;
        es.reserve(2);
        Entity& e = spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 2.3f);
        e.body.yaw = 0, e.body.hitTime = 99;  // facing away
        CHECK(criticalTarget(es, p) == &e);
        p.inv.equipped[SlotMainHand] = Shortsword;
        CHECK(!criticalTarget(es, p));
    }
    // The light chain lands all four hits with each class's stance damage and hit-stop.
    for (ItemId w : {gs, ga}) {
        Player p = holder(w);
        std::vector<Entity> es;
        es.reserve(2);
        spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 1.6f);
        Player& dummy = es[0].body;
        WeaponClass c = classOf(w);
        int hits = 0;
        Move want = Move::Light1;
        for (int i = 0; i < 8 * 60 && hits < 4; ++i) {
            Input move;
            updateMelee(p, {.attack = !p.punching() || moveProgress(p) >= spec(p).chain}, move, dt);
            dummy.lastDamage = 0;  // (immortal: the hit's number tells, ADR-056)
            dummy.stance = 1000;
            Move m = p.move;
            updateCombat(p, es, dt, rng);
            if (dummy.lastDamage > 0) {
                CHECK(m == want && dummy.hitStance == spec(c, m).stance);
                CHECK(std::abs(p.freeze - std::min(0.04f + 0.003f * spec(c, m).stance, 0.12f) * weaponClass(c).hitStop) < 1e-5f);
                ++hits, want = Move(int(want) + 1);
                dummy.health = float(maxHealth(sheet(dummy)));
                p.stamina = gMelee.staminaMax;
            }
        }
        same("greatWeapons", std::format("{} chain hits", kWeaponClassIds[size_t(c)]), hits, 4);
    }
    {  // What a level 1 body does with each per second of light swings on an unarmored dummy (for balance; printed).
        Player p = holder(NoItem);
        for (ItemId w : {Shortsword, mace, gs, ga}) {
            p.inv.equipped[SlotMainHand] = w;
            const Attack& at = itemType(w).attack;
            float per = (float(at.minDamage + at.maxDamage) / 2 + float(attributeBonus(sheet(p, skillOf(w)), at))) * spec(classOf(w), Move::Light1).damage;
            std::println("great weapons: {} light 1 {:.1f} damage, {:.2f} s, {:.1f} per second", itemType(w).name, per, light(w).first, per / light(w).first);
        }
    }

    // Found: a bandit's corpse holds its knife 40% of the time, else rarely a greatsword; a ghoul's its
    // charm, else rarely a greataxe. A creature without a rare drop rolls once, as before.
    const EntityType &bandit = typeOf(EntityKind::Bandit), &ghoul = typeOf(EntityKind::Ghoul);
    CHECK(bandit.rareLoot == gs && bandit.rareChance == 0.1f && ghoul.rareLoot == ga && ghoul.rareChance == 0.12f);
    CHECK(!typeOf(EntityKind::BanditChief).rareLoot && typeOf(EntityKind::BanditChief).rareChance == 0);
    for (EntityKind k : {EntityKind::Bandit, EntityKind::Ghoul}) {
        const EntityType& t = typeOf(k);
        int usual = 0, rare = 0, n = 4000;
        for (int i = 0; i < n; ++i) {
            std::vector<Entity> es;
            Entity& e = spawnAhead(es, g, holder(NoItem), k, uint32_t(i), 3);
            CHECK(damage(e, 100000));
            usual += e.loot == t.loot, rare += e.loot == t.rareLoot;
        }
        float want = (1 - t.lootChance) * t.rareChance * float(n);
        CHECK(std::abs(float(usual) - t.lootChance * float(n)) < 0.05f * float(n) && std::abs(float(rare) - want) < 0.3f * want);
        std::println("great weapons: {} of {} {} corpses hold a {} ({:.0f} expected), {} the usual drop", rare, n, kEntityKindNames[size_t(k)],
                     itemType(t.rareLoot).name, double(want), usual);
    }
    {  // Same seed, same rolls: the usual drop's roll is the first draw either way.
        std::vector<Entity> es;
        Entity& e = spawnAhead(es, g, holder(NoItem), EntityKind::Boar, 3, 3);
        std::mt19937 r = e.rng;
        damage(e, 100000);
        CHECK((e.loot == BoarSkin) == (std::uniform_real_distribution<float>(0, 1)(r) < typeOf(EntityKind::Boar).lootChance) && e.rng == r);
        int failed = 0;
        for (uint32_t seed = 0; seed < 40; ++seed) {  // a boar (no rare drop): one roll, made or missed
            std::vector<Entity> bs;
            Entity& b = spawnAhead(bs, g, holder(NoItem), EntityKind::Boar, seed, 3);
            std::mt19937 q = b.rng;
            damage(b, 100000);
            bool drops = std::uniform_real_distribution<float>(0, 1)(q) < typeOf(EntityKind::Boar).lootChance;
            CHECK((b.loot == BoarSkin) == drops && b.rng == q);
            failed += !drops;
        }
        CHECK(failed > 3);
        for (uint32_t seed = 0; seed < 40; ++seed) {  // a bandit: the knife's roll first, then (missed) the greatsword's, nothing more
            std::vector<Entity> bs;
            Entity& b = spawnAhead(bs, g, holder(NoItem), EntityKind::Bandit, seed, 3);
            std::mt19937 q = b.rng;
            damage(b, 100000);
            auto draw = [&] { return std::uniform_real_distribution<float>(0, 1)(q); };
            ItemId want = draw() < bandit.lootChance ? bandit.loot : draw() < bandit.rareChance ? bandit.rareLoot : NoItem;
            CHECK(b.loot == want && b.rng == q);
        }
    }

    {  // The pose: both hands on the long grip, the left pommel() below the right, at rest, guarding and mid-swing.
        auto center = [](const glm::mat4& m) { return glm::vec3(m * glm::vec4(1.5f, 1.5f, 1.5f, 1)); };
        for (auto [w, apartWant] : {std::pair{gs, 0.30f}, std::pair{ga, 0.35f}}) {
            Player p = holder(w);
            p.yaw = 0, p.gripBlend = 1;
            for (int pose = 0; pose < 3; ++pose) {
                if (pose == 1) p.guardBlend = 1;
                if (pose == 2) p.guardBlend = 0, startMove(p, Move::Light1, {}), p.moveTime = impactTime(p);
                glm::mat4 h(0);
                auto two = poseModel(p, ModelPlayer, &h);
                float apart = glm::distance(center(two[HandL]), center(two[HandR]));
                if (std::abs(apart - apartWant) > 0.03f) std::println(stderr, "FAIL {} pose {}: hands {} apart", itemType(w).name, pose, apart), ++failures;
                glm::vec3 up = glm::normalize(glm::vec3(h * glm::vec4(0, 1, 0, 0)));
                CHECK(glm::dot(glm::normalize(center(two[HandL]) - center(two[HandR])), -up) > 0.95f);
                // In the model's voxels: the right hand just under the crossguard (a greataxe's grip ends at 7), the left above the pommel.
                float right = (glm::inverse(h) * glm::vec4(center(two[HandR]), 1)).y, left = (glm::inverse(h) * glm::vec4(center(two[HandL]), 1)).y;
                float guard = w == gs ? 6 : 8;
                CHECK(right > guard - 2.5f && right < guard && left > 0.5f && left < right);
            }
        }
        // Long blades: through every move, start to end, the tip and the pommel (an axe's beard) stay out of the ground.
        for (auto [w, tip, beard] : {std::tuple{gs, glm::vec4(3.5f, 23.5f, 0.5f, 1), glm::vec4(3.5f, 0.5f, 0.5f, 1)},
                                     std::tuple{ga, glm::vec4(7.5f, 26.5f, 0.5f, 1), glm::vec4(7.5f, 15.5f, 0.5f, 1)}}) {
            for (size_t m = 1; m < size_t(Move::Count); ++m) {
                if (codeMove(Move(m))) continue;
                Player p = holder(w);
                p.yaw = 0, p.gripBlend = 1;
                startMove(p, Move(m), {});
                float low = 99;
                for (float u = 0; u <= 1.001f; u += 0.02f) {
                    p.moveTime = std::min(u, 1.0f) * moveLength(p);
                    glm::mat4 h(0);
                    poseModel(p, ModelPlayer, &h);
                    low = std::min({low, (h * tip).y - p.pos.y, (h * beard).y - p.pos.y, (h * glm::vec4(3.5f, 0.5f, 0.5f, 1)).y - p.pos.y});
                }
                if (low < -0.1f) std::println(stderr, "FAIL {} {}: down to {:.2f} below the ground", itemType(w).name, kMoveNames[m], low), ++failures;
            }
            for (float guard : {0.0f, 1.0f}) {  // at rest and guarding
                Player p = holder(w);
                p.gripBlend = 1, p.guardBlend = guard;
                glm::mat4 h(0);
                poseModel(p, ModelPlayer, &h);
                CHECK(std::min({(h * tip).y, (h * beard).y, (h * glm::vec4(3.5f, 0.5f, 0.5f, 1)).y}) - p.pos.y > 0);
            }
        }
    }
}

// Status effects (ADR-046): build-up and resists in the stat pipeline, from items and creatures;
// build-up that fills and wears off; bleed's burst, poison's ticks, frostbite's damage taken; a guard's
// share; the lockout while poison or frost is on; the timed effects' list; kills by poison; the shipped
// sources, in fights both ways.
static void statusEffects() {
    Grid g = flatWorld(0);
    constexpr float dt = 1 / 60.0f;
    std::mt19937 rng(11);
    std::vector<Entity> none;
    StatSheet base = compute(Stats{});
    for (int i = 0; i < int(Status::Count); ++i) {
        Status s = Status(i);
        CHECK(base[buildupStat(s)] == 0 && base[resistStat(s)] == kBaseStatusResist && !percentStat(buildupStat(s)));
        CHECK(std::string_view(kStatIds[size_t(buildupStat(s))]) == kStatusIds[i]);
        CHECK(std::string_view(kStatIds[size_t(resistStat(s))]) == std::string(kStatusIds[i]) + "_resist");
    }
    // Sources: the knife bleeds, a wolf's bite bleeds, a ghoul's claws poison and it shrugs poison off.
    Player knifer;
    knifer.inv.equipped[SlotMainHand] = Knife;
    CHECK(sheet(knifer)[Stat::Bleed] == 30 && sheet(knifer)[Stat::Poison] == 0);
    Player at;
    at.pos = {7.5f, 4, 7.5f};
    std::vector<Entity> beasts;
    beasts.reserve(2);
    Entity& wolf = spawnAhead(beasts, g, at, EntityKind::Wolf, 1);
    Entity& ghoul = spawnAhead(beasts, g, at, EntityKind::Ghoul, 2, 8);
    CHECK(sheet(wolf.body)[Stat::Bleed] == 22 && sheet(wolf.body)[Stat::BleedResist] == kBaseStatusResist);
    CHECK(sheet(ghoul.body)[Stat::Poison] == 25 && sheet(ghoul.body)[Stat::PoisonResist] == 500);
    CHECK(collect(wolf.body).size() == 1 && std::string_view(collect(wolf.body)[0].source) == "innate");

    // Build-up: two bites of 40 fill 80 of 100; the third fills it: a bleed of 10% of max health + 5, and
    // the bar starts over. A guard that lets half through builds half.
    Player biter, p;
    biter.innate = {{Stat::Bleed, ModKind::Flat, 40}};
    auto bite = [&](float share = 1) { return buildUp(biter, p, share, "Biter")[size_t(Status::Bleed)]; };
    CHECK(bite() == 0 && bite() == 0 && p.buildup[0] == 80 && !p.made(Noise::Bleed));
    CHECK(bite() == 15 && p.buildup[0] == 0 && p.made(Noise::Bleed) && p.effects.empty());
    CHECK(bite(0.5f) == 0 && p.buildup[0] == 20 && bite(0) == 0 && p.buildup[0] == 20);
    // It wears off at gStatus.decay a second.
    for (int i = 0; i < 60; ++i) updateCombat(p, none, dt, rng);
    CHECK(std::abs(p.buildup[0] - (20 - gStatus.decay)) < 0.01f);
    for (int i = 0; i < 180; ++i) updateCombat(p, none, dt, rng);
    CHECK(p.buildup[0] == 0);
    // A higher resist takes more: the whole iron set's 350.
    for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) p.inv.equipped[itemType(id).slot] = id;
    for (int i = 0; i < 8; ++i) CHECK(bite() == 0);
    CHECK(bite() == 15 && p.buildup[0] == 0);  // the 9th of 40
    p.inv = {};

    // Poison: a timed effect, 1 a second (0.5% of 100 + 0.5) for 10 s. Its ticks are damage numbers, not
    // hits (a back still open to a backstab), and keep the body in combat. No poison builds while it's on.
    Player venom;
    venom.innate = {{Stat::Poison, ModKind::Flat, 100}};
    p = Player{};
    CHECK(buildUp(venom, p, 1, "Venom")[size_t(Status::Poison)] == 0 && p.made(Noise::Poison));
    const TimedEffect* poisoned = p.effect(Effect::Poisoned);
    CHECK(poisoned && poisoned->seconds == gStatus.poisonSeconds && poisoned->length == gStatus.poisonSeconds &&
          poisoned->source == "Venom" && poisoned->stacks == 1 && poisoned->mods.empty() && p.buildup[1] == 0);
    Player sting;
    sting.innate = {{Stat::Poison, ModKind::Flat, 30}};
    buildUp(sting, p, 1, "Sting");
    CHECK(p.buildup[1] == 0 && p.effects.size() == 1 && p.effects[0].source == "Venom");  // locked out
    int ticks = 0;
    float hit = p.hitTime;
    for (int i = 0; i < 11 * 60; ++i) {
        float before = p.health;
        updateCombat(p, none, dt, rng);
        if (p.health < before) {
            ++ticks;
            CHECK(before - p.health == 1 && p.combatTimer == kCombatSeconds && p.lastDamage == 0);  // a number, not a hit
            CHECK(std::ranges::any_of(p.numbers, [](const DamageNumber& n) { return n.age == 0 && n.kind == Status::Poison && n.amount == 1; }));
        }
    }
    CHECK(ticks == 10 && p.health == 90 && p.effects.empty() && p.hitTime > hit);
    CHECK(buildUp(venom, p, 1, "Venom")[1] == 0 && p.effect(Effect::Poisoned));  // over: it builds again

    // Frostbite: a burst of 10% + 3, then 20% more damage taken (a modifier in the pipeline) for 15 s;
    // a fresh one replaces it.
    p = Player{};
    CHECK(afflict(p, Status::Frost, "Ice") == 13 && p.made(Noise::Frost) && p.buildup[2] == 0);
    CHECK(std::abs(sheet(p)[Stat::DamageTaken] - 1.2f) < 1e-5f && collect(p).size() == 1 && std::string_view(collect(p)[0].source) == "Frostbitten");
    Player hitter;
    hitter.stats.strength = 0;
    CHECK(resolveHit(hitter, p, {10, 10, 1}, {rng}) == 12 && resolveHit(hitter, Player{}, {10, 10, 1}, {rng}) == 10);
    for (int i = 0; i < 10 * 60; ++i) updateCombat(p, none, dt, rng);
    CHECK(afflict(p, Status::Frost, "Ice") == 13 && p.effects.size() == 1 && p.effects[0].seconds == gStatus.frostSeconds);
    Player frosty;
    frosty.innate = {{Stat::Frost, ModKind::Flat, 30}};
    buildUp(frosty, p, 1, "Frosty");
    CHECK(p.buildup[2] == 0 && p.effects[0].source == "Ice");  // locked out while it's on
    for (int i = 0; i < 15 * 60 + 1; ++i) updateCombat(p, none, dt, rng);
    CHECK(p.effects.empty() && sheet(p)[Stat::DamageTaken] == 1);
    // Poison kills, with the death's noise; nothing takes on a dead body.
    afflict(p, Status::Poison, "Venom");
    p.health = 1;
    for (int i = 0; i < 2 * 60 && p.alive(); ++i) p.noises = 0, updateCombat(p, none, dt, rng);
    CHECK(!p.alive() && p.made(Noise::Death) && p.effects.empty() && afflict(p, Status::Frost, "Ice") == 0 && p.effects.empty());
    p = Player{};
    // Death ends them all.
    afflict(p, Status::Poison, "Venom"), p.buildup[0] = 50;
    CHECK(hurt(p, 99) && p.alive() && p.effects.size() == 1 && hurt(p, 5) && !p.alive() && p.effects.empty() && p.buildup[0] == 0);

    // A kill by poison is the player's: the creature's XP and a Killed event.
    {
        Player hero;
        hero.pos = {7.5f, 4, 7.5f};
        std::vector<Entity> es;
        Entity& boar = spawnAhead(es, g, hero, EntityKind::Boar, 3);
        afflict(boar.body, Status::Poison, "Rusty Knife");
        boar.body.health = 2;
        std::vector<GameEvent> events;
        for (int i = 0; i < 3 * 60 && boar.body.alive(); ++i) updateCombat(hero, es, dt, rng, kArmorK, &events);
        CHECK(!boar.body.alive() && boar.body.made(Noise::Death) && events.size() == 1 && hero.progress.xp == boar.type().xp);
        CHECK(std::get<Killed>(events[0]).kind == EntityKind::Boar && boar.body.effects.empty());
        // Poison's ticks don't call back a fighter that gave up: a wolf going home stays on its way.
        Entity& wolf = spawnAhead(es, g, hero, EntityKind::Wolf, 4, 30);
        setState(wolf, AiState::Return, "test");
        afflict(wolf.body, Status::Poison, "Rusty Knife");
        for (int i = 0; i < 3 * 60; ++i) updateCombat(hero, es, dt, rng);
        CHECK(wolf.body.health < float(maxHealth(sheet(wolf.body))) && !(wolf.events & EventDamaged));
    }

    // In fights: the player's knife on the dummy bleeds it every few hits, the bleed's number beside the
    // hit's; a wolf's bites bleed the player.
    {
        Player hero;
        hero.pos = {7.5f, 4, 7.5f};
        hero.onGround = true, hero.offGround = 0;
        hero.inv.equipped[SlotMainHand] = Knife;
        std::vector<Entity> es;
        Entity& d = spawnAhead(es, g, hero, EntityKind::TrainingDummy, 1, 1.5f);
        d.body.yaw = std::numbers::pi_v<float>;  // facing: no backstabs
        int hits = 0, bleeds = 0, firstBleed = 0;
        for (int i = 0; i < 60 * 60 && hits < 40; ++i) {
            d.body.lastDamage = 0;  // (immortal: the hit's number tells, ADR-056)
            d.body.stance = d.type().stance;
            Input still;
            updateMelee(hero, {.attack = !hero.punching()}, still, dt);
            updateCombat(hero, es, dt, rng);
            if (d.body.lastDamage > 0) {
                ++hits;
                if (d.body.made(Noise::Bleed)) {
                    ++bleeds;
                    if (!firstBleed) firstBleed = hits;
                    auto fresh = [&](const DamageNumber& n) { return n.age <= dt; };  // (its clock ran once since)
                    CHECK(std::ranges::count_if(d.body.numbers, fresh) == 2);
                    CHECK(std::ranges::any_of(d.body.numbers, [&](const DamageNumber& n) { return fresh(n) && n.kind == Status::Bleed && n.amount == 15; }));
                }
            }
            d.body.health = float(maxHealth(sheet(d.body)));
            d.body.noises = 0;
        }
        std::println("status: {} bleeds in {} knife hits on the dummy, the first on hit {}", bleeds, hits, firstBleed);
        CHECK(hits == 40 && firstBleed == 4 && bleeds >= 8 && bleeds <= 10);
        // A guard behind a buckler (slash negation 0.85) lets 15% of the knife's build-up through.
        es.clear();
        Entity& guard = spawnAhead(es, g, hero, EntityKind::Guard, 2, 1.5f);
        guard.body.yaw = std::numbers::pi_v<float>;
        guard.body.inv.equipped[SlotOffHand] = defs().itemById.at("oak_buckler");
        int blocked = 0;
        for (int i = 0; i < 60 * 60 && blocked < 5; ++i) {
            guard.body.guarding = true, guard.body.stamina = gMelee.staminaMax, guard.body.buildup = {};
            Input still;
            updateMelee(hero, {.attack = !hero.punching()}, still, dt);
            updateCombat(hero, es, dt, rng);
            if (guard.body.made(Noise::Block) && guard.body.health < float(maxHealth(sheet(guard.body)))) {
                ++blocked;
                CHECK(std::abs(guard.body.buildup[0] - (30 * 0.15f - gStatus.decay * dt)) < 0.01f);  // (its clock ran once since)
            }
            guard.body.health = float(maxHealth(sheet(guard.body)));
            guard.body.noises = 0, guard.body.stagger = 0;
        }
        CHECK(blocked == 5);
        // A hit so light the guard stops all its damage still builds by that share.
        hero.innate = {{Stat::Damage, ModKind::More, -0.95f}};
        int light = 0;
        for (int i = 0; i < 60 * 60 && light < 3; ++i) {
            guard.body.guarding = true, guard.body.stamina = gMelee.staminaMax, guard.body.buildup = {};
            Input still;
            updateMelee(hero, {.attack = !hero.punching()}, still, dt);
            updateCombat(hero, es, dt, rng);
            if (guard.body.made(Noise::Block)) {
                ++light;
                CHECK(guard.body.health == float(maxHealth(sheet(guard.body))));
                CHECK(std::abs(guard.body.buildup[0] - (30 * 0.15f - gStatus.decay * dt)) < 0.01f);
            }
            guard.body.noises = 0, guard.body.stagger = 0;
        }
        CHECK(light == 3);
    }
    {
        Player hero;
        hero.pos = {7.5f, 4, 7.5f};
        hero.onGround = true, hero.offGround = 0;
        std::vector<Entity> es;
        Entity& w = spawnAhead(es, g, hero, EntityKind::Wolf, 1, 1.5f);
        w.body.onGround = true;
        int bites = 0, bleeds = 0;
        for (int i = 0; i < 60 * 60 && bites < 30; ++i) {
            if (!w.body.punching() && w.body.swingTimer <= 0) startPunch(w.body, {0, -1});
            float before = hero.health;
            Input still;
            updateMelee(hero, {}, still, dt);
            updateCombat(hero, es, dt, rng);
            if (hero.health < before) ++bites, bleeds += hero.made(Noise::Bleed);
            hero.health = float(maxHealth(sheet(hero)));
            hero.noises = 0, hero.stagger = 0;
        }
        std::println("status: {} bleeds in {} wolf bites on the player", bleeds, bites);
        CHECK(bites == 30 && bleeds >= 4 && bleeds <= 7);
        // Behind a buckler (pierce negation 0.8) a bite builds a fifth; a perfect guard none.
        hero.inv.equipped[SlotOffHand] = defs().itemById.at("oak_buckler");
        int blocked = 0;
        for (int i = 0; i < 60 * 60 && blocked < 5; ++i) {
            if (!w.body.punching() && w.body.swingTimer <= 0) startPunch(w.body, {0, -1});
            hero.buildup = {};
            Input still;
            updateMelee(hero, {.guard = true}, still, dt);
            updateCombat(hero, es, dt, rng);
            if (hero.made(Noise::Block)) {
                ++blocked;
                float want = hero.made(Noise::PerfectGuard) ? 0 : 22 * (1 - 0.8f);
                CHECK(std::abs(hero.buildup[0] - want) < 0.01f);
            }
            hero.health = float(maxHealth(sheet(hero)));
            hero.noises = 0, hero.stagger = 0, hero.stamina = gMelee.staminaMax;
        }
        CHECK(blocked == 5);
        // A bite so light the buckler stops all its damage still builds a fifth.
        w.body.innate.push_back({Stat::Damage, ModKind::More, -0.95f});
        int light = 0;
        for (int i = 0; i < 60 * 60 && light < 3; ++i) {
            if (!w.body.punching() && w.body.swingTimer <= 0) startPunch(w.body, {0, -1});
            hero.buildup = {};
            Input still;
            updateMelee(hero, {.guard = true}, still, dt);
            updateCombat(hero, es, dt, rng);
            if (hero.made(Noise::Block) && !hero.made(Noise::PerfectGuard)) {
                ++light;
                CHECK(hero.health == float(maxHealth(sheet(hero))) && std::abs(hero.buildup[0] - 22 * 0.2f) < 0.01f);
            }
            hero.noises = 0, hero.stagger = 0, hero.stamina = gMelee.staminaMax;
        }
        CHECK(light == 3);
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
    const ItemId buckler = defs().itemById.at("oak_buckler"), tower = defs().itemById.at("tower_shield"), bow = defs().itemById.at("short_bow");
    const ItemId bomb = defs().itemById.at("clay_bomb"), pot = defs().itemById.at("poison_pot"), throwingKnife = defs().itemById.at("throwing_knife");
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
    {  // a great weapon's light whooshes as a heavy does, deeper, without the effort's voice (ADR-049)
        ItemId gs = defs().itemById.at("iron_greatsword");
        auto layers = layersOf({.what = Noise::Swing, .move = Move::Light1, .with = gs});
        CHECK(names({.what = Noise::Swing, .move = Move::Light1, .with = gs}) == Names{"swing_heavy"} && layers.size() == 1 && layers[0].pitch < 0.9f);
    }
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
                for (Move move : {Move::Light1, Move::Heavy1, Move::Parry, Move::Drink, Move::Shot, Move::Throw})
                    for (ItemId with : {NoItem, Shortsword, Knife, buckler, tower, bow, bomb, pot, throwingKnife})
                        for (float amount : {0.0f, 30.0f})
                            for (const Layer& l : layersOf({Noise(n), {}, kind, ground, move, with, amount}))
                                if (!(kind < 0 && Noise(n) == Noise::Windup && move != Move::Shot))  // only NPCs telegraph (the player draws a bow)
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

// ADR-050: bows and projectiles. The bow class (ranged: only the shot), its data checks, the draw (at
// once, held, let go), the arrow's flight (gravity, range by draw, swept through thin walls), what it
// hits (a body through strike(), a guard, a dodge's i-frames let it through), sticking and picking up,
// the cap, aiming (straight at the crosshair, lobbed at a lock-on), determinism, the pose, the sounds.
static void bows() {
    namespace fs = std::filesystem;
    const Defs& d = defs();
    const ItemId bow = d.itemById.at("short_bow");
    const MoveSpec& shot = spec(WeaponClass::Bow, Move::Shot);
    CHECK(classOf(bow) == WeaponClass::Bow && itemType(bow).damage == DamageType::Pierce && itemType(bow).slot == SlotMainHand);
    CHECK(heldModel(bow) == ModelBow && weaponClass(WeaponClass::Bow).ranged && weaponClass(WeaponClass::Bow).twoHanded);
    for (size_t c = 0; c < size_t(WeaponClass::Count); ++c) {  // a ranged class has only the shot (and a crossbow the reload, ADR-052), the rest every move but them
        const WeaponClassDef& w = weaponClass(WeaponClass(c));
        CHECK(w.ranged == (WeaponClass(c) == WeaponClass::Bow || WeaponClass(c) == WeaponClass::Crossbow) && w.loads == (WeaponClass(c) == WeaponClass::Crossbow));
        for (size_t m = 1; m < size_t(Move::Count); ++m)
            if (!codeMove(Move(m))) CHECK((w.moves[m].seconds > 0) == (Move(m) == Move::Shot ? w.ranged : Move(m) == Move::Reload ? w.loads : !w.ranged));
    }
    CHECK(shot.holdAt < shot.impact && shot.lunge == 0 && shot.thrust);
    CHECK(typeOf(EntityKind::Guard).loot == d.itemById.at("light_crossbow") && typeOf(EntityKind::Guard).lootChance == 0.5f && Player{}.arrows == kQuiver);  // (ADR-052: the guard's crossbow; archers drop the bow)
    for (Model m : {ModelBow, ModelBowDrawn, ModelArrow}) {  // voxels in the Head slot only
        int n = 0;
        Grid g = buildPart(m, Head);
        for (int z = 0; z < g.size.z; ++z)
            for (int y = 0; y < g.size.y; ++y)
                for (int x = 0; x < g.size.x; ++x) n += g.get({x, y, z}) != Air;
        CHECK(n > 8 && buildPart(m, Torso).get({0, 0, 0}) == Air);
    }
    {  // a creature's bow (ADR-051): it keeps a range band; the band, aim and sidearm go only with a weapon that shoots
        fs::path dir = fs::temp_directory_path() / "flycoos_bows_test";
        fs::create_directories(dir);
        copyWeaponClasses(dir);
        fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
        std::ostringstream text;
        text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
        std::string creatures = text.str();
        creatures.erase(creatures.rfind(']'));
        std::string error;
        for (auto [extra, want] : {
                 std::pair{R"("copy_from": "bandit", "weapon": "short_bow", "parry": null)", "record 10 'archer': field 'keep_range': missing"},
                 {R"("copy_from": "bandit", "keep_range": [7, 16])", "field 'keep_range': only for a creature whose 'weapon' shoots"},
                 {R"("copy_from": "bandit", "aim_error": 0.1)", "field 'aim_error': only for a creature whose 'weapon' shoots"},
                 {R"("copy_from": "bandit", "sidearm": "rusty_knife")", "field 'sidearm': only for a creature whose 'weapon' shoots"},
                 {R"("copy_from": "bandit_archer", "keep_range": [7])", "field 'keep_range': must be [min, max] blocks"},
                 {R"("copy_from": "bandit_archer", "keep_range": [7, 16, 20])", "field 'keep_range': must be [min, max] blocks"},
                 {R"("copy_from": "bandit_archer", "keep_range": 7)", "field 'keep_range': must be [min, max] blocks"},
                 {R"("copy_from": "bandit_archer", "keep_range": [9, 8])", "field 'keep_range': must be 2 <= min < max <= 40"},
                 {R"("copy_from": "bandit_archer", "keep_range": [1, 8])", "field 'keep_range': must be 2 <= min < max <= 40"},
                 {R"("copy_from": "bandit_archer", "keep_range": [8, 41])", "field 'keep_range': must be 2 <= min < max <= 40"},
                 {R"("copy_from": "bandit_archer", "aim_error": 1)", "field 'aim_error': 1 is out of range 0..0.5"},
                 {R"("copy_from": "bandit_archer", "sidearm": "short_bow")", "field 'sidearm': 'short_bow' is not a main_hand weapon that swings"},
                 {R"("copy_from": "bandit_archer", "sidearm": "iron_helm")", "field 'sidearm': 'iron_helm' is not a main_hand weapon that swings"},
                 {R"("copy_from": "bandit_archer", "dodge": null, "guard": 0.5)", "field 'guard': a bow doesn't guard or parry"},
                 {R"("copy_from": "bandit_archer", "shield": "heater_shield")", "field 'shield': a bow is held in both hands"},
                 {R"("copy_from": "bandit_archer", "weapon": "light_crossbow")", "field 'weapon': creatures can't reload a crossbow yet"},
                 {R"("copy_from": "bandit_archer", "keep_range": [3, 5], "aim_error": 0, "sidearm": null)", ""}}) {
            std::ofstream(dir / "creatures.json", std::ios::trunc) << creatures << R"(,{"id": "archer", "save_id": 10, )" << extra << "}]";
            Defs t;
            error.clear();
            bool loaded = loadDefs(dir, t, error);
            CHECK(*want ? !loaded && error.contains(want) : loaded && t.creatures[10].keepMin == 3 && t.creatures[10].keepMax == 5 && !t.creatures[10].sidearm);
            if (*want ? !error.contains(want) : !loaded) std::println(stderr, "FAIL archer data '{}': {}", extra, error);
        }
        // An NPC that always parries guards a drawn bow instead: an arrow can't be parried (a sword's swing it parries).
        std::ofstream(dir / "creatures.json", std::ios::trunc) << creatures << R"(,{"id": "fencer", "save_id": 10, "copy_from": "bandit", "parry": 1, "dodge": null, "back_off": null}])";
        Defs test, shipped;
        CHECK(loadDefs(dir, test, error) && loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
        fs::remove_all(dir);
        setDefs(std::move(test));
        for (int mode : {0, 1, 2}) {  // a sword's swing, a bow's draw, a throw (ADR-053: a throwable can't be parried either)
            bool shooting = mode == 1;
            Session s{.world = Grid({32, 16, 32}), .seed = 5};
            for (int z = 0; z < 32; ++z)
                for (int x = 0; x < 32; ++x)
                    for (int y = 0; y < 4; ++y) s.world.set({x, y, z}, Stone);
            startSession(s);
            s.player.teleport({8.5f, 4, 16.5f});
            s.player.yaw = std::numbers::pi_v<float> / 2;
            s.player.inv.equipped[SlotMainHand] = shooting ? defs().itemById.at("short_bow") : Shortsword;
            Entity& fencer = spawnAhead(s.entities, s.world, s.player, EntityKind(10), 3, 2.5f);
            setState(fencer, AiState::Chase, "test");
            fencer.body.swingTimer = 5;  // not about to swing itself
            if (mode == 2) {  // (on the ground first: a throw doesn't start in the air)
                for (int i = 0; i < 10; ++i) step(s, {}, kTick);
                step(s, {.toss = true}, kTick);
            }
            else step(s, {.attack = true, .attackHeld = shooting}, kTick);  // a sword's tap is a light at once
            CHECK(s.player.punching() && fencer.picked && fencer.answer == (mode ? Answer::Guard : Answer::Parry));
        }
        setDefs(std::move(shipped));
    }

    // A field to shoot across: ground top at y 4, the player at x 8.5 facing +x with the bow.
    auto field = [&](bool wall = false) {
        Session s{.world = Grid({128, 32, 32}), .seed = 5};
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 128; ++x) {
                for (int y = 0; y < 4; ++y) s.world.set({x, y, z}, Stone);
                if (wall && x == 20)
                    for (int y = 4; y < 24; ++y) s.world.set({x, y, z}, Stone);
            }
        startSession(s);
        s.player.teleport({8.5f, 4, 16.5f});
        s.player.yaw = std::numbers::pi_v<float> / 2;
        s.player.inv.equipped[SlotMainHand] = bow;
        for (int i = 0; i < 10; ++i) step(s, {}, kTick);
        s.sounds.clear();
        return s;
    };
    auto heard = [](const Session& s, Noise n) { return std::ranges::any_of(s.sounds, [&](const Sound& h) { return h.what == n; }); };
    // Shoots until an arrow is loosed (drawing `hold` seconds first), then lets it fly until it stops or `ticks` pass.
    auto shoot = [&](Session& s, float hold, SimInput in = {}, int ticks = 0) {
        SimInput press = in;
        press.attack = press.attackHeld = true;
        step(s, press, kTick);
        press.attack = false;
        for (float t = kTick; t < hold; t += kTick) step(s, press, kTick);
        size_t before = s.projectiles.size();
        for (int i = 0; i < 60 && s.projectiles.size() == before; ++i) step(s, in, kTick);
        for (int i = 0; i < ticks; ++i) step(s, in, kTick);
    };

    {  // The draw: at once, held at full draw as long as the button is down, loosed on letting go.
        Session s = field();
        Player& p = s.player;
        step(s, {.attack = true, .attackHeld = true}, kTick);
        CHECK(p.move == Move::Shot && p.holding && p.hitPending);  // no tap-or-hold wait
        CHECK(std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Windup && h.kind < 0 && h.move == Move::Shot; }));
        for (int i = 0; i < 180; ++i) step(s, {.attackHeld = true}, kTick);
        CHECK(p.move == Move::Shot && p.charged && p.hitPending && drawOf(p) == 1 && s.projectiles.empty() && p.arrows == kQuiver);
        CHECK(heard(s, Noise::Charged) && p.moveTime == holdTime(p));
        s.sounds.clear();
        for (int i = 0; i < 30 && s.projectiles.empty(); ++i) step(s, {}, kTick);
        CHECK(s.projectiles.size() == 1 && p.arrows == kQuiver - 1 && heard(s, Noise::Loose) && !p.hitPending);
        const Projectile& a = s.projectiles[0];
        CHECK(a.weapon == bow && a.power == shot.damage && a.stance == shot.stance && !a.stuck);
        CHECK(std::abs(glm::length(a.vel) - gRanged.maxSpeed) < 0.3f && a.vel.x > 0.99f * glm::length(a.vel));  // along the facing
        CHECK(p.stamina < gMelee.staminaMax);  // it cost stamina
        // A tap: no draw, the slowest and weakest shot.
        Session t = field();
        step(t, {.attack = true}, kTick);
        for (int i = 0; i < 60 && t.projectiles.empty(); ++i) step(t, {}, kTick);
        CHECK(t.projectiles.size() == 1 && std::abs(t.projectiles[0].power - gRanged.minPower * shot.damage) < 1e-5f);
        CHECK(std::abs(glm::length(t.projectiles[0].vel) - gRanged.minSpeed) < 0.3f);
        // Range by draw: the full draw lands far past the tap, about where the arc says (from 1.35 up, hipPitch).
        for (int i = 0; i < 300; ++i) step(s, {}, kTick), step(t, {}, kTick);
        CHECK(s.projectiles.size() == 1 && s.projectiles[0].stuck && t.projectiles.size() == 1 && t.projectiles[0].stuck);
        float v = gRanged.maxSpeed, vy = v * std::sin(gRanged.hipPitch), g = gRanged.gravity;
        float far = 9 + v * std::cos(gRanged.hipPitch) * (vy + std::sqrt(vy * vy + 2 * g * 1.35f)) / g;
        CHECK(std::abs(s.projectiles[0].pos.x - far) < 1.5f && t.projectiles[0].pos.x < far - 25 && std::abs(s.projectiles[0].pos.y - 4) < 0.01f);
        // No arrows, no shot; a roll lets a draw down (the arrow kept).
        p.arrows = 0;
        step(s, {.attack = true, .attackHeld = true}, kTick);
        CHECK(p.move == Move::None && p.want == Want::None);  // dropped, not waiting for an arrow
        p.arrows = 5;
        for (int i = 0; i < 60; ++i) step(s, {}, kTick);
        step(s, {.attack = true, .attackHeld = true}, kTick);
        CHECK(p.move == Move::Shot);
        SimInput roll{.attackHeld = true};
        roll.move.roll = true, roll.move.move = {0, 1};
        step(s, roll, kTick);
        for (int i = 0; i < 60; ++i) step(s, {.attackHeld = true}, kTick);
        CHECK(p.move == Move::None && p.arrows == 5 && s.projectiles.size() == 1);
        // Flinched on the very tick it looses: the arrow still flies with the shot's damage.
        step(s, {.attack = true, .attackHeld = true}, kTick);
        for (int i = 0; i < 60; ++i) step(s, {.attackHeld = true}, kTick);
        interrupt(p, gMelee.flinchSeconds, false);
        loose(s, std::nullopt);
        CHECK(p.move == Move::None && s.projectiles.back().power == shot.damage && s.projectiles.back().stance == shot.stance);
        // Swapping weapons mid-draw: the shot finishes with the bow; knocked out mid-draw, nothing is loosed and the aim lets go.
        for (int i = 0; i < 60; ++i) step(s, {}, kTick);
        int left = p.arrows;
        step(s, {.attack = true, .attackHeld = true}, kTick);
        p.inv.equipped[SlotMainHand] = Shortsword;
        for (int i = 0; i < 60; ++i) step(s, {}, kTick);
        CHECK(p.arrows == left - 1 && s.projectiles.back().weapon == bow && s.projectiles.back().age < 1 && p.move == Move::None);
        p.inv.equipped[SlotMainHand] = bow;
        step(s, {.attack = true, .attackHeld = true, .guard = true}, kTick);
        CHECK(p.move == Move::Shot && p.aiming);
        p.health = 0;
        for (int i = 0; i < 60; ++i) step(s, {.guard = true}, kTick);
        CHECK(p.arrows == left - 1 && s.projectiles.back().age > 0.9f && !p.aiming && p.aimBlend == 0);  // the latest is still the swap's
    }

    {  // Aim mode: the guard button aims (no guard, no parry), walking slowly; the shot goes straight at the point.
        Session s = field(true);
        Player& p = s.player;
        SimInput aim{.guard = true, .aim = {1, 0}};
        step(s, aim, kTick);
        CHECK(p.aiming && !p.guarding && parryOf(p) == 0);
        SimInput walk = aim;
        walk.move.move = {0, 1};
        for (int i = 0; i < 40; ++i) step(s, walk, kTick);
        CHECK(std::abs(glm::length(glm::vec2(p.vel.x, p.vel.z)) - gRanged.aimWalk) < 0.2f && p.aimBlend > 0.99f);
        glm::vec3 target(20, 9, p.pos.z);  // on the wall, above the line of the facing
        aim.aimAt = target;
        shoot(s, 1, aim, 60);
        CHECK(s.projectiles.size() == 1 && s.projectiles[0].stuck && glm::distance(s.projectiles[0].pos, target) < 0.6f);
        CHECK(std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Thunk && h.ground == Stone; }));
        step(s, {}, kTick);
        CHECK(!p.aiming);
    }

    {  // Flight: gravity on a semi-implicit step; swept, so even 10 blocks a tick doesn't pass a 1-block wall.
        Session s = field(true);
        s.projectiles.push_back({{2.5f, 20, 16.5f}, {}, {15, 0, 0}, bow, 1, 0});
        for (int i = 0; i < 30; ++i) updateProjectiles(s, kTick);
        const Projectile& a = s.projectiles[0];
        CHECK(std::abs(a.pos.x - 10) < 1e-3f && std::abs(a.pos.y - (20 - gRanged.gravity * kTick * kTick * 465)) < 1e-3f && !a.stuck);
        for (float speed : {55.0f, 200.0f, 600.0f}) {
            s.projectiles = {{{10.5f, 6, 16.5f}, {}, {speed, 0, 0}, bow, 1, 0}};
            for (int i = 0; i < 12; ++i) updateProjectiles(s, kTick);
            CHECK(s.projectiles.size() == 1 && s.projectiles[0].stuck && std::abs(s.projectiles[0].pos.x - 20) < 0.01f);
        }
        // Stuck: picked up walking past (a quiver not yet full), else gone after stuckSeconds.
        Player& p = s.player;
        p.arrows = 10;
        s.projectiles = {{p.pos + glm::vec3(0.6f, 0.4f, 0), {}, {1, 0, 0}, bow, 1, 0, 0, true}, {p.pos + glm::vec3(3, 0.4f, 0), {}, {1, 0, 0}, bow, 1, 0, 0, true}};
        updateProjectiles(s, kTick);
        CHECK(p.arrows == 11 && s.projectiles.size() == 1 && p.made(Noise::Loot));
        p.arrows = kQuiver;
        s.projectiles[0].pos = p.pos + glm::vec3(0.6f, 0.4f, 0);
        updateProjectiles(s, kTick);
        CHECK(p.arrows == kQuiver && s.projectiles.size() == 1);  // full: left there
        s.projectiles[0].pos.x += 5;
        for (float t = 0; t < gRanged.stuckSeconds - 0.1f; t += kTick) updateProjectiles(s, kTick);
        CHECK(s.projectiles.size() == 1);
        for (int i = 0; i < 12; ++i) updateProjectiles(s, kTick);
        CHECK(s.projectiles.empty());
        p.arrows = 2;
        respawnPlayer(s);
        CHECK(p.arrows == kQuiver);  // getting back up fills the quiver
        // A flying arrow lasts flightSeconds.
        s.projectiles = {{{2.5f, 25, 16.5f}, {}, {0, 40, 0}, bow, 1, 0}};
        for (float t = 0; t < gRanged.flightSeconds - 0.1f; t += kTick) updateProjectiles(s, kTick);
        CHECK(s.projectiles.size() == 1);
        for (int i = 0; i < 12; ++i) updateProjectiles(s, kTick);
        CHECK(s.projectiles.empty());
        // The cap: the oldest stuck arrow goes, else the oldest.
        Projectile flying{{50, 30, 2}, {}, {0, 0, 0}, bow, 1, 0};
        s.projectiles.assign(kMaxProjectiles, flying);
        s.projectiles[0].age = 7, s.projectiles[10].stuck = true;
        loose(s, std::nullopt);
        CHECK(s.projectiles.size() == kMaxProjectiles && std::ranges::none_of(s.projectiles, &Projectile::stuck) && s.projectiles[0].age == 7);
        loose(s, std::nullopt);
        CHECK(s.projectiles.size() == kMaxProjectiles && s.projectiles[0].age == 0);
        p.arrows = 0;
        loose(s, std::nullopt);
        CHECK(s.projectiles.size() == kMaxProjectiles && p.arrows == 0);  // none to loose
    }

    {  // Hits: through strike() with the bow's roll; a guard in front blocks it; a dodge's i-frames let it through.
        Session s = field();
        Player& p = s.player;
        Entity& dummy = spawnAhead(s.entities, s.world, p, EntityKind::TrainingDummy, 4, 12);
        p.swingWeapon = Shortsword;
        shoot(s, 1, {}, 20);
        CHECK(dummy.body.lastDamage > 0 && dummy.body.hitWith == bow && s.projectiles.empty() && p.swingWeapon == bow);  // (immortal, ADR-056)
        CHECK(p.progress.skills[size_t(Skill::OneHanded)].xp > 0);  // XP as a hit (one-handed: no new use skills)
        // Determinism: the same shots again give the same everything.
        Session again = field();
        Entity& twin = spawnAhead(again.entities, again.world, again.player, EntityKind::TrainingDummy, 4, 12);
        again.player.swingWeapon = Shortsword;
        shoot(again, 1, {}, 20);
        CHECK(twin.body.lastDamage == dummy.body.lastDamage && twin.body.lastCrit == dummy.body.lastCrit && again.player.arrows == p.arrows);
        // A kill: Killed, as a swing's (on a boar: the dummy can't die, ADR-056).
        s.entities.clear();
        Entity& boar = spawnAhead(s.entities, s.world, p, EntityKind::Boar, 4, 12);
        boar.body.health = 1;
        s.events.clear();
        s.projectiles = {{boar.body.pos + glm::vec3(-3, 1, 0), {}, {40, 0, 0}, bow, 1, 0}};
        for (int i = 0; i < 10; ++i) updateProjectiles(s, kTick);
        CHECK(!boar.body.alive() && std::ranges::any_of(s.events, [](const GameEvent& e) { return std::holds_alternative<Killed>(e); }));
        // Two in line: the nearer takes it (whichever comes first in the list). The player's own swing weapon
        // is its own again after; an arrow may be a random critical.
        s.entities.clear();
        spawnAhead(s.entities, s.world, p, EntityKind::TrainingDummy, 4, 8);
        spawnAhead(s.entities, s.world, p, EntityKind::TrainingDummy, 4, 12);
        Entity &nearer = s.entities[0], &farther = s.entities[1];  // (after both: a spawn may move the vector)
        p.swingWeapon = Shortsword;
        p.innate.push_back({Stat::CritChance, ModKind::Flat, 1, "test"});
        s.projectiles = {{p.pos + glm::vec3(1, 1, 0), {}, {900, 0, 0}, bow, 1, 0}};  // both on its first tick's way
        updateProjectiles(s, kTick);
        CHECK(nearer.body.hitTime < 1 && farther.body.hitTime > 1 && p.swingWeapon == Shortsword && nearer.body.hitWith == bow);
        CHECK(nearer.body.lastCrit && nearer.body.made(Noise::Crit));
        p.innate.clear();

        s.entities.clear();
        Entity& guard = spawnAhead(s.entities, s.world, p, EntityKind::Guard, 4, 10);
        guard.body.guarding = true;
        float health = guard.body.health;
        s.projectiles = {{guard.body.pos + glm::vec3(-4, 1.2f, 0), {}, {40, 0, 0}, bow, 1, 5}};
        for (int i = 0; i < 10; ++i) updateProjectiles(s, kTick);
        CHECK(guard.body.health == health && guard.body.made(Noise::Block) && guard.body.stamina < gMelee.staminaMax && s.projectiles.empty());
        CHECK(guard.body.freeze == 0 && p.freeze == 0 && p.stagger == 0);  // an arrow: no hit-stop, no recoil
        guard.body.inv.equipped[SlotOffHand] = d.itemById.at("tower_shield");  // a greatshield: a swing this weak would bounce off
        s.projectiles = {{guard.body.pos + glm::vec3(-4, 1.2f, 0), {}, {40, 0, 0}, bow, 1, 5}};
        for (int i = 0; i < 10; ++i) updateProjectiles(s, kTick);
        CHECK(guard.body.health == health && p.stagger == 0 && !guard.body.made(Noise::Bounce));  // an arrow doesn't
        s.projectiles = {{guard.body.pos + glm::vec3(4, 1.2f, 0), {}, {-40, 0, 0}, bow, 1, 5}};  // from behind
        for (int i = 0; i < 10; ++i) updateProjectiles(s, kTick);
        CHECK(guard.body.health < health);

        guard.body.guarding = false;
        guard.body.rollTime = 0.1f, guard.body.rollLength = 0.45f, guard.body.backstep = false;  // rolling, in its i-frames
        health = guard.body.health;
        s.projectiles = {{guard.body.pos + glm::vec3(-2, 1.2f, 0), {}, {30, 0, 0}, bow, 1, 5}};
        for (int i = 0; i < 10; ++i) updateProjectiles(s, kTick);
        CHECK(guard.body.health == health && s.projectiles.size() == 1 && s.projectiles[0].pos.x > guard.body.pos.x + 1);
    }

    {  // Level band: full draws to kill a level 1 boar and a level 2 bandit with the starting player, and how long that takes.
        float cycle = holdTime(Player{}) + gRanged.drawSeconds;  // (at the bow's speed 1) the windup, the draw...
        cycle += (shot.chain - shot.holdAt) * shot.seconds;      // ...the release and the recovery to the next draw
        for (auto [kind, lo, hi] : {std::tuple{EntityKind::Boar, 8, 12}, std::tuple{EntityKind::Bandit, 10, 16}}) {
            Session z = field();
            spawnAhead(z.entities, z.world, z.player, kind, 4, 20);
            const Entity& e = z.entities[0];
            z.player.swingWeapon = bow;
            std::mt19937 rng(11);
            double sum = 0;
            for (int i = 0; i < 400; ++i)
                sum += resolveHit(z.player, e.body, weaponAttack(bow), {rng, z.armorK, shot.damage * (1 - e.type().resist[size_t(DamageType::Pierce)])});
            int shots = int(std::ceil(e.body.health / (sum / 400)));
            std::println("level band: bow full draws: level {} {}: {} ({:.1f} s)", e.type().level, kEntityKindNames[int(kind)], shots, shots * cycle);
            CHECK(shots >= lo && shots <= hi);
        }
    }

    {  // No riposte or backstab with a bow: a broken foe in reach doesn't pull the shot toward it.
        Session s = field();
        Entity& bandit = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 4, 1.5f);
        bandit.body.teleport(s.player.pos + glm::vec3(1.2f, 0, 0.6f));
        bandit.body.broken = true, bandit.body.stagger = bandit.body.staggerLength = 3;
        step(s, {.attack = true, .aim = {1, 0}}, kTick);
        CHECK(s.player.move == Move::Shot && std::abs(s.player.yaw - std::numbers::pi_v<float> / 2) < 0.01f);
    }

    {  // Locked on: lobbed onto the target, from a tap as from a full draw.
        for (float hold : {0.0f, 1.0f}) {
            Session s = field();
            Entity& dummy = spawnAhead(s.entities, s.world, s.player, EntityKind::TrainingDummy, 4, 30);
            SimInput in{.aim = {1, 0}, .aimAt = dummy.body.pos + glm::vec3(0, 1, 0)};
            shoot(s, hold, in, 120);
            CHECK(dummy.body.lastDamage > 0 && s.projectiles.empty());
        }
    }

    {  // The aim's maths: straight, lobbed (it passes through the point), out of reach (45 degrees), none (the facing).
        glm::vec3 from(0, 5, 0), at(30, 8, 4);
        glm::vec3 v = shotVelocity(from, 0, at, false, 40);
        CHECK(glm::length(glm::normalize(v) - glm::normalize(at - from)) < 1e-5f && std::abs(glm::length(v) - 40) < 1e-3f);
        v = shotVelocity(from, 0, at, true, 40);
        CHECK(std::abs(glm::length(v) - 40) < 1e-3f && v.y > 0);
        glm::vec3 x = from;
        float closest = 99;
        for (int i = 0; i < 200; ++i) v.y -= gRanged.gravity * kTick, x += v * kTick, closest = std::min(closest, glm::distance(x, at));
        CHECK(closest < 0.4f);
        float gravity = std::exchange(gRanged.gravity, 0.0f);  // no gravity (a debug knob): nothing to lob over, straight at it
        v = shotVelocity(from, 0, at, true, 40);
        CHECK(glm::length(glm::normalize(v) - glm::normalize(at - from)) < 1e-5f);
        gRanged.gravity = gravity;
        v = shotVelocity(from, 0, glm::vec3(100, 5, 0), true, 5);
        CHECK(std::abs(v.y - 5 * std::sin(std::numbers::pi_v<float> / 4)) < 1e-4f);
        v = shotVelocity(from, std::numbers::pi_v<float> / 2, std::nullopt, true, 20);
        CHECK(std::abs(v.x - 20 * std::cos(gRanged.hipPitch)) < 1e-4f && std::abs(v.y - 20 * std::sin(gRanged.hipPitch)) < 1e-4f && std::abs(v.z) < 1e-4f);
        CHECK(segmentBox({0, 0.5f, 0}, {10, 0.5f, 0}, {4, 0, -0.5f}, {5, 1, 0.5f}) == 0.4f);
        CHECK(!segmentBox({0, 1.5f, 0}, {10, 1.5f, 0}, {4, 0, -0.5f}, {5, 1, 0.5f}) && !segmentBox({0, 0.5f, 0}, {3, 0.5f, 0}, {4, 0, -0.5f}, {5, 1, 0.5f}));
        CHECK(segmentBox({4.5f, 0.5f, 0}, {10, 0.5f, 0}, {4, 0, -0.5f}, {5, 1, 0.5f}) == 0.0f);
        CHECK(!segmentBox({0, 0, 0}, {10, 10, 0}, {4, 0, -0.5f}, {5, 1, 0.5f}));  // past its corner: in x's slab only after leaving y's
        {
            Session z = field();
            spawnAhead(z.entities, z.world, z.player, EntityKind::Boar, 4, 6);
            spawnAhead(z.entities, z.world, z.player, EntityKind::Bandit, 4, 9);
            Box3 b = hitBox(z.entities[0]), h = hitBox(z.entities[1]);
            CHECK(b.hi.y - b.lo.y < 1.2f && std::abs(h.hi.y - h.lo.y - kHeight) < 1e-4f && std::abs(h.hi.x - h.lo.x - 2 * gRanged.hitRadius) < 1e-4f);
        }
        glm::mat4 m = arrowPose({3, 4, 5}, {0, 0, 1});  // the tip there, the nock a block back
        CHECK(glm::distance(glm::vec3(m * glm::vec4(1.5f, 10, 0.5f, 1)), glm::vec3(3, 4, 5)) < 1e-4f);
        CHECK(glm::distance(glm::vec3(m * glm::vec4(1.5f, 0, 0.5f, 1)), glm::vec3(3, 4, 4)) < 1e-4f);
    }

    {  // The pose: the bow hangs at the side; raised to aim, an arrow on the string; drawn back to the cheek.
        Player p;
        p.inv.equipped[SlotMainHand] = bow;
        p.gripBlend = 1;
        glm::mat4 held(0), arrow(0);
        auto rest = poseModel(p, ModelPlayer, &held, 1, nullptr, nullptr, &arrow);
        CHECK(held != glm::mat4(0) && arrow == glm::mat4(0) && heldModel(p) == ModelBow);
        float low = 99;
        for (int c = 0; c < 8; ++c) low = std::min(low, (held * glm::vec4(c & 1 ? 1 : 0, c & 2 ? 15 : 0, c & 4 ? 4 : 0, 1)).y);
        CHECK(low > 0.05f);  // out of the ground
        p.aiming = true, p.aimBlend = 1;
        auto aimed = poseModel(p, ModelPlayer, &held, 1, nullptr, nullptr, &arrow);
        CHECK(arrow != glm::mat4(0) && aimed[HandL][3].z > rest[HandL][3].z + 0.5f && aimed[HandL][3].y > rest[HandL][3].y + 0.3f);
        p.aiming = false;  // raised (the draw's hold point is), aimBlend still 1: only the string hand differs
        startMove(p, Move::Shot, {});
        p.moveTime = 0.2f * holdTime(p);
        arrow = glm::mat4(0);
        auto early = poseModel(p, ModelPlayer, &held, 1, nullptr, nullptr, &arrow);
        CHECK(arrow != glm::mat4(0) && heldModel(p) == ModelBow);
        p.moveTime = holdTime(p), p.charge = gRanged.drawSeconds, p.charged = true;
        auto drawn = poseModel(p, ModelPlayer, &held, 1, nullptr, nullptr, &arrow);
        CHECK(heldModel(p) == ModelBowDrawn && drawn[HandR][3].z < early[HandR][3].z - 0.2f);  // the string hand back
        p.moveTime = 0.9f * moveLength(p), p.hitPending = false;
        arrow = glm::mat4(0);
        poseModel(p, ModelPlayer, &held, 1, nullptr, nullptr, &arrow);
        CHECK(arrow == glm::mat4(0) && heldModel(p) == ModelBow);  // loosed
    }

    {  // A rest and getting back up fill the quiver; the sounds.
        Player p;
        p.arrows = 3;
        rest(p);
        CHECK(p.arrows == kQuiver);
        auto names = [](const Sound& h) {
            std::set<std::string> out;
            for (const Layer& l : layersOf(h)) out.insert(l.name);
            return out;
        };
        CHECK(names({Noise::Loose, {}, -1}).contains("bow_loose") && names({Noise::Windup, {}, -1, Air, Move::Shot}).contains("bow_draw"));
        CHECK(names({Noise::Swing, {}, -1, Air, Move::Shot, bow}).empty());  // the string's let go is Loose's
        CHECK(names({Noise::Thunk, {}, -1, Stone}).contains("arrow_hit") && names({Noise::Hurt, {}, int(EntityKind::Bandit), Air, Move::None, bow}).contains("arrow_flesh"));
        CHECK(names({Noise::Hurt, {}, int(EntityKind::TrainingDummy), Air, Move::None, bow}).contains("arrow_hit"));
        float stone = layersOf({Noise::Thunk, {}, -1, Stone})[0].pitch, dirt = layersOf({Noise::Thunk, {}, -1, Dirt})[0].pitch;
        CHECK(stone > dirt);
    }
}

// Archers (ADR-051): an NPC with a bow keeps a range band from the player and shoots it with ADR-050's
// arrows, which hit only the player, through strikePlayer (the swings' path): guards, perfect guards, rolls.
static void archers() {
    namespace fs = std::filesystem;
    const ItemId bow = defs().itemById.at("short_bow");
    // A test kind: the archer with a true aim and no dodge, as save id 9.
    fs::path dir = fs::temp_directory_path() / "flycoos_archers_test";
    fs::create_directories(dir);
    copyWeaponClasses(dir);
    fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
    std::ostringstream text;
    text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string creatures = text.str();
    creatures.erase(creatures.rfind(']'));
    std::ofstream(dir / "creatures.json") << creatures << R"(,{"id": "sharp", "save_id": 10, "copy_from": "bandit_archer", "aim_error": 0, "dodge": null}])";
    Defs test, shipped;
    std::string error;
    CHECK(loadDefs(dir, test, error) && loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
    if (!error.empty()) std::println(stderr, "FAIL archer defs: {}", error);
    fs::remove_all(dir);
    setDefs(std::move(test));
    const EntityKind kSharp = EntityKind(10);
    const EntityType& archer = typeOf(EntityKind::BanditArcher);
    CHECK(archer.weapon == bow && archer.keepMin == 7 && archer.keepMax == 16 && archer.aimError == 0.07f && archer.sidearm == Knife &&
          archer.loot == bow && archer.model == ModelBandit && archer.guard == 0 && archer.parry == 0 && archer.cooldown > 0);

    // Stone ground (top at y 4), the player at x 8.5 facing +x with the sword, an archer `distance` ahead
    // chasing it (entities[0]); `wall`: a tall wall across at that x.
    auto arena = [&](float distance, EntityKind kind, int wall = -1) {
        Session s{.world = Grid({96, 32, 32}), .seed = 5};
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 96; ++x) {
                for (int y = 0; y < 4; ++y) s.world.set({x, y, z}, Stone);
                if (x == wall)
                    for (int y = 4; y < 24; ++y) s.world.set({x, y, z}, Stone);
            }
        startSession(s);
        s.player.teleport({8.5f, 4, 16.5f});
        s.player.yaw = std::numbers::pi_v<float> / 2;
        s.player.inv.equipped[SlotMainHand] = Shortsword;
        for (int i = 0; i < 10; ++i) step(s, {}, kTick);
        setState(spawnAhead(s.entities, s.world, s.player, kind, 3, distance), AiState::Chase, "test");
        s.sounds.clear();
        return s;
    };
    auto heard = [](const Session& s, Noise n, int kind) {
        return std::ranges::any_of(s.sounds, [&](const Sound& h) { return h.what == n && h.kind == kind; });
    };
    auto flatDist = [](const Player& a, const Player& b) { return glm::length(glm::vec2(a.pos.x - b.pos.x, a.pos.z - b.pos.z)); };
    auto newest = [](const Entity& e, uint32_t back = 0) { return std::string_view(e.history[(e.transitions - 1 - back) % e.history.size()].reason); };
    // Steps until the archer's next arrow is in the air (true), `in` each tick; `with` sees every tick after its step.
    auto untilLoosed = [&](Session& s, SimInput in = {}, int most = 600, const std::function<void()>& with = {}) {
        for (int i = 0; i < most; ++i) {
            size_t n = s.projectiles.size();
            step(s, in, kTick);
            if (with) with();
            if (s.projectiles.size() > n) return true;
        }
        return false;
    };

    {  // It shoots: a creaking draw it holds a while (the tell), at full draw, then the arrow, and the arrow hits.
        Session s = arena(12, kSharp);
        Entity& e = s.entities[0];
        Player& p = s.player;
        float health = p.health;
        int ticks = 0, drawing = -1;
        bool full = false, creak = false, last = false;
        CHECK(untilLoosed(s, {}, 600, [&] {
            ++ticks;
            if (drawing < 0 && e.body.move == Move::Shot) drawing = ticks;
            full |= e.body.move == Move::Shot && e.body.charged && e.body.moveTime == holdTime(e.body);
            creak |= heard(s, Noise::Windup, int(kSharp)), last |= heard(s, Noise::Charged, int(kSharp));
            s.sounds.clear();
        }));
        CHECK(drawing == 1 && float(ticks - drawing) * kTick >= 0.9f);  // the first shot at once, a long tell
        CHECK(full && creak && last && e.state == AiState::Windup && newest(e) == "shoots");
        const Projectile& a = s.projectiles[0];
        CHECK(a.owner == e.id && a.weapon == bow && a.power == 1 && a.stance == spec(e.body, Move::Shot).stance && !a.stuck);
        glm::vec2 at = glm::normalize(glm::vec2(p.pos.x - e.body.pos.x, p.pos.z - e.body.pos.z));
        CHECK(std::abs(glm::length(a.vel) - gRanged.maxSpeed) < 1e-3f && glm::dot(glm::normalize(glm::vec2(a.vel.x, a.vel.z)), at) > 0.999f);  // full draw, at the player
        CHECK(a.vel.y > glm::normalize(p.pos + glm::vec3(0, 1.1f, 0) - a.pos).y * gRanged.maxSpeed + 0.1f);  // lobbed onto the chest: above the straight line
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) step(s, {}, kTick);
        CHECK(s.projectiles.empty() && p.health < health && p.hitWith == bow && p.combatTimer > 0);
        for (int i = 0; i < 120 && e.state != AiState::Chase; ++i) step(s, {}, kTick);
        CHECK(e.state == AiState::Chase && newest(e, 1) == "shot");
    }
    {  // Its loose and the arrow into the player are heard (the bow's sounds, ADR-050), the player's hurt by an arrow.
        Session s = arena(12, kSharp);
        CHECK(untilLoosed(s) && heard(s, Noise::Loose, int(kSharp)));
        s.sounds.clear();
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) step(s, {}, kTick);
        CHECK(std::ranges::any_of(s.sounds, [&](const Sound& h) { return h.what == Noise::Hurt && h.kind < 0 && h.with == bow; }));
    }

    {  // A shield in front takes it (the heater stops all of a pierce), stamina only; from behind it gets through.
        for (bool back : {false, true}) {
            Session s = arena(12, kSharp);
            Player& p = s.player;
            p.inv.equipped[SlotOffHand] = defs().itemById.at("heater_shield");
            if (back) p.yaw = -p.yaw;
            float health = p.health;
            for (int i = 0; i < 30; ++i) step(s, {.guard = true}, kTick);  // up a while: not a perfect guard
            CHECK(p.guarding && untilLoosed(s, {.guard = true}));
            s.sounds.clear();
            for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) step(s, {.guard = true}, kTick);
            CHECK(s.projectiles.empty() && (back ? p.health < health : p.health == health));
            CHECK(heard(s, Noise::Block, -1) == !back && !heard(s, Noise::PerfectGuard, -1));
            if (!back) CHECK(p.stamina < gMelee.staminaMax && p.stagger == 0 && s.entities[0].body.freeze == 0);  // no recoil, no hit-stop
        }
    }
    {  // The sword's guard lets a part through; raised just as the arrow comes it's a perfect guard: nothing.
        float through[2] = {};
        for (bool perfect : {false, true}) {
            Session s = arena(12, kSharp);
            Player& p = s.player;
            float health = p.health;
            SimInput in{.guard = !perfect};
            for (int i = 0; i < 30; ++i) step(s, in, kTick);
            CHECK(untilLoosed(s, in));
            s.sounds.clear();
            for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) {
                in.guard |= perfect && s.projectiles[0].pos.x - p.pos.x < 6;  // a tick or so before it lands
                step(s, in, kTick);
            }
            through[perfect] = health - p.health;
            CHECK(heard(s, Noise::Block, -1) && heard(s, Noise::PerfectGuard, -1) == perfect);
        }
        CHECK(through[0] > 0 && through[1] == 0);
    }
    {  // Unguarded it hurts more, and makes the player flinch if its stance beats the poise.
        Session s = arena(12, kSharp);
        Player& p = s.player;
        float health = p.health;
        CHECK(untilLoosed(s));
        float stance = s.projectiles[0].stance;
        bool flinched = false;
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) step(s, {}, kTick), flinched |= p.stagger > 0;
        CHECK(health - p.health > 1 && flinched == (stance >= playerPoise(p)) && p.hitStance == stance);
        // The arrow's stance, not the archer's swings': armor for a poise between the two (9.2) takes it unshaken.
        for (ItemId id : {IronCuirass, IronHelm, IronSabatons}) p.inv.equipped[itemType(id).slot] = id;
        CHECK(playerPoise(p) > stance && playerPoise(p) < archer.hitStance);
        CHECK(untilLoosed(s, {}, 600));
        health = p.health, flinched = false;
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) step(s, {}, kTick), flinched |= p.stagger > 0;
        CHECK(p.health < health && !flinched);
    }
    {  // A roll through it: the i-frames let it by and it flies on behind.
        Session s = arena(12, kSharp);
        Player& p = s.player;
        float health = p.health;
        CHECK(untilLoosed(s));
        bool rolled = false, behind = false, through = false;
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) {
            bool near = s.projectiles[0].pos.x - p.pos.x < 5;
            step(s, {.move = {.move = {1, 0}, .roll = near && !rolled}}, kTick);
            rolled |= near;
            through |= invulnerable(p);
            behind |= !s.projectiles.empty() && s.projectiles[0].pos.x < p.pos.x - 0.5f;
        }
        CHECK(rolled && through && behind && p.health == health);
    }
    {  // It never hits another NPC: through a dummy in the way, into the player.
        Session s = arena(12, kSharp);
        spawnAhead(s.entities, s.world, s.player, EntityKind::TrainingDummy, 4, 6);
        float health = s.player.health, its = s.entities[1].body.health;
        CHECK(untilLoosed(s));
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) step(s, {}, kTick);
        CHECK(s.entities[1].body.health == its && s.player.health < health);
        // The player's arrows still hit NPCs (the archer here), not the player.
        Session z = arena(12, kSharp);
        Entity& e = z.entities[0];
        its = e.body.health, health = z.player.health;
        z.projectiles = {{z.player.pos + glm::vec3(0.6f, 1.2f, 0), {}, {40, 0, 0}, bow, 1, 5}};
        for (int i = 0; i < 30 && !z.projectiles.empty(); ++i) updateProjectiles(z, kTick);
        CHECK(e.body.health < its && z.player.health == health);
        // An archer's arrow flying at the player through the archer: past it, into the player.
        z.projectiles = {{e.body.pos + glm::vec3(1, 1.2f, 0), {}, {-40, 0, 0}, bow, 1, 5}};
        z.projectiles[0].owner = e.id;
        its = e.body.health;
        ItemId held = e.body.swingWeapon;  // (none: it hasn't swung) the bow for the hit, then back
        for (int i = 0; i < 30 && !z.projectiles.empty(); ++i) updateProjectiles(z, kTick);
        CHECK(z.player.health < health && e.body.health == its && z.player.hitWith == bow && e.body.swingWeapon == held && held == NoItem);
        // A guard faces where the arrow comes from, not where its archer stands: one from behind gets through.
        z.player.inv.equipped[SlotOffHand] = defs().itemById.at("heater_shield");
        for (int i = 0; i < 30; ++i) step(z, {.guard = true}, kTick);
        z.projectiles = {{z.player.pos + glm::vec3(-3, 1.2f, 0), {}, {40, 0, 0}, bow, 1, 5}};
        z.projectiles[0].owner = e.id;
        health = z.player.health;
        CHECK(z.player.guarding);
        for (int i = 0; i < 30 && !z.projectiles.empty(); ++i) updateProjectiles(z, kTick);
        CHECK(z.player.health < health && !z.player.made(Noise::Block));
        z.player.inv.equipped[SlotOffHand] = NoItem;
        // A wall before the player stops it there, even within one tick's flight.
        health = z.player.health;
        for (int y = 4; y < 8; ++y) z.world.set({11, y, 16}, Stone);
        z.projectiles = {{z.player.pos + glm::vec3(6, 1.2f, 0), {}, {-600, 0, 0}, bow, 1, 5}};
        z.projectiles[0].owner = e.id;
        for (int i = 0; i < 3; ++i) updateProjectiles(z, kTick);
        CHECK(z.projectiles.size() == 1 && z.projectiles[0].stuck && z.player.health == health);
    }
    {  // With its archer gone (despawned) an arrow is no one's: it flies through the player.
        Session s = arena(12, kSharp);
        float health = s.player.health;
        CHECK(untilLoosed(s));
        s.entities.clear();
        bool past = false;
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i)
            step(s, {}, kTick), past |= !s.projectiles.empty() && s.projectiles[0].pos.x < s.player.pos.x - 0.5f;
        CHECK(past && s.player.health == health);
    }
    {  // A hit mid-draw that makes it flinch (its poise is 0) spoils the shot: no arrow.
        Session s = arena(12, kSharp);
        Entity& e = s.entities[0];
        for (int i = 0; i < 300 && !(e.body.move == Move::Shot && e.body.moveTime == holdTime(e.body)); ++i) step(s, {}, kTick);
        CHECK(e.body.move == Move::Shot && e.body.hitPending);
        s.projectiles = {{e.body.pos + glm::vec3(-2, 1.2f, 0), {}, {40, 0, 0}, bow, 1, 5}};  // the player's
        float health = s.player.health;
        bool flinched = false, shot = false;
        for (int i = 0; i < 90; ++i) {
            size_t n = s.projectiles.size();
            step(s, {}, kTick);
            flinched |= e.body.stagger > 0, shot |= s.projectiles.size() > n;
        }
        CHECK(flinched && !shot && s.player.health == health);
    }

    {  // The band: too close it backs off facing the player; too far it closes in; between, it strafes round it.
        Session s = arena(4.5f, kSharp);
        Entity& e = s.entities[0];
        for (int i = 0; i < 50; ++i) step(s, {}, kTick);  // (within its cooldown: no shot yet)
        glm::vec2 to = glm::normalize(glm::vec2(s.player.pos.x - e.body.pos.x, s.player.pos.z - e.body.pos.z));
        CHECK(flatDist(e.body, s.player) > 5 && e.body.move == Move::None && ranged(e.body));
        CHECK(glm::dot(glm::vec2(std::sin(e.body.yaw), std::cos(e.body.yaw)), to) > 0.9f);  // facing the player
        Session far = arena(19, kSharp);
        for (int i = 0; i < 60; ++i) step(far, {}, kTick);
        CHECK(flatDist(far.entities[0].body, far.player) < 17.5f && far.projectiles.empty());
        Session mid = arena(12, kSharp);  // (its first shot comes at once; between shots it strafes)
        Entity& m = mid.entities[0];
        CHECK(untilLoosed(mid));
        for (int i = 0; i < 120 && m.state != AiState::Chase; ++i) step(mid, {}, kTick);
        glm::vec3 from = m.body.pos;
        for (int i = 0; i < 40; ++i) step(mid, {}, kTick);
        CHECK(m.state == AiState::Chase && std::abs(m.body.pos.z - from.z) > 0.5f && std::abs(flatDist(m.body, mid.player) - 12) < 0.6f && m.sight);
    }
    {  // No line to the player (a wall): it doesn't shoot; it steps in, and gets stuck at the wall.
        Session s = arena(12, kSharp, 14);
        Entity& e = s.entities[0];
        bool lost = false;
        for (int i = 0; i < 240; ++i) step(s, {}, kTick), lost |= e.lostTime > 0;
        CHECK(!e.sight && s.projectiles.empty() && e.body.move == Move::None && lost);
    }
    {  // Cornered (a wall right behind it) and too close, it can't back off: it shoots point-blank.
        Session s = arena(5, kSharp, 15);
        Entity& e = s.entities[0];
        CHECK(untilLoosed(s, {}, 300) && flatDist(e.body, s.player) < archer.keepMin && s.projectiles[0].owner == e.id);
    }
    {  // Close in it takes out its knife and fights with it, in the pack (tokens); away again, the bow comes back.
        Session s = arena(2.5f, kSharp);
        Entity& e = s.entities[0];
        Player& p = s.player;
        float health = p.health;
        step(s, {}, kTick), step(s, {}, kTick);  // (the ring is dealt before it thinks: a tick later)
        CHECK(e.body.inv.equipped[SlotMainHand] == Knife && !ranged(e.body) && e.pack == 1 && e.token);
        bool swung = false;
        for (int i = 0; i < 240 && p.health == health; ++i) step(s, {}, kTick), swung |= e.body.move >= Move::Light1 && e.body.move <= Move::Light4;
        CHECK(swung && p.health < health && p.hitWith == Knife && s.projectiles.empty());
        if (e.body.punching()) {  // mid-swing it keeps the knife in hand, the player gone or not
            p.teleport(e.body.pos - glm::vec3(9, 0, 0));
            step(s, {}, kTick);
            CHECK(e.body.inv.equipped[SlotMainHand] == Knife);
            p.teleport(e.body.pos - glm::vec3(2, 0, 0));
        }
        for (int i = 0; i < 120 && e.body.punching(); ++i) step(s, {}, kTick);
        p.teleport(e.body.pos - glm::vec3(5, 0, 0));  // inside its band's near end: still the knife
        step(s, {}, kTick);
        CHECK(e.body.inv.equipped[SlotMainHand] == Knife);
        p.teleport(e.body.pos - glm::vec3(9, 0, 0));  // past it: the bow
        step(s, {}, kTick), step(s, {}, kTick);
        CHECK(e.body.inv.equipped[SlotMainHand] == bow && ranged(e.body) && e.pack == 0);
        // Dead, it holds its bow again (it comes back an archer).
        p.teleport(e.body.pos - glm::vec3(2, 0, 0));
        step(s, {}, kTick);
        CHECK(e.body.inv.equipped[SlotMainHand] == Knife);
        damage(e, 9999);
        step(s, {}, kTick), step(s, {}, kTick);
        CHECK(!e.body.alive() && e.body.inv.equipped[SlotMainHand] == bow);
    }
    {  // Its knife is slow (its cooldown spaces every attack: a poor fighter up close); a closeIn past its keepMin doesn't flip-flop its hands.
        Session s = arena(2.5f, kSharp);
        Entity& e = s.entities[0];
        int swings = 0;
        for (int i = 0; i < 240; ++i) {
            Move was = e.body.move;
            step(s, {}, kTick);
            swings += e.body.move != was && e.body.move >= Move::Light1 && e.body.move <= Move::Light4;
        }
        CHECK(swings >= 1 && swings <= 2);  // in 4 s (a bandit swings every second or so)
        for (int i = 0; i < 120 && e.body.punching(); ++i) step(s, {}, kTick);
        gAi.closeIn = 8;
        s.player.teleport(e.body.pos - glm::vec3(7.5f, 0, 0));
        bool flipped = false;
        for (int i = 0; i < 10; ++i) step(s, {}, kTick), flipped |= e.body.inv.equipped[SlotMainHand] != Knife;
        CHECK(!flipped);
        gAi = AiTuning{};
    }
    {  // With its bow out it's no fighter on the ring: it takes no side and no token from the bandit; and it doesn't guard.
        Session s = arena(12, kSharp);
        setState(spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 4, 2), AiState::Chase, "test");
        step(s, {}, kTick);
        CHECK(s.entities[0].pack == 0 && s.entities[1].pack == 1 && s.entities[1].token);
        for (int i = 0; i < 240 && s.entities[0].state != AiState::Chase; ++i) step(s, {}, kTick);  // its first shot done
        s.entities[0].guardTimer = 1, s.entities[0].sinceSwing = 0;  // means to guard, no shot due
        step(s, {}, kTick);
        CHECK(s.entities[0].state == AiState::Chase && !s.entities[0].body.guarding);
    }
    {  // strikePlayer itself: an arrow's power scales its damage; a parry in its window only guards an arrow (a weak guard)
       // but deflects a swing.
        Session s = arena(12, kSharp);
        Entity& e = s.entities[0];
        Player& p = s.player;
        e.body.swingWeapon = bow;
        float hit[2];
        for (int k = 0; k < 2; ++k) {
            p.health = 100, s.rng.seed(3);
            strikePlayer(e, p, {k ? 0.5f : 1.0f, 0, e.body.pos, DamageType::Pierce, false, false, false}, s.rng);
            hit[k] = 100 - p.health;
        }
        CHECK(hit[0] > 4 && std::abs(hit[1] - hit[0] / 2) <= 1);
        p.inv.equipped[SlotOffHand] = defs().itemById.at("heater_shield");
        for (bool melee : {false, true}) {
            p.health = 100, p.noises = 0, e.parried = 0, e.body.stagger = 0;
            startMove(p, Move::Parry, {1, 0});
            p.moveScale = parryOf(p);
            p.moveTime = 0.2f * p.moveScale;
            CHECK(parries(p, e.body.pos));
            strikePlayer(e, p, {1, 5, e.body.pos, DamageType::Pierce, false, true, melee}, s.rng);
            CHECK(p.made(Noise::Parry) == melee && p.made(Noise::Block) == !melee && (e.body.stagger > 0) == melee);
            interrupt(p, 0, false);
        }
    }
    {  // Its aim (the shipped archer's): off by up to aimError either way, both ways, from its facing.
        Session s = arena(12, EntityKind::BanditArcher);
        Entity& e = s.entities[0];
        e.body.swingWeapon = bow, e.body.charge = 1;
        e.body.yaw = std::numbers::pi_v<float> * 1.5f;  // facing the player (-x)
        float most = 0, sum = 0;
        int left = 0;
        for (int i = 0; i < 200; ++i) {
            s.projectiles.clear();
            loose(s, e);
            float off = std::atan2(s.projectiles[0].vel.z, -s.projectiles[0].vel.x);
            most = std::max(most, std::abs(off)), sum += off, left += off > 0;
        }
        CHECK(most <= archer.aimError * 1.1f && most > archer.aimError * 0.7f && std::abs(sum / 200) < 0.01f && left > 60 && left < 140);
        float low = 99, high = -99;  // up and down too; and along its facing, not at the player (a late sidestep beats it)
        e.body.yaw += 0.2f;
        sum = 0;
        for (int i = 0; i < 200; ++i) {
            s.projectiles.clear();
            loose(s, e);
            sum += std::atan2(s.projectiles[0].vel.z, -s.projectiles[0].vel.x);
            low = std::min(low, s.projectiles[0].vel.y), high = std::max(high, s.projectiles[0].vel.y);
        }
        CHECK(std::abs(sum / 200 - 0.2f) < 0.01f && high - low > 3);
    }
    {  // Deterministic: the same fight twice, tick for tick.
        auto run = [&] {
            Session s = arena(12, EntityKind::BanditArcher);
            spawnAhead(s.entities, s.world, s.player, EntityKind::BanditArcher, 8, 14);
            setState(s.entities[1], AiState::Chase, "test");
            std::vector<float> trace;
            for (int i = 0; i < 900; ++i) {
                step(s, {.move = {.move = {0, i % 240 < 120 ? 1.0f : -1.0f}}}, kTick);
                for (const Projectile& a : s.projectiles) trace.push_back(a.pos.x + a.pos.y + a.pos.z);
                trace.push_back(s.player.health);
            }
            return trace;
        };
        std::vector<float> a = run();
        CHECK(a == run() && a.back() < 100);  // (the last is the player's health: hit along the way)
    }

    // Balance band: the starting player (the shortsword, no armor) runs at the shipped archer from 14 blocks
    // and cuts it down, swinging when in reach; then a bandit with an archer behind. Twelve seeds; the
    // means of the health left and the seconds it takes are what's tuned.
    auto band = [&](bool withBandit, uint32_t seed) {
        Session s = arena(14, EntityKind::BanditArcher);
        s.rng.seed(seed);
        s.entities[0].rng.seed(seed);
        if (withBandit) setState(spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, seed + 1, 8), AiState::Chase, "test");
        Player& p = s.player;
        for (int i = 0; i < 90 * 60; ++i) {
            auto alive = s.entities | std::views::filter([](const Entity& e) { return e.body.alive(); });
            auto flat = [&](const Entity& e) { return glm::vec2(e.body.pos.x - p.pos.x, e.body.pos.z - p.pos.z); };
            auto foe = std::ranges::min_element(alive, {}, [&](const Entity& e) { return glm::length(flat(e)); });
            if (foe == alive.end()) return std::pair(p.health, float(i) * kTick);
            glm::vec2 to = glm::normalize(flat(*foe));
            bool reach = glm::length(flat(*foe)) < kPunchReach - 0.2f;
            SimInput in{.move = {.move = reach ? glm::vec2(0) : to, .run = true}, .aim = to};
            in.attack = reach && !p.punching() && p.stamina > 15;
            step(s, in, kTick);
            if (!p.alive()) return std::pair(-1.0f, float(i) * kTick);
        }
        return std::pair(999.0f, 90.0f);
    };
    for (bool withBandit : {false, true}) {
        float health = 0, seconds = 0;
        int won = 0;
        for (uint32_t seed = 1; seed <= 12; ++seed) {
            auto [left, took] = band(withBandit, seed);
            won += left > 0 && left < 999, health += std::max(left, 0.0f), seconds += took;
        }
        std::println("level band: the starting player rushing {}: won {} of 12, health left {:.0f} on average, {:.1f} s",
                     withBandit ? "a bandit and an archer" : "an archer", won, health / 12, seconds / 12);
        // Alone it costs a rush an arrow or two; covering a bandit it's about as much trouble as a second bandit
        // (28 health left against two, rushing them the same way; 56 against one).
        CHECK(won >= (withBandit ? 8 : 12) && health / 12 < (withBandit ? 45 : 97) && health / 12 > (withBandit ? 12 : 75));
    }
    setDefs(std::move(shipped));
}

// Crossbows (ADR-052): a ranged class that loads. Loaded, a press fires at once (no draw: full power, a
// faster bolt); then it reloads, a move queued like a press, seating a bolt from the quiver at its impact
// frame. A roll before then leaves it empty, and the next press reloads first.
static void crossbows() {
    const Defs& d = defs();
    const ItemId xbow = d.itemById.at("light_crossbow"), bow = d.itemById.at("short_bow");
    const WeaponClassDef& c = weaponClass(WeaponClass::Crossbow);
    const MoveSpec &shot = spec(WeaponClass::Crossbow, Move::Shot), &reload = spec(WeaponClass::Crossbow, Move::Reload);
    CHECK(classOf(xbow) == WeaponClass::Crossbow && c.ranged && c.loads && c.twoHanded && heldModel(xbow) == ModelCrossbow);
    CHECK(itemType(xbow).damage == DamageType::Pierce && itemType(xbow).attack.minDamage > itemType(bow).attack.maxDamage);  // harder than a bow...
    CHECK(std::ranges::all_of(itemType(xbow).attack.scaling, [](float k) { return k == 0; }));  // ...and no attribute adds to it
    CHECK(shot.stance > spec(WeaponClass::Bow, Move::Shot).stance && shot.seconds < reload.seconds && reload.damage == 0 && gRanged.boltSpeed > gRanged.maxSpeed);
    CHECK(!weaponClass(WeaponClass::Bow).loads && spec(WeaponClass::Bow, Move::Reload).seconds == 0);

    // A field: ground top at y 4, the player at x 8.5 facing +x with the crossbow, empty.
    auto field = [&] {
        Session s{.world = Grid({128, 32, 32}), .seed = 5};
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 128; ++x)
                for (int y = 0; y < 4; ++y) s.world.set({x, y, z}, Stone);
        startSession(s);
        s.player.teleport({8.5f, 4, 16.5f});
        s.player.yaw = std::numbers::pi_v<float> / 2;
        s.player.inv.equipped[SlotMainHand] = xbow;
        for (int i = 0; i < 10; ++i) step(s, {}, kTick);
        s.sounds.clear();
        return s;
    };
    auto heard = [](const Session& s, Noise n, Move m) {
        return std::ranges::any_of(s.sounds, [&](const Sound& h) { return h.what == n && h.kind < 0 && h.move == m; });
    };
    SimInput press{.attack = true, .attackHeld = true}, roll;
    roll.move.roll = true, roll.move.move = {0, 1};
    // Steps until `done`, at most `most` ticks; how many it took (most + 1: never).
    auto until = [](Session& s, const std::function<bool()>& done, int most = 300, SimInput in = {}) {
        for (int i = 1; i <= most; ++i) {
            step(s, in, kTick);
            if (done()) return i;
        }
        return most + 1;
    };

    {  // Empty, a press reloads: the crank, the bolt seated from the quiver at the reload's impact frame; no shot.
        Session s = field();
        Player& p = s.player;
        CHECK(p.loaded == NoItem && heldModel(p) == ModelCrossbow);
        step(s, press, kTick);
        CHECK(p.move == Move::Reload && p.hitPending && !p.holding && heard(s, Noise::Windup, Move::Reload));
        int seat = until(s, [&] { return p.loaded == xbow; }, 200, {.attackHeld = true});
        CHECK(std::abs(float(seat + 1) * kTick - reload.impact * reload.seconds) < 2.5f * kTick && p.arrows == kQuiver - 1);
        CHECK(heard(s, Noise::Charged, Move::Reload) && heldModel(p) == ModelCrossbowSpanned);
        until(s, [&] { return !p.punching(); }, 200, {.attackHeld = true});
        for (int i = 0; i < 60; ++i) step(s, {.attackHeld = true}, kTick);  // held down it doesn't fire: a press does
        CHECK(s.projectiles.empty() && p.loaded == xbow && p.move == Move::None);

        // Loaded, a press fires at once: no draw however long it's held, full power, the fast bolt along the facing.
        s.sounds.clear();
        step(s, press, kTick);
        CHECK(p.move == Move::Shot && !p.holding && p.stamina < gMelee.staminaMax);
        int fired = until(s, [&] { return !s.projectiles.empty(); }, 60, {.attackHeld = true});
        CHECK(std::abs(float(fired + 1) * kTick - shot.impact * shot.seconds) < 2.5f * kTick);
        const Projectile& a = s.projectiles[0];
        CHECK(a.weapon == xbow && a.power == shot.damage && a.stance == shot.stance && std::abs(glm::length(a.vel) - gRanged.boltSpeed) < 0.01f);
        CHECK(a.vel.x > 0.99f * glm::length(a.vel) && p.loaded == NoItem && p.arrows == kQuiver - 1 && heard(s, Noise::Loose, Move::Shot));
        CHECK(heldModel(p) == ModelCrossbow);
        // Then it reloads by itself, like a press: the next bolt, from the quiver.
        CHECK(until(s, [&] { return p.move == Move::Reload; }, 60) <= int(shot.seconds / kTick) && p.want == Want::None);
        CHECK(until(s, [&] { return p.loaded == xbow; }, 200) <= 200 && p.arrows == kQuiver - 2 && s.projectiles.size() == 1);
        // A press during the reload waits for it (the buffer): it fires as the reload ends.
        Session t = field();
        t.player.loaded = xbow;
        step(t, press, kTick);
        until(t, [&] { return t.player.move == Move::Reload; }, 60);
        until(t, [&] { return t.player.moveTime > 0.7f * moveLength(t.player); }, 200);
        step(t, press, kTick);
        CHECK(until(t, [&] { return t.projectiles.size() == 2; }, 120) <= 60 && t.player.arrows == kQuiver - 1);
    }
    {  // A roll in the reload before the bolt is in: it stays empty, the arrow still in the quiver; the next press reloads first.
        Session s = field();
        Player& p = s.player;
        step(s, press, kTick);
        for (int i = 0; i < 30; ++i) step(s, {}, kTick);
        CHECK(p.move == Move::Reload && p.hitPending);
        step(s, roll, kTick);
        for (int i = 0; i < 90; ++i) step(s, {}, kTick);
        CHECK(p.move == Move::None && p.loaded == NoItem && p.arrows == kQuiver);
        step(s, press, kTick);
        CHECK(p.move == Move::Reload);
        // A roll out of the shot's recovery skips the reload it queued.
        Session t = field();
        t.player.loaded = xbow;
        step(t, press, kTick);
        until(t, [&] { return !t.projectiles.empty(); }, 60);
        step(t, roll, kTick);
        for (int i = 0; i < 120; ++i) step(t, {}, kTick);
        CHECK(t.player.rollTime < 99 && t.player.move == Move::None && t.player.loaded == NoItem && t.player.arrows == kQuiver);
        // A roll in the raise, before the release, keeps the bolt in.
        Session u = field();
        u.player.loaded = xbow;
        step(u, press, kTick);
        step(u, roll, kTick);
        for (int i = 0; i < 60; ++i) step(u, {}, kTick);
        CHECK(u.projectiles.empty() && u.player.loaded == xbow && u.player.move == Move::None);
    }
    {  // The automatic reload isn't a press: a sword swapped in before it starts doesn't swing for it...
        Session s = field();
        Player& p = s.player;
        p.loaded = xbow;
        step(s, press, kTick);
        until(s, [&] { return !s.projectiles.empty(); }, 60);
        CHECK(p.want == Want::Light && p.reloadWant);
        p.inv.equipped[SlotMainHand] = Shortsword;
        bool swung = false;
        for (int i = 0; i < 60; ++i) step(s, {}, kTick), swung |= p.move != Move::None && p.move != Move::Shot;
        CHECK(!swung && p.move == Move::None && p.want == Want::None && p.arrows == kQuiver);
        // ...and a shot that spends the last stamina still reloads (a reload costs none).
        Session t = field();
        t.player.loaded = xbow, t.player.stamina = 3;
        step(t, press, kTick);
        CHECK(until(t, [&] { return t.player.move == Move::Reload; }, 60) <= 60 && t.player.stamina <= 0);
        // Mid-swing with a sword, a bow equipped and pressed shoots (what's in hand decides, not the swing's).
        Session u = field();
        u.player.inv.equipped[SlotMainHand] = Shortsword;
        step(u, {.attack = true}, kTick);
        step(u, {}, kTick);
        CHECK(u.player.move == Move::Light1);
        u.player.inv.equipped[SlotMainHand] = bow;
        step(u, {.attack = true}, kTick);
        CHECK(until(u, [&] { return u.player.move == Move::Shot; }, 60) <= 60);
    }
    {  // A bandit facing a reloading player doesn't answer it as an attack (it hits nothing); a shot it does.
        Session s = field();
        Player& p = s.player;
        Entity& e = spawnAhead(s.entities, s.world, p, EntityKind::Bandit, 3, 2.5f);
        setState(e, AiState::Chase, "test");
        e.body.swingTimer = 5;
        step(s, press, kTick);
        CHECK(p.move == Move::Reload);
        bool answered = false;
        for (int i = 0; i < 40 && p.move == Move::Reload; ++i) step(s, {}, kTick), answered |= e.picked;
        CHECK(!answered);
    }
    {  // An empty quiver: a loaded crossbow still fires, then has nothing to reload; a press does nothing.
        Session s = field();
        Player& p = s.player;
        p.loaded = xbow, p.arrows = 0;
        step(s, press, kTick);
        CHECK(until(s, [&] { return !s.projectiles.empty(); }, 60) <= 60);
        for (int i = 0; i < 120; ++i) step(s, {}, kTick);
        CHECK(p.move == Move::None && p.loaded == NoItem && p.arrows == 0);
        step(s, press, kTick);
        CHECK(p.move == Move::None && p.want == Want::None);
    }
    {  // The bolt is the crossbow's: put away it stays in; a bow in hand shoots its own arrows (from the same quiver).
        Session s = field();
        Player& p = s.player;
        p.loaded = xbow;
        p.inv.equipped[SlotMainHand] = bow;
        CHECK(!loads(p));
        step(s, {.attack = true}, kTick);
        CHECK(until(s, [&] { return !s.projectiles.empty(); }, 90) <= 90 && p.arrows == kQuiver - 1 && p.loaded == xbow);
        for (int i = 0; i < 90; ++i) step(s, {}, kTick);
        p.inv.equipped[SlotMainHand] = xbow;
        CHECK(heldModel(p) == ModelCrossbowSpanned);
        step(s, press, kTick);
        CHECK(p.move == Move::Shot);  // still loaded: it fires
        // Another weapon's bolt isn't this one's: it reloads; and with none in it, the release looses nothing.
        Session t = field();
        t.player.loaded = bow;
        step(t, press, kTick);
        CHECK(t.player.move == Move::Reload);
        t.player.loaded = NoItem, t.player.swingWeapon = xbow;
        loose(t, std::nullopt);
        CHECK(t.projectiles.empty() && t.player.arrows == kQuiver);
    }
    {  // Aim mode (the guard button) as with a bow: aiming, a slow walk, straight at the crosshair's point.
        Session s = field();
        Player& p = s.player;
        p.loaded = xbow;
        glm::vec3 at = p.pos + glm::vec3(30, 4, 3);
        for (int i = 0; i < 20; ++i) step(s, {.guard = true, .aimAt = at}, kTick);
        CHECK(p.aiming && !p.guarding);
        step(s, {.attack = true, .attackHeld = true, .guard = true, .aimAt = at}, kTick);
        until(s, [&] { return !s.projectiles.empty(); }, 60, {.guard = true, .aimAt = at});
        CHECK(!s.projectiles.empty());
        if (!s.projectiles.empty()) {
            const Projectile& a = s.projectiles[0];
            CHECK(glm::length(glm::normalize(a.vel) - glm::normalize(at - a.pos)) < 0.05f);
        }
        // While reloading it walks slowly.
        until(s, [&] { return p.move == Move::Reload; }, 60);
        SimInput walk;
        walk.move.move = {0, 1}, walk.move.run = true;
        for (int i = 0; i < 20; ++i) step(s, walk, kTick);
        float speed = glm::length(glm::vec2(p.vel.x, p.vel.z));
        CHECK(p.move == Move::Reload && speed <= gRanged.aimWalk + 0.01f && speed > 0.7f * gRanged.aimWalk);
    }
    {  // A bolt into a bandit: it hurts, and harder than a bow's full draw on average (twelve of each).
        auto hits = [&](ItemId weapon) {
            float total = 0;
            for (int i = 0; i < 12; ++i) {
                Session s = field();
                s.rng.seed(uint32_t(100 + i));
                Entity& e = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 3, 8);
                float before = e.body.health;
                s.player.inv.equipped[SlotMainHand] = weapon;
                s.player.loaded = xbow;
                s.player.charge = gRanged.drawSeconds;  // (a bow at full draw)
                s.projectiles.clear();
                std::optional<glm::vec3> none;
                s.player.swingWeapon = weapon;
                loose(s, none);
                for (int k = 0; k < 60 && !s.projectiles.empty() && !s.projectiles[0].stuck; ++k) updateProjectiles(s, kTick);
                total += before - s.entities[0].body.health;
            }
            return total / 12;
        };
        float bolt = hits(xbow), arrow = hits(bow);
        CHECK(bolt > 0 && arrow > 0 && bolt > 1.3f * arrow);
        std::println("level band: a crossbow bolt into a bandit {:.1f}, a full-draw arrow {:.1f} (the starting stats)", bolt, arrow);
    }
    {  // The models: voxels in the Head slot only; spanned differs from slack; a bolt shorter than an arrow, tipped the same.
        auto count = [](Model m) {
            Grid g = buildPart(m, Head);
            int n = 0;
            for (int z = 0; z < g.size.z; ++z)
                for (int y = 0; y < g.size.y; ++y)
                    for (int x = 0; x < g.size.x; ++x) n += g.get({x, y, z}) != Air;
            return n;
        };
        CHECK(count(ModelCrossbow) > 30 && count(ModelCrossbowSpanned) > 30 && count(ModelBolt) > 5 && count(ModelBolt) < count(ModelArrow));
        CHECK(buildPart(ModelBolt, Head).size == buildPart(ModelArrow, Head).size && buildPart(ModelCrossbow, Torso).size == glm::ivec3(1));
        Grid slack = buildPart(ModelCrossbow, Head), spanned = buildPart(ModelCrossbowSpanned, Head);
        CHECK(slack.get({6, 2, 8}) == Straw && spanned.get({6, 2, 4}) == Straw && slack.get({6, 2, 4}) == Air);
    }
    {  // The pose: in both hands; raised, the trigger hand at its grip and the butt at the shoulder; the bolt drawn only when in it.
        Session s = field();
        Player& p = s.player;
        auto at = [](const glm::mat4& m, glm::vec3 v) { return glm::vec3(m * glm::vec4(v, 1)); };
        auto hand = [&](const std::array<glm::mat4, PartCount>& parts, int i) { return at(parts[i], glm::vec3(1.5f)); };  // a hand's middle (3 voxels)
        glm::mat4 held(0), arrow(0);
        auto parts = poseModel(p, ModelPlayer, &held, 1, nullptr, nullptr, &arrow);
        CHECK(held != glm::mat4(0) && arrow == glm::mat4(0));
        CHECK(glm::length(hand(parts, HandR) - at(held, {6.5f, 0.5f, 3.5f})) < 0.15f);  // at rest it hangs from the right hand...
        Player withBow = p;  // ...where the hand is with a bow (no two-handed rest pose)
        withBow.inv.equipped[SlotMainHand] = bow;
        CHECK(glm::length(hand(parts, HandR) - hand(poseModel(withBow, ModelPlayer), HandR)) < 1e-4f && p.gripBlend > 0.9f);
        p.loaded = xbow, p.aimBlend = 1, p.aiming = true;
        held = arrow = glm::mat4(0);
        parts = poseModel(p, ModelPlayer, &held, 1, nullptr, nullptr, &arrow);
        CHECK(arrow != glm::mat4(0) && glm::length(hand(parts, HandR) - at(held, {6.5f, 0.5f, 3.5f})) < 0.05f &&
              glm::length(hand(parts, HandL) - at(held, {6.5f, 0.5f, 8.5f})) < 0.05f);
        glm::vec3 butt = at(held, {6.5f, 1, 0}), nose = at(held, {6.5f, 1, 12});
        CHECK(butt.y > p.pos.y + 0.9f && std::abs(nose.y - butt.y) < 0.05f && nose.x > butt.x + 1);  // level, pointing +x (the facing)
        CHECK(butt.z - p.pos.z > 0.45f);  // at the right shoulder (facing +x, the right is +z), not in front of the chest
        CHECK(glm::length(at(arrow, {1.5f, 10, 0.5f}) - at(held, {6.5f, 2.5f, 11})) < 0.01f);  // the bolt's tip in the groove's front
        // Reloading: tipped nose down, the right hand on the string.
        p.aiming = false, p.aimBlend = 0, p.loaded = NoItem;
        step(s, press, kTick);
        for (int i = 0; i < 40; ++i) step(s, {}, kTick);
        CHECK(p.move == Move::Reload);
        held = glm::mat4(0);
        parts = poseModel(p, ModelPlayer, &held, 1);
        CHECK(at(held, {6.5f, 1, 12}).y < at(held, {6.5f, 1, 0}).y - 0.5f);
        glm::vec3 r = hand(parts, HandR), a = at(held, {6.5f, 2.5f, 8.5f}), b = at(held, {6.5f, 2.5f, 4.5f});  // on the string's way back
        float along = std::clamp(glm::dot(r - a, b - a) / glm::dot(b - a, b - a), 0.0f, 1.0f);
        CHECK(glm::length(r - glm::mix(a, b, along)) < 0.03f && along > 0.1f && along < 0.9f);
    }
}

// Throwables (ADR-053): a pouch per kind, a left-hand throw, knives that stick and bleed, bombs and pots that burst.
// The combat test arena (ADR-054): built around the world's middle, its zones, prefabs and markers,
// a rest by the start, and spawning groups at points from the middle.
static void arena() {
    namespace fs = std::filesystem;
    CHECK(worldMiddle(Grid({64, 32, 64})) == glm::ivec2(32, 32) && worldMiddle(Grid({64, 32, 64}, {-32, 0, 10})) == glm::ivec2(0, 42));
    CHECK(worldMiddle(Grid::streamed({256, 32, 256}, {-512, 0, 96})) == glm::ivec2(0));  // the endless world's origin, wherever it streams
    PrefabLibrary lib = loadPrefabs(fs::path(FLYCOOS_SOURCE_DIR) / "prefabs");
    CHECK(lib.errors.empty());
    for (const char* name : {"arena_pillar", "brazier", "banner", "arena_gate", "archery_target", "crate_stack"}) CHECK(lib.find(name));
    Session s{.world = Grid({192, 96, 192}), .seed = 1337};
    Grid& g = s.world;
    std::vector<Instance> instances;
    CHECK(buildArena(g, PrefabLibrary{}, instances, s.markers).starts_with("no prefab"));
    CHECK(buildArena(g, lib, instances, s.markers).empty());
    constexpr int F = kArenaFloor, R = kArenaRadius;
    const glm::ivec2 mid = worldMiddle(g);  // (96, 96)
    auto top = [&](int x, int z) { return groundTop(g, mid.x + x, mid.y + z); };
    auto at = [&](int x, int y, int z) { return g.get({mid.x + x, y, mid.y + z}); };
    CHECK(instances.size() == 26);  // 2 gates, 5 pillars, 3 crates, 3 targets, 5 braziers, 8 banners
    CHECK(s.markers.size() == 10);
    for (size_t i = 0; i < s.markers.size(); ++i) {
        CHECK(s.markers[i].id == i + 1 && !checkMarker(g, s.markers[i]));
        if (const char* why = checkMarker(g, s.markers[i])) std::println(stderr, "  marker {}: {}", s.markers[i].name, why);
    }
    CHECK(std::ranges::count(s.markers, MarkerKind::Poi, &Marker::kind) == 7);
    // The open middle: sand, the (0, 0) mark and the duel ring in clay, flat out to the zones.
    CHECK(top(0, 0) == F && at(0, F, 0) == Clay && at(-1, F, -1) == Clay && at(1, F, 0) == Sand && at(0, F + 1, 0) == Air);
    CHECK(at(11, F, 0) == Clay && at(-12, F, 0) == Clay && at(10, F, 0) == Sand);  // d 11.5..12.5 from the middle corner
    for (int z = -16; z <= 16; ++z)
        for (int x = -16; x <= 16; ++x) CHECK(top(x, z) == F);
    // The wall (walkway on top) is too high to climb out of; outside, the grass plain.
    CHECK(top(40, 40) == F + 7 && at(40, F + 7, 40) == Slate);
    CHECK(top(-41, -41) == F + 7 && top(-48, -48) == F + 12);  // a tier of the stands
    CHECK(top(90, 90) == F && at(90, F, 90) == Grass && at(90, F - 1, 90) == Dirt && at(90, 0, 90) == Stone);
    // Nothing stands just inside the wall (the banners are all on the walkway), and every banner's
    // pole and cloth are up there: eight poles on the walkway, none on the floor.
    int poles = 0;
    for (int z = -60; z < 60; ++z)
        for (int x = -60; x < 60; ++x) {
            float d = std::hypot(float(x) + 0.5f, float(z) + 0.5f);
            if (d >= 54 && d < R) CHECK(top(x, z) == F);
            for (int y = F + 1; y <= F + 14 && d >= 50; ++y) {  // (the targets' legs and centres are well inside)
                if (at(x, y, z) == Wood && at(x, y - 1, z) != Wood) {
                    CHECK(y == F + 8 && d >= R);
                    poles += y == F + 8;
                }
                if (at(x, y, z) == Hood) CHECK(d >= R && d < R + 2);
            }
        }
    CHECK(poles == 8);
    // The gates: iron bars in the wall's face, the passage behind open.
    CHECK(at(0, F + 1, -57) == Iron && at(1, F + 1, -57) == Air && at(1, F + 1, -58) == Air && at(-3, F + 1, -57) == Brick);
    for (int x = -2; x <= 2; ++x) CHECK(at(x, F + 2, -57) == Iron);  // a cross bar: the gaps below are too low to walk in
    CHECK(at(0, F + 1, 56) == Iron && at(1, F + 1, 56) == Air && at(0, F + 1, 57) == Air);
    // The Hill: the plateau 5 up, the ramp a block every two, the stairs a block every one, a cliff.
    CHECK(top(0, 40) == F + 5 && at(0, F + 5, 40) == Grass && top(0, 24) == F + 1 && top(0, 26) == F + 2 && top(0, 32) == F + 5);
    CHECK(top(11, 40) == F + 4 && top(14, 40) == F + 1 && top(-11, 40) == F);
    // The Pool: a shallow end a block deep, a deep end four deep, a bridge a block above the water.
    CHECK(at(-28, F, -6) == Water && top(-28, -6) == F - 1 && at(-28, F - 1, -6) == Sand);
    CHECK(at(-40, F - 3, -6) == Water && top(-40, -6) == F - 4);
    CHECK(top(-40, 0) == F + 1 && at(-40, F + 1, 0) == DarkPlanks && at(-38, F - 2, 0) == Wood);
    // The Pillars: a wall three high, a kerb one high, columns 7 high.
    CHECK(top(29, 0) == F + 3 && top(42, 0) == F + 1 && top(34, 0) == F + 7);
    // The Narrows: the corridor clear through between its walls.
    for (int z = 19; z <= 35; ++z) CHECK(at(-28, F + 1, z) == Air && at(-27, F + 4, z) == Air);
    for (int x = -28; x <= -18; ++x) CHECK(at(x, F + 2, 34) == Air);
    CHECK(top(-29, 25) == F + 4 && top(-26, 25) == F + 4 && top(-22, 33) == F + 4 && top(-22, 36) == F + 4 && top(-23, 25) == F);
    CHECK(top(-28, 18) == F && top(-17, 34) == F);  // the way in and the way out
    // The Rough: bumps of 1 and 2 among the flat, never more.
    int bumps[3] = {};
    for (int z = 18; z <= 34; ++z)
        for (int x = 18; x <= 34; ++x) {
            CHECK(top(x, z) >= F && top(x, z) <= F + 2);
            ++bumps[std::clamp(top(x, z) - F, 0, 2)];
        }
    CHECK(bumps[0] > 30 && bumps[1] > 30 && bumps[2] > 30);
    // The start: the player spawns there, can rest by the brazier, three dummies stand by.
    startSession(s);
    const glm::vec3 start = s.player.pos;
    CHECK(std::abs(start.x - float(mid.x) - 0.5f) < 0.01f && std::abs(start.z - float(mid.y) + 39.5f) < 0.01f && start.y == float(F + 1));
    s.player.hitTime = 99;
    CHECK(canRest(s));
    CHECK(std::ranges::count(s.entities, EntityKind::TrainingDummy, &Entity::kind) == 3);
    // The world file round trip keeps it all.
    std::string path = (fs::temp_directory_path() / "flycoos_arena_test.flyw").string();
    CHECK(saveWorld(path.c_str(), g, 1337, instances, s.markers).empty());
    Grid back({192, 96, 192});
    uint32_t seed = 0;
    std::vector<Instance> list;
    std::optional<std::vector<Marker>> marks;
    CHECK(loadWorld(path.c_str(), back, seed, list, marks).empty());
    CHECK(back.cells == g.cells && list.size() == instances.size() && seed == 1337);
    CHECK(marks && *marks == s.markers);
    fs::remove(path);

    // Spawning groups: around a point from the middle, standing on the ground, apart, facing the player.
    std::vector<Entity> group;
    glm::vec2 m(mid);
    CHECK(spawnGroup(group, g, EntityKind::Wolf, 3, {m.x, F + 1, m.y}, start, 5) == 3);
    for (size_t i = 0; i < group.size(); ++i) {
        CHECK(group[i].kind == EntityKind::Wolf && group[i].spawn.id == 0 && group[i].body.pos.y == float(F + 1));
        for (size_t j = 0; j < i; ++j) CHECK(glm::distance(group[i].body.pos, group[j].body.pos) > 1.5f);
        CHECK(glm::distance(glm::vec2(group[i].body.pos.x, group[i].body.pos.z), m) < 3);
        glm::vec2 toPlayer = glm::normalize(glm::vec2(start.x, start.z) - glm::vec2(group[i].body.pos.x, group[i].body.pos.z));
        CHECK(glm::dot(toPlayer, glm::vec2(std::sin(group[i].body.yaw), std::cos(group[i].body.yaw))) > 0.99f);
    }
    CHECK(glm::vec2(group[0].body.pos.x, group[0].body.pos.z) == m);  // the first right on the point
    CHECK(spawnGroup(group, g, EntityKind::Bandit, 16, {m.x, F + 6, m.y + 40}, start, 6) == 16 && group.size() == 19);
    for (size_t i = 3; i < group.size(); ++i) CHECK(group[i].kind == EntityKind::Bandit && group[i].body.pos.y == float(F + 6));  // on the hill
    std::set<float> phases;  // each its own seed (its breathing, its rolls): not all the same bandit
    for (size_t i = 3; i < group.size(); ++i) phases.insert(group[i].body.breathPhase);
    CHECK(phases.size() == 16);
    Grid empty({64, 32, 64});  // no ground (the endless world's unloaded columns): none
    CHECK(spawnGroup(group, empty, EntityKind::Wolf, 4, {32, 10, 32}, start, 5) == 0 && group.size() == 19);
    float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(spawnGroup(group, g, EntityKind::Wolf, 4, {nan, F + 1, m.y}, start, 5) == 0 && spawnGroup(group, g, EntityKind::Wolf, 4, {1e10f, F + 1, 0}, start, 5) == 0);
    CHECK(spawnGroup(group, g, EntityKind::Wolf, 0, {m.x, F + 1, m.y}, start, 5) == 0 && spawnGroup(group, g, EntityKind::Wolf, -3, {m.x, F + 1, m.y}, start, 5) == 0);
    {  // Under a roof (a canopy, a house): on the ground at the height asked for, not on top of it; a
       // height far from any ground: on the column's top. At most kMaxSpawnedNpcs.
        std::vector<Entity> under;
        for (int z = -26; z <= -14; ++z)
            for (int x = -6; x <= 6; ++x) g.set({mid.x + x, F + 6, mid.y + z}, Leaves);
        CHECK(spawnGroup(under, g, EntityKind::Bandit, 6, {m.x, F + 1, m.y - 20}, start, 8) == 6);
        for (const Entity& e : under) CHECK(e.body.pos.y == float(F + 1));
        under.clear();
        CHECK(spawnGroup(under, g, EntityKind::Bandit, 6, {m.x, F + 7, m.y - 20}, start, 8) == 6);
        for (const Entity& e : under) CHECK(e.body.pos.y == float(F + 7));  // asked on the canopy: on it
        under.clear();
        CHECK(spawnGroup(under, g, EntityKind::Bandit, 2, {m.x, F + 30, m.y}, start, 8) == 2 && under[0].body.pos.y == float(F + 1));
        under.clear();
        CHECK(spawnGroup(under, g, EntityKind::Bandit, 100000, {m.x, F + 1, m.y}, start, 8) == kMaxSpawnedNpcs);
        for (int z = -26; z <= -14; ++z)
            for (int x = -6; x <= 6; ++x) g.set({mid.x + x, F + 6, mid.y + z}, Air);
    }
    // Deterministic: the same seed, the same NPCs in the same places.
    std::vector<Entity> again;
    spawnGroup(again, g, EntityKind::Wolf, 3, {m.x, F + 1, m.y}, start, 5);
    for (size_t i = 0; i < again.size(); ++i) CHECK(again[i].body.breathPhase == group[i].body.breathPhase && again[i].body.pos == group[i].body.pos);
}

static void throwables() {
    const Defs& d = defs();
    const ItemId knife = d.itemById.at("throwing_knife"), bomb = d.itemById.at("clay_bomb"), pot = d.itemById.at("poison_pot"),
                 bow = d.itemById.at("short_bow");
    const ItemType &k = itemType(knife), &b = itemType(bomb), &o = itemType(pot);
    CHECK(throwable(k) && throwable(b) && throwable(o) && !throwable(itemType(Shortsword)) && !throwable(itemType(bow)));
    CHECK(k.thrown.look == ThrowLook::Knife && k.thrown.radius == 0 && !k.thrown.lob && k.damage == DamageType::Pierce && k.thrown.buildup[0] > 0);
    CHECK(b.thrown.look == ThrowLook::Bomb && b.thrown.radius > 0 && b.thrown.lob && b.damage == DamageType::Strike && b.thrown.stance > k.thrown.stance);
    CHECK(o.thrown.look == ThrowLook::Pot && o.thrown.radius > 0 && o.thrown.buildup[size_t(Status::Poison)] >= kBaseStatusResist);
    CHECK(k.thrown.pouch > b.thrown.pouch && k.slot == SlotNone && b.weight == 0 && k.thrown.speed > b.thrown.speed);
    CHECK(codeMove(Move::Throw) && spec(WeaponClass::Greatsword, Move::Throw).seconds == kThrowSpec.seconds && kThrowSpec.stamina > 0);
    CHECK(throwableModel(knife) == ModelThrowingKnife && throwableModel(bomb) == ModelBomb && throwableModel(pot) == ModelPoisonPot);
    CHECK(burstModel(bomb) == ModelBurst && burstModel(pot) == ModelPoisonBurst);

    // A field: ground top at y 4, the player at x 8.5 facing +x with a sword, a full pouch, the knife readied.
    auto field = [&] {
        Session s{.world = Grid({128, 32, 32}), .seed = 5};
        for (int z = 0; z < 32; ++z)
            for (int x = 0; x < 128; ++x)
                for (int y = 0; y < 4; ++y) s.world.set({x, y, z}, Stone);
        startSession(s);
        s.player.teleport({8.5f, 4, 16.5f});
        s.player.yaw = std::numbers::pi_v<float> / 2;
        s.player.inv.equipped[SlotMainHand] = Shortsword;
        for (int i = 0; i < 10; ++i) step(s, {}, kTick);
        s.sounds.clear();
        return s;
    };
    auto until = [](Session& s, const std::function<bool()>& done, int most = 300, SimInput in = {}) {
        for (int i = 1; i <= most; ++i) {
            step(s, in, kTick);
            if (done()) return i;
        }
        return most + 1;
    };
    SimInput toss{.toss = true};

    {  // A new game: a full pouch of each, the knife (the first) readied.
        Session s = field();
        Player& p = s.player;
        CHECK(p.pouch[knife] == k.thrown.pouch && p.pouch[bomb] == b.thrown.pouch && p.pouch[pot] == o.thrown.pouch && p.readied == knife);
        // A press throws: the move, its stamina, the whoosh; at its impact frame one knife out of the pouch, along the facing.
        step(s, toss, kTick);
        CHECK(p.move == Move::Throw && p.throwing == knife && p.stamina < gMelee.staminaMax && p.moveScale == 1);
        int out = until(s, [&] { return !s.projectiles.empty(); }, 60);
        CHECK(std::abs(float(out + 1) * kTick - kThrowSpec.impact * kThrowSpec.seconds) < 2.5f * kTick);
        const Projectile& a = s.projectiles[0];
        CHECK(a.weapon == knife && a.owner == 0 && std::abs(glm::length(a.vel) - k.thrown.speed) < 0.01f && a.vel.x > 0.95f * glm::length(a.vel));
        CHECK(a.stance == k.thrown.stance && p.pouch[knife] == k.thrown.pouch - 1 && p.throwing == NoItem && p.pouch[bomb] == b.thrown.pouch);
        CHECK(std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Swing && h.kind < 0 && h.move == Move::Throw; }));
        // It sticks where it lands; walked over, it's back in the pouch (and not past a full one).
        until(s, [&] { return s.projectiles[0].stuck; }, 120);
        glm::vec3 at = s.projectiles[0].pos;
        CHECK(at.x > 12 && std::abs(at.y - 4) < 0.6f);
        until(s, [&] { return !p.punching(); }, 60);
        p.teleport({at.x, 4, at.z});
        step(s, {}, kTick);
        CHECK(s.projectiles.empty() && p.pouch[knife] == k.thrown.pouch && p.arrows == kQuiver);
        Session t = field();
        t.player.pouch[knife] = uint8_t(k.thrown.pouch - 1);
        t.projectiles.push_back({{9, 4.2f, 16.5f}, {9, 4.2f, 16.5f}, {1, 0, 0}, knife, 1, 0, 0, true});
        step(t, {}, kTick);
        CHECK(t.projectiles.empty() && t.player.pouch[knife] == k.thrown.pouch);
        t.projectiles.push_back({{9, 4.2f, 16.5f}, {9, 4.2f, 16.5f}, {1, 0, 0}, knife, 1, 0, 0, true});
        step(t, {}, kTick);
        CHECK(t.projectiles.size() == 1 && t.player.pouch[knife] == k.thrown.pouch);  // full: left there
        // An arrow walked over still goes to the quiver, not the pouch.
        t.projectiles.clear(), t.player.arrows = 3;
        t.projectiles.push_back({{9, 4.2f, 16.5f}, {9, 4.2f, 16.5f}, {1, 0, 0}, bow, 1, 0, 0, true});
        step(t, {}, kTick);
        CHECK(t.projectiles.empty() && t.player.arrows == 4);
    }
    {  // Nothing left of the readied kind, or a bow in hand (both hands full): the press does nothing.
        Session s = field();
        Player& p = s.player;
        p.pouch[knife] = 0;
        for (int i = 0; i < 30; ++i) step(s, i ? SimInput{} : toss, kTick);
        CHECK(p.move == Move::None && s.projectiles.empty() && p.want == Want::None);
        p.pouch[knife] = 2, p.inv.equipped[SlotMainHand] = bow;
        for (int i = 0; i < 30; ++i) step(s, i ? SimInput{} : toss, kTick);
        CHECK(p.move == Move::None && s.projectiles.empty() && p.pouch[knife] == 2);
        // With no stamina it waits (the buffer) and goes once there's some.
        p.inv.equipped[SlotMainHand] = Shortsword, p.stamina = 0, p.staminaWait = 0.05f;
        step(s, toss, kTick);
        CHECK(p.move == Move::None && p.want == Want::Throw);
        CHECK(until(s, [&] { return p.move == Move::Throw; }, 30) <= 30);
        // A flinch before it lets go keeps it in the pouch.
        Session t = field();
        step(t, toss, kTick);
        step(t, {}, kTick);
        interrupt(t.player, 0.3f, false);
        for (int i = 0; i < 60; ++i) step(t, {}, kTick);
        CHECK(t.projectiles.empty() && t.player.pouch[knife] == k.thrown.pouch && t.player.move == Move::None);
        // It walks slowly while throwing, and turns toward the aim until it lets go.
        Session u = field();
        SimInput walk;
        walk.move.move = {0, 1};
        step(u, toss, kTick);
        for (int i = 0; i < 12; ++i) step(u, walk, kTick);
        float walked = glm::length(glm::vec2(u.player.vel.x, u.player.vel.z));
        CHECK(u.player.move == Move::Throw && walked > 0.5f && walked <= gMelee.drinkSpeed + 0.01f);
        // A press with nothing to throw, or a bow in hand, changes nothing: a draw goes on, a recovery isn't cut.
        Session n = field();
        n.player.inv.equipped[SlotMainHand] = bow;
        step(n, {.attack = true, .attackHeld = true}, kTick);
        for (int i = 0; i < 20; ++i) step(n, {.attackHeld = true}, kTick);
        step(n, {.attackHeld = true, .toss = true}, kTick);
        CHECK(n.player.move == Move::Shot && n.player.hitPending && n.player.want == Want::None);
        Session z = field();
        z.player.pouch[knife] = 0;
        step(z, {.attack = true}, kTick);
        until(z, [&] { return moveProgress(z.player) > spec(z.player).cancel + 0.02f; }, 120);
        step(z, toss, kTick);
        CHECK(z.player.move == Move::Light1);
        // A throw cuts an attack's recovery (like the flask), and a great weapon's slowness doesn't slow it.
        Session w = field();
        step(w, {.attack = true}, kTick);
        until(w, [&] { return w.player.move == Move::Light1 && moveProgress(w.player) > spec(w.player).cancel + 0.02f; }, 120);
        step(w, toss, kTick);
        CHECK(w.player.move == Move::Throw);
        Session g = field();
        g.player.inv.equipped[SlotMainHand] = d.itemById.at("iron_greatsword");
        step(g, toss, kTick);
        CHECK(g.player.move == Move::Throw && g.player.moveScale == 1);
        // A bandit up close answers a throw's windup as an attack.
        Session h = field();
        Entity& e = spawnAhead(h.entities, h.world, h.player, EntityKind::Bandit, 3, 2.5f);
        setState(e, AiState::Chase, "test");
        e.body.swingTimer = 5;
        step(h, toss, kTick);
        bool answered = false;
        for (int i = 0; i < 20 && h.player.move == Move::Throw; ++i) step(h, {}, kTick), answered |= e.picked;
        CHECK(answered);
        Session v = field();
        SimInput turn{.aim = {0, 1}};
        step(v, toss, kTick);  // thrown facing +x, then aimed at +z: it turns before it lets go
        for (int i = 0; i < 12; ++i) step(v, turn, kTick);
        CHECK(v.player.move == Move::Throw && v.player.hitPending && std::abs(v.player.yaw) < 0.5f);  // (facing +z: yaw 0)
        // What changed while the press waited counts: nothing left, or a bow in hand by then, and it isn't thrown.
        for (int k = 0; k < 2; ++k) {
            Session b = field();
            step(b, {.attack = true}, kTick);
            step(b, toss, kTick);  // buffered behind the swing's windup
            CHECK(b.player.move == Move::Light1 && b.player.want == Want::Throw);
            if (k == 0) b.player.pouch[knife] = 0;
            else b.player.inv.equipped[SlotMainHand] = bow;
            for (int i = 0; i < 60; ++i) step(b, {}, kTick);
            CHECK(b.player.move != Move::Throw && b.projectiles.empty() && b.player.want == Want::None);
        }
    }
    {  // The next kind: knife, bomb, pot, round again; ones with none left skipped, unless none has any.
        Player p;
        fillPouch(p);
        CHECK(p.readied == knife);
        cycleThrowable(p);
        CHECK(p.readied == bomb);
        cycleThrowable(p);
        CHECK(p.readied == pot);
        cycleThrowable(p);
        CHECK(p.readied == knife);
        p.pouch[bomb] = 0;
        cycleThrowable(p);
        CHECK(p.readied == pot);
        p.pouch = {};
        cycleThrowable(p);
        CHECK(p.readied == knife);
        // A rest fills the pouch, as does getting back up; never lowers one over (a debug fill).
        p.pouch[bomb] = 1, p.pouch[knife] = 40;
        rest(p);
        CHECK(p.pouch[bomb] == b.thrown.pouch && p.pouch[pot] == o.thrown.pouch && p.pouch[knife] == 40);
        Session s = field();
        s.player.pouch = {};
        s.player.health = 0;
        respawnPlayer(s);
        CHECK(s.player.pouch[knife] == k.thrown.pouch && s.player.pouch[pot] == o.thrown.pouch);
    }
    {  // A knife into a bandit: the hit, and its own bleed build-up.
        Session s = field();
        s.rng.seed(7);
        Entity& e = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 3, 6);
        float before = e.body.health;
        s.player.throwing = knife;
        hurl(s, std::nullopt);
        for (int i = 0; i < 60 && !s.projectiles.empty(); ++i) updateProjectiles(s, kTick);
        CHECK(s.projectiles.empty() && e.body.health < before && e.body.buildup[size_t(Status::Bleed)] == k.thrown.buildup[0]);
        // Its build-up is its own, not the weapon in hand's (a Rusty Knife's bleed): a knife the same, a pot no bleed.
        for (ItemId what : {knife, pot}) {
            Session r = field();
            r.player.inv.equipped[SlotMainHand] = Knife;
            Entity& t = spawnAhead(r.entities, r.world, r.player, EntityKind::Bandit, 3, 6);
            r.player.throwing = what;
            hurl(r, lockPoint(t));
            for (int i = 0; i < 120 && !r.projectiles.empty(); ++i) updateProjectiles(r, kTick);
            CHECK(t.body.buildup[size_t(Status::Bleed)] == (what == knife ? k.thrown.buildup[0] : 0) && t.body.hitTime < 1);
        }
        CHECK(e.body.hitWith == knife && s.player.swingWeapon == NoItem);
        // A bomb or pot thrown at nothing goes up (lobPitch) and bursts where it comes down, short of any stuck one.
        Session t = field();
        t.player.throwing = bomb;
        hurl(t, std::nullopt);
        CHECK(t.projectiles.size() == 1 && t.projectiles[0].vel.y > 0.3f * b.thrown.speed && t.player.pouch[bomb] == b.thrown.pouch - 1);
        for (int i = 0; i < 300 && !t.projectiles.empty(); ++i) updateProjectiles(t, kTick);
        CHECK(t.projectiles.empty() && t.bursts.size() == 1 && t.bursts[0].item == bomb && t.bursts[0].radius == b.thrown.radius);
        CHECK(t.bursts[0].pos.x > 15 && t.bursts[0].pos.y > 3.9f && t.bursts[0].pos.y < 4.2f);
        CHECK(std::ranges::any_of(t.sounds, [&](const Sound& h) { return h.what == Noise::Thunk && h.with == bomb; }));
        for (int i = 0; i < int(gRanged.burstSeconds / kTick) + 2; ++i) updateProjectiles(t, kTick);
        CHECK(t.bursts.empty());  // the puff is gone
        // Locked on (aimAt), it comes down on the target.
        Session u = field();
        u.player.throwing = pot;
        glm::vec3 target{20.5f, 4.1f, 18.5f};
        hurl(u, target);
        for (int i = 0; i < 300 && !u.projectiles.empty(); ++i) updateProjectiles(u, kTick);
        CHECK(u.bursts.size() == 1 && glm::distance(u.bursts[0].pos, target) < 0.8f);
    }
    {  // A burst strikes every bandit within its radius (less at the edge), none past it or behind a wall, never the player.
        auto blast = [&](uint32_t seed, std::vector<std::pair<EntityKind, float>> where, bool wall, ItemId what) {
            Session s = field();
            s.rng.seed(seed);
            glm::vec3 at{20.5f, 4.05f, 16.5f};
            for (auto [kind, dx] : where) spawnAhead(s.entities, s.world, s.player, kind, 3, 5).body.teleport({at.x + dx, 4, at.z});
            if (wall)
                for (int y = 4; y < 8; ++y)
                    for (int z = 10; z < 23; ++z) s.world.set({21, y, z}, Stone);
            s.player.teleport({at.x, 4, at.z});
            float hp = s.player.health;
            burst(s, at, what, at - glm::vec3(3, 0, 0));
            CHECK(s.player.health == hp && s.bursts.size() == 1 && s.bursts[0].pos == at);
            CHECK((s.player.trauma > 0.3f) == (what == bomb));  // a bomb going off at your feet shakes the camera; a pot doesn't
            return s;
        };
        auto lost = [](const Entity& e) { return float(maxHealth(sheet(e.body))) - e.body.health; };
        float middle = 0, edge = 0, past = 0, walled = 0;
        for (uint32_t seed = 1; seed <= 12; ++seed) {
            Session s = blast(seed, {{EntityKind::Bandit, 0}, {EntityKind::Bandit, 2.2f}, {EntityKind::Bandit, 3.5f}}, false, bomb);
            middle += lost(s.entities[0]), edge += lost(s.entities[1]), past += lost(s.entities[2]);
            CHECK(s.entities[0].body.hitWith == bomb && s.player.swingWeapon == NoItem);
            walled += lost(blast(seed, {{EntityKind::Bandit, 2.2f}}, true, bomb).entities[0]);
        }
        CHECK(edge > 0 && middle > 1.3f * edge && past == 0 && walled == 0);
        std::println("level band: a clay bomb into bandits: {:.1f} in the middle, {:.1f} near its edge (the starting stats)", middle / 12, edge / 12);
        // A poison pot poisons a bandit at its middle; a ghoul shrugs it off (its poison resist).
        Session s = blast(3, {{EntityKind::Bandit, 0}, {EntityKind::Ghoul, 0.5f}}, false, pot);
        CHECK(s.entities[0].body.effect(Effect::Poisoned) && !s.entities[1].body.effect(Effect::Poisoned) && s.entities[1].body.buildup[1] > 0);
        CHECK(lost(s.entities[0]) > 0);
        // A dodge's i-frames let a burst through.
        Session r = field();
        Entity& e = spawnAhead(r.entities, r.world, r.player, EntityKind::Bandit, 3, 5);
        e.body.rollTime = gMelee.iframeFrom + 0.01f, e.body.rollLength = 1;
        CHECK(invulnerable(e.body));
        float hp = e.body.health;
        burst(r, e.body.pos + glm::vec3(0, 0.5f, 0), bomb, r.player.pos);
        CHECK(e.body.health == hp);
        // The dead are left alone; one it kills is a kill (the quest event).
        Session q = field();
        Entity& dead = spawnAhead(q.entities, q.world, q.player, EntityKind::Bandit, 3, 5);
        Entity& weak = spawnAhead(q.entities, q.world, q.player, EntityKind::Bandit, 4, 5);
        Entity& d0 = q.entities[0];
        d0.body.health = 0, weak.body.health = 1;
        (void)dead;
        q.events.clear();
        burst(q, q.entities[1].body.pos + glm::vec3(0, 0.5f, 0), bomb, q.player.pos);
        CHECK(q.entities[0].body.health == 0 && q.entities[0].body.hitWith == NoItem && !q.entities[1].body.alive());
        CHECK(std::ranges::count_if(q.events, [](const GameEvent& ev) { return std::holds_alternative<Killed>(ev); }) == 1);
    }
    {  // The models: a knife in the arrow's grid, tipped where an arrow is; a lit fuse, a green stopper; puffs.
        auto count = [](Model m) {
            Grid g = buildPart(m, Head);
            int n = 0;
            for (int z = 0; z < g.size.z; ++z)
                for (int y = 0; y < g.size.y; ++y)
                    for (int x = 0; x < g.size.x; ++x) n += g.get({x, y, z}) != Air;
            return n;
        };
        Grid kn = buildPart(ModelThrowingKnife, Head), bm = buildPart(ModelBomb, Head), pt = buildPart(ModelPoisonPot, Head);
        CHECK(kn.size == buildPart(ModelArrow, Head).size && kn.get({1, 9, 0}) == Steel && kn.get({1, 0, 0}) == Leather);
        CHECK(bm.get({2, 7, 2}) == Ember && bm.get({2, 2, 2}) == Slate && pt.get({2, 6, 2}) == OakLeaves3 && pt.get({0, 2, 2}) == Blade);
        CHECK(count(ModelBurst) > 400 && count(ModelPoisonBurst) > 400 && buildPart(ModelBurst, Torso).size == glm::ivec3(1));
        // The puff swells most of the way at once, is full by 70% of its time, gone at its end.
        auto across = [](float u) { return glm::length(glm::vec3(burstPose({0, 0, 0}, 2, u) * glm::vec4(1, 0, 0, 0))) * 12; };
        CHECK(across(0) > 0.6f * 4 && std::abs(across(0.7f) - 4) < 1e-3f && across(1) < 1e-3f);
    }
    {  // The pose: drawn back up behind the shoulder by the hold point, the throwable in the left hand; out in front at the release.
        Session s = field();
        Player& p = s.player;
        auto at = [](const glm::mat4& m, glm::vec3 v) { return glm::vec3(m * glm::vec4(v, 1)); };
        auto hand = [&](const std::array<glm::mat4, PartCount>& parts) { return at(parts[HandL], glm::vec3(1.5f)); };
        glm::vec3 rest = hand(poseModel(p, ModelPlayer));
        p.readied = bomb;
        step(s, toss, kTick);
        until(s, [&] { return p.moveTime >= 0.5f * holdTime(p); }, 60);
        CHECK(hand(poseModel(p, ModelPlayer)).y > rest.y + 0.3f);  // on its way up by halfway to the hold point
        until(s, [&] { return p.moveTime >= holdTime(p); }, 60);
        glm::mat4 held(0), inHand(0);
        auto parts = poseModel(p, ModelPlayer, &held, 1, nullptr, &inHand);
        glm::vec3 back = hand(parts);
        CHECK(back.x < rest.x - 0.3f && back.y > rest.y + 0.7f && inHand != glm::mat4(0));  // facing +x: behind is -x
        CHECK(glm::length(at(inHand, {2.5f, 2.5f, 2.5f}) - back) < 0.25f);  // the bomb's middle at the hand
        until(s, [&] { return !p.hitPending; }, 60);
        inHand = glm::mat4(0);
        parts = poseModel(p, ModelPlayer, &held, 1, nullptr, &inHand);
        CHECK(hand(parts).x > rest.x + 0.5f && inHand == glm::mat4(0) && s.projectiles.size() == 1);  // let go: out in front, gone from the hand
        until(s, [&] { return !p.punching(); }, 60);
        CHECK(glm::length(hand(poseModel(p, ModelPlayer)) - rest) < 0.05f);
    }
}

// Lasting deaths (ADR-056): nothing comes back. The dummy can't die (its hits show, its health stays), a villager
// runs from a hit, corpses with nothing on them go once the player is far and can't see them, the player save keeps
// who died, and a quest whose needed NPC dies fails, started or not.
// Hit effects (ADR-058): what the sim tells them, what each hit throws out, how bits fly, land and go, the
// cap, the blade's smear, and that none of it changes the sim.
static void hitEffects() {
    const int bandit = int(EntityKind::Bandit), dummy = int(EntityKind::TrainingDummy), ghoul = int(EntityKind::Ghoul);
    CHECK(typeOf(EntityKind::Bandit).blood == Blood::Red && typeOf(EntityKind::TrainingDummy).blood == Blood::Splinters &&
          typeOf(EntityKind::Ghoul).blood == Blood::Dust && typeOf(EntityKind::Wolf).blood == Blood::Red);
    {  // where a blow came from reaches the sounds; plate is known; a guard's noises come from straight ahead
        Session s{.world = meadow(), .seed = 3};
        s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}}};
        startSession(s);
        for (int i = 0; i < 30; ++i) step(s, {}, kTick);
        s.entities.reserve(8);  // the references below stay put while more spawn
        Entity& b = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 1, 1.5f);
        Strike st{1, 10, s.player.pos + glm::vec3(0.25f, 0, 0)};
        st.canCrit = false;
        s.sounds.clear();
        strike(s.player, b, st, s.rng);
        CHECK(b.body.hitFrom == st.from);
        step(s, {}, kTick);
        auto hurt = std::ranges::find_if(s.sounds, [&](const Sound& h) { return h.what == Noise::Hurt && h.kind == bandit; });
        CHECK(hurt != s.sounds.end() && hurt->from == st.from && !hurt->plated);
        Entity& guard = spawnAhead(s.entities, s.world, s.player, EntityKind::Guard, 3, 1.5f);
        s.sounds.clear();
        strike(s.player, guard, st, s.rng);
        step(s, {}, kTick);
        CHECK(std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Hurt && h.kind == int(EntityKind::Guard) && h.plated; }));
        strikePlayer(b, s.player, {1, 5, b.body.pos + glm::vec3(0, 0, 0.1f)}, s.rng);
        CHECK(s.player.hitFrom == b.body.pos + glm::vec3(0, 0, 0.1f));
        CHECK(plated(ModelGuard, {}) && !plated(ModelBandit, {}) && !plated(ModelPlayer, {}));
        Inventory worn;
        worn.equipped[SlotChest] = IronCuirass;
        CHECK(plated(ModelPlayer, worn));
        s.sounds.clear();
        s.player.noise(Noise::Block);
        step(s, {}, kTick);
        auto block = std::ranges::find(s.sounds, Noise::Block, &Sound::what);
        glm::vec3 ahead(std::sin(s.player.yaw), 0, std::cos(s.player.yaw));
        CHECK(block != s.sounds.end() && glm::distance(block->from - block->pos, ahead) < 1e-4f);
        Entity& never = spawnAhead(s.entities, s.world, s.player, EntityKind::Wolf, 2, 4);  // never struck (a poison death, say)
        s.sounds.clear();
        never.body.noise(Noise::Death);
        step(s, {}, kTick);
        auto death = std::ranges::find(s.sounds, Noise::Death, &Sound::what);
        CHECK(death != s.sounds.end() && death->from == death->pos);  // from itself: no side, not the world's corner
    }
    // What each hit throws out (the eye at the origin, everything near it).
    auto burst = [](const Sound& h, FxSettings set = {}) {
        Fx fx;
        fx.set = set;
        spawnFx(fx, h, {});
        return fx.particles;
    };
    auto glowing = [](const std::vector<Particle>& ps) { return int(std::ranges::count(ps, true, &Particle::glow)); };
    auto red = [](const Particle& p) { return p.colour.r > 3 * p.colour.g && p.colour.r > 3 * p.colour.b; };
    glm::vec3 from(-1, 0, 0);
    Sound hit{.what = Noise::Hurt, .kind = bandit, .with = Shortsword, .amount = 0, .from = from};
    auto light = burst(hit);
    CHECK(light.size() == 6 && glowing(light) == 0 && std::ranges::all_of(light, red));
    hit.amount = 30;
    CHECK(burst(hit).size() == 12);  // 6 to 12 by how heavy the hit was
    hit.amount = 300;
    CHECK(burst(hit).size() == 12);
    glm::vec3 flung(0);
    for (const Particle& p : light) flung += p.vel;
    CHECK(flung.x < 0 && flung.y > 0 && light[0].pos.y > 0.8f && light[0].pos.y < 1.4f && light[0].pos.x < -0.2f);  // out of the struck side, up
    auto farHit = burst({.what = Noise::Hurt, .kind = bandit, .from = {-10, 0, 0}});
    CHECK(std::abs(light[0].pos.x + 0.5f) < 0.1f && std::abs(farHit[0].pos.x + 0.8f) < 0.1f);  // halfway to the hitter, at most 0.8
    CHECK(burst({.what = Noise::Death, .kind = bandit, .from = from}).size() == 22 && burst({.what = Noise::Crit, .kind = bandit, .from = from}).size() == 14);
    CHECK(burst({.what = Noise::Hurt, .from = from}).size() == 6 && std::ranges::all_of(burst({.what = Noise::Hurt, .from = from}), red));  // the player bleeds too
    auto splinters = burst({.what = Noise::Hurt, .kind = dummy, .from = from});
    auto ofBlock = [](Block b) { return [b](const Particle& p) { return p.colour == glm::vec3(kPalette[b][0], kPalette[b][1], kPalette[b][2]); }; };
    CHECK(splinters.size() == 6 && std::ranges::none_of(splinters, red) &&
          std::ranges::all_of(splinters, [&](const Particle& p) { return ofBlock(Wood)(p) || ofBlock(Straw)(p); }));
    auto dust = burst({.what = Noise::Hurt, .kind = ghoul, .from = from});
    CHECK(dust.size() == 6 && std::ranges::none_of(dust, red) && glowing(dust) == 0);
    auto plate = burst({.what = Noise::Hurt, .kind = int(EntityKind::Guard), .from = from, .plated = true});
    CHECK(plate.size() == 9 && glowing(plate) == 3);  // 2-3 sparks off plate, over the blood
    CHECK(glowing(burst({.what = Noise::Crit, .kind = int(EntityKind::Guard), .from = from, .plated = true})) == 0);
    const ItemId buckler = defs().itemById.at("oak_buckler"), heater = defs().itemById.at("heater_shield");
    auto sparks = burst({.what = Noise::Block, .with = Shortsword, .from = from});
    glm::vec3 sprayed(0);
    for (const Particle& p : sparks) sprayed += p.vel;
    CHECK(sparks.size() == 10 && glowing(sparks) == 10 && sprayed.x < 0 && sprayed.y > 0);  // metal on metal: up, a little back toward the blow
    CHECK(glowing(burst({.what = Noise::Block, .with = heater, .from = from})) == 10);
    auto wood = burst({.what = Noise::Block, .with = buckler, .from = from});
    CHECK(wood.size() == 8 && glowing(wood) == 2);  // a buckler's splinters, a spark or two
    auto arms = burst({.what = Noise::Block, .from = from});
    CHECK(arms.size() == 5 && glowing(arms) == 0);
    FxSettings unflashed;  // ADR-060's shards are tested in flashes()
    unflashed.flashes = false;
    CHECK(glowing(burst({.what = Noise::Parry, .from = from}, unflashed)) == 18 && glowing(burst({.what = Noise::PerfectGuard, .from = from})) == 10);
    CHECK(glowing(burst({.what = Noise::Bounce, .from = from})) == 8 && burst({.what = Noise::GuardBreak, .with = buckler, .from = from}).size() == 24);
    auto stone = burst({.what = Noise::Thunk, .ground = Stone, .from = from});
    CHECK(stone.size() == 8 && glowing(stone) == 3 && std::ranges::count_if(stone, ofBlock(Stone)) == 5);
    CHECK(burst({.what = Noise::Thunk, .ground = Dirt, .from = from}).size() == 5 && burst({.what = Noise::Thunk, .from = from}).empty());
    CHECK(burst({.what = Noise::Step}).empty() && burst({.what = Noise::Swing, .with = Shortsword}).empty());
    FxSettings off;
    off.blood = false;
    CHECK(burst(hit, off).empty() && burst({.what = Noise::Hurt, .kind = int(EntityKind::Guard), .from = from, .plated = true}, off).size() == 3);
    off = {}, off.sparks = false;
    CHECK(burst({.what = Noise::Block, .with = Shortsword, .from = from}, off).empty() && burst({.what = Noise::Block, .with = buckler, .from = from}, off).size() == 6);
    off = {}, off.chips = false;
    CHECK(burst({.what = Noise::Block, .from = from}, off).empty() && burst({.what = Noise::Thunk, .ground = Stone, .from = from}, off).size() == 3);
    off = {}, off.amount = 2;
    CHECK(burst(hit, off).size() == 24);
    {  // out of reach of the eye: nothing
        Fx fx;
        Sound far = hit;
        far.pos = {0, 0, fx.set.reach + 1};
        spawnFx(fx, far, {});
        CHECK(fx.particles.empty());
    }
    {  // blood flies, lands on the ground and lies there its while; sparks die where they land, within their life
        Grid g = meadow();  // grass at y 3: the ground's top at 4
        Fx fx;
        fx.set.flashes = false;  // ADR-060's shards are tested in flashes()
        spawnFx(fx, {.what = Noise::Death, .pos = {10, 4, 10}, .kind = bandit, .from = {9, 4, 10}}, {10, 6, 10});
        spawnFx(fx, {.what = Noise::Parry, .pos = {20, 4, 20}, .from = {20, 4, 21}}, {10, 6, 10});
        CHECK(fx.particles.size() == 40);
        Particle first = fx.particles[0];
        updateFx(fx, g, 0);
        CHECK(fx.particles.size() == 40 && fx.particles[0].pos == first.pos && fx.particles[0].age == 0);  // paused
        updateFx(fx, g, kTick);
        CHECK(fx.particles[0].pos != first.pos && fx.particles[0].vel.y < first.vel.y);  // gravity
        for (int i = 0; i < 18; ++i) updateFx(fx, g, kTick);  // 0.32 s
        CHECK(glowing(fx.particles) == 0);
        for (int i = 0; i < 120; ++i) updateFx(fx, g, kTick);
        CHECK(fx.particles.size() == 22 && std::ranges::all_of(fx.particles, [](const Particle& p) {
                  return p.resting && std::abs(p.pos.y - 4.01f) < 1e-4f && p.vel == glm::vec3(0) && p.life == FxSettings{}.bloodSeconds;
              }));
        float oldest = std::ranges::max(fx.particles, {}, &Particle::age).age;
        Particle lying = fx.particles[0];
        CHECK(drawnSize(lying) == lying.size);  // full size until its last quarter
        lying.age = lying.life * 0.875f;
        CHECK(std::abs(drawnSize(lying) - lying.size * 0.5f) < 1e-4f);
        for (int i = 0; i < int((FxSettings{}.bloodSeconds - oldest) * 60) - 2; ++i) updateFx(fx, g, kTick);
        CHECK(fx.particles.size() == 22);
        for (int i = 0; i < 90; ++i) updateFx(fx, g, kTick);
        CHECK(fx.particles.empty());  // faded within its 6 s on the ground
        spawnFx(fx, {.what = Noise::Thunk, .pos = {10, 1.5f, 10}, .ground = Dirt, .from = {9, 1.5f, 10}}, {10, 6, 10});
        CHECK(fx.particles.size() == 5);
        updateFx(fx, g, kTick);
        CHECK(fx.particles.empty());  // born inside the ground: gone, not stuck there
        spawnFx(fx, {.what = Noise::Death, .pos = {10, 8, 10}, .kind = bandit, .from = {9, 8, 10}}, {10, 6, 10});
        glm::vec3 was = fx.particles[0].pos, v = fx.particles[0].vel;
        updateFx(fx, g, 1);  // a stalled frame counts as 0.05 s
        CHECK(glm::distance(fx.particles[0].pos, was) < glm::length(v) * 0.05f + 0.1f);
        fx.set.sparkSeconds = 5;  // long-lived sparks still die where they land
        spawnFx(fx, {.what = Noise::Parry, .pos = {10, 4, 10}, .from = {10, 4, 11}}, {10, 6, 10});
        for (int i = 0; i < 90; ++i) updateFx(fx, g, kTick);
        CHECK(glowing(fx.particles) == 0);
        Particle spark;
        spark.glow = true, spark.life = 0.3f, spark.age = 0.15f;
        CHECK(drawnSize(spark) < spark.size);  // sparks shrink as they go
    }
    {  // the cap: a new bit replaces the oldest; a lower cap drops the rest
        Fx fx;
        fx.set.cap = 50;
        for (int i = 0; i < 6; ++i) spawnFx(fx, {.what = Noise::Hurt, .pos = {float(i), 0, 0}, .kind = bandit, .from = from}, {});
        for (int i = 0; i < 3; ++i) spawnFx(fx, {.what = Noise::Parry, .from = from}, {});
        CHECK(fx.particles.size() == 50 && glowing(fx.particles) == 50);  // 36 drops, then 54 sparks: every drop was older
        fx.set.cap = 10;
        updateFx(fx, meadow(), 0);
        CHECK(fx.particles.size() == 10);
        fx.set.cap = 0;
        spawnFx(fx, hit, {});
        updateFx(fx, meadow(), 0);
        CHECK(fx.particles.empty());
    }
    {  // the smear: a fast blade leaves one, a slow or idle one none; it fades from the blade back, holds in hit-stop
        auto at = [](Move m, float u) {  // u of the way through m
            Player p;
            p.move = m;
            p.moveTime = u * moveLength(p);
            return p;
        };
        const MoveSpec& light = spec(WeaponClass::Fists, Move::Light1);
        CHECK(swingsBlade(at(Move::Light1, light.holdAt)) && swingsBlade(at(Move::Light1, light.impact + kSmearAfter - 0.01f)) && !swingsBlade(Player{}));
        for (Move m : {Move::Parry, Move::Drink, Move::Shot, Move::Reload, Move::Throw})
            for (float u = 0.05f; u < 1; u += 0.05f) CHECK(!swingsBlade(at(m, u)));  // no blade's strike
        CHECK(!swingsBlade(at(Move::Light1, light.holdAt - 0.01f)) && !swingsBlade(at(Move::Light1, light.impact + kSmearAfter + 0.01f)));  // not the windup or the recovery
        CHECK(!swingsBlade([] { Player p; p.move = Move::Parry; return p; }()) && !swingsBlade([] { Player p; p.move = Move::Shot; return p; }()));
        glm::vec3 base, tip;
        CHECK(bladeEdge(ModelSword, base, tip) && !bladeEdge(ModelBow, base, tip) && !bladeEdge(ModelPlayer, base, tip));
        float sword = glm::distance(base, tip), dagger = 0, great = 0;
        CHECK(bladeEdge(ModelDagger, base, tip) && (dagger = glm::distance(base, tip)) < sword);
        CHECK(bladeEdge(ModelGreatsword, base, tip) && (great = glm::distance(base, tip)) > sword);  // sized by the weapon
        Grid g = meadow();
        Fx fx;
        int body = 0;
        auto pose = [](float x) { return glm::scale(glm::translate(glm::mat4(1), {x, 2, 0}), glm::vec3(0.1f)); };
        auto frame = [&](float x, bool swinging = true, bool frozen = false, float dt = kTick) {
            trailFx(fx, &body, ModelSword, pose(x), swinging, frozen);
            updateFx(fx, g, dt);
        };
        frame(0);
        CHECK(fx.trails.size() == 1 && fx.trails[0].samples.empty() && fx.ribbon.empty());
        frame(0.2f, true, false, kTick / 2);  // 12 blocks/s; frames at 120 Hz from here
        CHECK(fx.trails[0].samples.size() == 2 && fx.ribbon.size() == 24);  // from where it was: 4 steps of 2 triangles
        frame(0.2f, true, false, kTick / 2);  // the frame between two ticks: the same edge
        CHECK(fx.trails[0].samples.size() == 2);
        frame(0.4f);
        CHECK(fx.trails[0].samples.size() == 3 && fx.ribbon.size() == 48);
        const RibbonVertex &hilt = fx.ribbon[fx.ribbon.size() - 1], &end = fx.ribbon[fx.ribbon.size() - 2];
        CHECK(end.alpha > 0.7f && hilt.alpha < end.alpha * 0.3f && fx.ribbon[1].alpha < end.alpha - 0.1f);  // brightest at the tip, now
        CHECK(std::abs(end.pos.y - (2 + 1.5f)) < 1e-4f);  // the sword's tip, 15 voxels up the grip
        frame(0.41f);  // 0.6 blocks/s: too slow to add
        CHECK(fx.trails[0].samples.size() == 3);
        std::vector<float> ages;
        for (const TrailSample& s : fx.trails[0].samples) ages.push_back(s.age);
        frame(0.41f, true, true);  // hit-stop: held
        for (size_t i = 0; i < ages.size(); ++i) CHECK(fx.trails[0].samples[i].age == ages[i]);
        for (int i = 0; i < int(FxSettings{}.trailSeconds * 60) + 1; ++i) frame(0.41f);
        CHECK(fx.trails[0].samples.empty() && fx.ribbon.empty());  // faded out
        frame(0.6f, true, false, kTick / 2), frame(0.8f, true, false, kTick / 2);  // a swing, a pause, another while it's still showing
        frame(0.8f, true, false, kTick / 2), frame(0.81f, true, false, kTick / 2);
        frame(1.0f, true, false, kTick / 2);
        CHECK(fx.trails[0].samples.size() == 4 && fx.ribbon.size() == 48);  // two smears, no band across the gap
        frame(1.0f, true, true);  // held in hit-stop, then no longer drawn: it fades and goes
        for (int i = 0; i < 10; ++i) updateFx(fx, g, kTick);
        CHECK(fx.trails.empty());
        frame(0.61f, false);  // fast, but not an attack
        frame(0.81f, false);
        CHECK(fx.trails[0].samples.empty());
        frame(5);  // a jump: another body at this address, not a swing
        CHECK(fx.trails[0].samples.empty());
        fx.set.trails = false;
        frame(5.2f);
        CHECK(fx.trails[0].samples.empty());
        updateFx(fx, g, kTick);  // not drawn this frame and nothing left: forgotten
        CHECK(fx.trails.empty());
        trailFx(fx, &body, ModelBow, pose(0), true, false);
        CHECK(fx.trails.empty());  // a bow has no edge
    }
    {  // feeding the effects never changes the sim: the same fight with and without them
        size_t most = 0, smear = 0;  // the most bits and smear triangles at once
        float glow = 0;              // the strongest charge glow
        auto fight = [&](bool effects, Fx& fx) {
            Session s{.world = meadow(), .seed = 9};
            s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}}};
            startSession(s);
            Entity& b = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 4, 1.8f);
            damage(b, 1);  // it fights back
            for (int i = 0; i < 600; ++i) {
                SimInput in;
                in.heavy = i % 160 == 40, in.heavyHeld = i % 160 >= 40 && i % 160 < 130;  // charged heavies too (ADR-060)
                in.attack = i % 40 == 0 && !in.heavyHeld;
                in.guard = i % 200 > 150;
                step(s, in, kTick);
                if (effects) {
                    for (const Sound& h : s.sounds) spawnFx(fx, h, s.player.pos);
                    for (const Player* p : {&static_cast<const Player&>(s.player), &static_cast<const Player&>(s.entities[0].body)}) {
                        glm::mat4 held(0);
                        poseModel(*p, p == &s.player ? ModelPlayer : ModelBandit, &held);
                        if (held != glm::mat4(0)) trailFx(fx, p, heldModel(*p), held, swingsBlade(*p), p->freeze > 0 && p->punching());
                        chargeFx(fx, *p, heldModel(*p), held, p->pos, kTick);
                        glow = std::max(glow, chargeTint(*p, fx.set).a);
                    }
                    updateFx(fx, s.world, kTick);
                    most = std::max(most, fx.particles.size()), smear = std::max(smear, fx.ribbon.size() / 3);
                }
                s.sounds.clear();
            }
            return s;
        };
        Fx fx, unused;
        Session a = fight(true, fx), b = fight(false, unused);
        CHECK(a.rng == b.rng && a.player.pos == b.player.pos && a.player.health == b.player.health);
        CHECK(a.entities.size() == b.entities.size() && a.entities[0].body.pos == b.entities[0].body.pos &&
              a.entities[0].body.health == b.entities[0].body.health);
        CHECK(most >= 6 && smear > 0 && unused.particles.empty());  // the effects did run
        CHECK(glow >= FxSettings{}.glowFull - 1e-4f);  // a heavy charged in full
    }
}

// ADR-060: a charging heavy's glow and motes, the parry, riposte and crit flashes, and damage numbers by type.
static void flashes() {
    const int bandit = int(EntityKind::Bandit);
    auto glowing = [](const std::vector<Particle>& ps) { return int(std::ranges::count(ps, true, &Particle::glow)); };
    auto near = [](glm::vec3 a, glm::vec3 b) { return glm::distance(a, b) < 1e-4f; };
    glm::vec3 from(-1, 0, 0);
    {  // a hit's number keeps its type and whether a guard let it through; a riposte is marked by its stance
        Session s{.world = meadow(), .seed = 3};
        s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}}};
        startSession(s);
        for (int i = 0; i < 30; ++i) step(s, {}, kTick);
        s.entities.reserve(8);
        Entity& b = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 1, 1.5f);
        b.body.buildup = {}, b.body.innate.push_back({Stat::BleedResist, ModKind::Flat, 1e6f});
        // The newest number (older ones aged first: no tick runs between these hits).
        auto latest = [](const Player& p) { return *std::ranges::min_element(p.numbers, {}, &DamageNumber::age); };
        auto older = [](Player& p) { for (DamageNumber& n : p.numbers) n.age += 0.1f; };
        auto heal = [](Player& p) { p.health = float(maxHealth(sheet(p))); };
        Strike st{1, 10, s.player.pos, DamageType::Pierce};
        st.canCrit = false;
        strike(s.player, b, st, s.rng);
        CHECK(latest(b.body).type == DamageType::Pierce && !latest(b.body).guarded && latest(b.body).age == 0);
        st.type = DamageType::Strike;
        older(b.body), heal(b.body);
        strike(s.player, b, st, s.rng);
        CHECK(latest(b.body).type == DamageType::Strike);
        st.riposte = true;
        older(b.body), heal(b.body), b.body.stagger = 0;
        strike(s.player, b, st, s.rng);
        CHECK(b.body.hitStance == kRiposteStance && latest(b.body).crit);
        s.sounds.clear();
        step(s, {}, kTick);
        CHECK(std::ranges::any_of(s.sounds, [](const Sound& h) { return h.what == Noise::Crit && h.amount == kRiposteStance; }));
        // An NPC's guard (a buckler stops 85% of slashes) lets some through: its number is a guarded one.
        Entity& g = spawnAhead(s.entities, s.world, s.player, EntityKind::Guard, 3, 1.5f);
        g.body.yaw = std::atan2(s.player.pos.x - g.body.pos.x, s.player.pos.z - g.body.pos.z);
        g.body.inv.equipped[SlotOffHand] = defs().itemById.at("oak_buckler");
        g.body.guarding = true, g.body.stamina = gMelee.staminaMax, g.body.buildup = {};
        int before = int(std::ranges::count(g.body.numbers, 0.0f, &DamageNumber::age));
        Strike heavy{8, 10, s.player.pos, DamageType::Slash};
        heavy.canCrit = false;
        strike(s.player, g, heavy, s.rng);
        CHECK(g.body.made(Noise::Block) && std::ranges::count(g.body.numbers, 0.0f, &DamageNumber::age) == before + 1 && latest(g.body).guarded &&
              latest(g.body).type == DamageType::Slash);
        // The player's guard (a sword stops 75%): the hit's number on the player is guarded and of its type.
        Player p = s.player;
        p.inv.equipped[SlotOffHand] = NoItem, p.inv.equipped[SlotMainHand] = Shortsword;
        p.guarding = true, p.guardTime = 1, p.stamina = gMelee.staminaMax, p.buildup = {};
        p.innate.push_back({Stat::BleedResist, ModKind::Flat, 1e6f});
        glm::vec3 ahead = p.pos + glm::vec3(std::sin(p.yaw), 0, std::cos(p.yaw));
        strikePlayer(b, p, {6, 5, ahead, DamageType::Strike}, s.rng);
        CHECK(p.made(Noise::Block) && latest(p).guarded && latest(p).type == DamageType::Strike && latest(p).amount > 0);
        p.guarding = false;
        older(p), heal(p);
        strikePlayer(b, p, {6, 5, ahead, DamageType::Pierce}, s.rng);
        CHECK(!latest(p).guarded && latest(p).type == DamageType::Pierce);
        older(p);
        CHECK(hurt(p, 3, false, Status::Count, DamageType::Strike, true) && latest(p).guarded && latest(p).amount == 3);
        older(p);
        CHECK(hurt(p, 2, false, Status::Poison) && latest(p).kind == Status::Poison && !latest(p).guarded);
    }
    {  // the number's style: who was hit by colour, its type by glyph, a crit orange, a guarded hit steel grey, statuses as before
        glm::vec3 hit(1, 0.85f, 0.3f), status(0.9f, 0.1f, 0.4f);
        auto style = [&](DamageNumber n, NumberSettings set = {}) { return numberStyle(n, hit, status, set); };
        NumberStyle slash = style({.amount = 5}), strikes = style({.amount = 5, .type = DamageType::Strike}), pierce = style({.amount = 5, .type = DamageType::Pierce});
        CHECK(slash.glyph == Glyph::Slash && strikes.glyph == Glyph::Strike && pierce.glyph == Glyph::Pierce);
        CHECK(slash.rgb == hit && strikes.rgb == hit && slash.scale == 1 && !slash.bang);
        NumberStyle crit = style({.amount = 5, .crit = true, .type = DamageType::Pierce});
        CHECK(crit.rgb == kCritRgb && crit.scale == 1.5f && crit.bang && crit.glyph == Glyph::Pierce);
        NumberStyle guarded = style({.amount = 5, .type = DamageType::Strike, .guarded = true});
        CHECK(guarded.rgb == kGuardedRgb && guarded.scale == NumberSettings{}.guardedScale && guarded.glyph == Glyph::Shield && !guarded.bang);
        NumberSettings plain;
        plain.guarded = false;
        CHECK(style({.amount = 5, .type = DamageType::Strike, .guarded = true}, plain).glyph == Glyph::Strike &&
              style({.amount = 5, .guarded = true}, plain).rgb == hit);
        plain = {}, plain.glyphs = false;
        CHECK(style({.amount = 5}, plain).glyph == Glyph::None && style({.amount = 5, .guarded = true}, plain).glyph == Glyph::None &&
              style({.amount = 5, .guarded = true}, plain).rgb == kGuardedRgb);
        NumberStyle bleed = style({.amount = 9, .kind = Status::Bleed}), poison = style({.amount = 1, .kind = Status::Poison}),
                    frost = style({.amount = 9, .kind = Status::Frost, .guarded = true});
        CHECK(bleed.rgb == status && bleed.bang && bleed.scale == 1.3f && bleed.glyph == Glyph::None);
        CHECK(poison.scale == 0.75f && !poison.bang && poison.glyph == Glyph::None && frost.rgb == status && frost.scale == 1.3f);
    }
    int who = 0;  // the body that made a sound: its flash's
    auto spawned = [&](Sound h, FxSettings set = {}) {
        if (h.what != Noise::Hurt) h.body = &who;
        Fx fx;
        fx.set = set;
        spawnFx(fx, h, {});
        return fx;
    };
    {  // a parry: the parrier's guard flashes cold white, 10 big shards over its 18 sparks, a disc
        Fx fx = spawned({.what = Noise::Parry, .kind = -1, .from = from});
        CHECK(glowing(fx.particles) == 28 && std::ranges::count_if(fx.particles, [](const Particle& p) { return p.size > 0.1f && p.life == 0.25f; }) == 10);
        CHECK(fx.flashes.size() == 1 && fx.flashes[0].on == FlashOn::Guard && fx.flashes[0].colour == kParryRgb && fx.flashes[0].strength == 1 &&
              fx.flashes[0].body == &who && fx.discs.size() == 1);
        CHECK(fx.discs[0].pos.y > 1 && fx.discs[0].pos.x < 0);  // at the guard held up toward the blow
        FxSettings off;
        off.flashes = false;
        Fx none = spawned({.what = Noise::Parry, .from = from}, off);
        CHECK(glowing(none.particles) == 18 && none.flashes.empty() && none.discs.empty());
        off = {}, off.disc = false;
        Fx noDisc = spawned({.what = Noise::Parry, .from = from}, off);
        CHECK(noDisc.flashes.size() == 1 && noDisc.discs.empty() && glowing(noDisc.particles) == 28);
        CHECK(spawned({.what = Noise::PerfectGuard, .from = from}).flashes.empty() && spawned({.what = Noise::Block, .with = Shortsword, .from = from}).flashes.empty());
    }
    {  // a riposte or backstab: the body flashes warm in full, 14 shards, a disc; a random crit the body at half, nothing else
        Fx fx = spawned({.what = Noise::Crit, .kind = bandit, .amount = kRiposteStance, .from = from});
        CHECK(fx.flashes.size() == 1 && fx.flashes[0].on == FlashOn::Body && fx.flashes[0].colour == kRiposteRgb && fx.flashes[0].body == &who &&
              fx.flashes[0].strength == FxSettings{}.flashStrength && fx.discs.size() == 1);
        CHECK(glowing(fx.particles) == 14 && fx.particles.size() == 28);  // and the crit's blood
        Fx random = spawned({.what = Noise::Crit, .kind = bandit, .amount = 20, .from = from});
        CHECK(random.flashes.size() == 1 && random.flashes[0].strength == 0.5f * FxSettings{}.flashStrength && random.discs.empty() &&
              glowing(random.particles) == 0);
        Fx onPlayer = spawned({.what = Noise::Crit, .kind = -1, .amount = kRiposteStance, .from = from});  // NPCs don't riposte: a random crit
        CHECK(onPlayer.discs.empty() && onPlayer.flashes.size() == 1 && onPlayer.flashes[0].strength < FxSettings{}.flashStrength);
        CHECK(spawned({.what = Noise::Hurt, .kind = bandit, .amount = kRiposteStance, .from = from}).flashes.empty());
        FxSettings off;
        off.flashes = false;
        Fx none = spawned({.what = Noise::Crit, .kind = bandit, .amount = kRiposteStance, .from = from}, off);
        CHECK(none.flashes.empty() && none.discs.empty() && glowing(none.particles) == 0);
        Fx nobody;  // a sound with no body (the debug window's bursts): no flash, the rest as ever
        spawnFx(nobody, {.what = Noise::Crit, .kind = bandit, .amount = kRiposteStance, .from = from}, {});
        CHECK(nobody.flashes.empty() && nobody.discs.size() == 1 && glowing(nobody.particles) == 14);
    }
    {  // a heavy's full charge: a ring of 12 gold motes rising about the chest; a bow's or crossbow's: none
        Fx fx = spawned({.what = Noise::Charged, .kind = -1, .move = Move::Heavy1});
        CHECK(fx.particles.size() == 12 && glowing(fx.particles) == 12 && std::ranges::all_of(fx.particles, [](const Particle& p) {
                  return p.gravity < 0 && p.life == 0.4f && p.pos.y > 0.8f && p.pos.y < 1.4f && (p.colour == kChargedRgb || p.colour == kChargeRgb);
              }));
        CHECK(spawned({.what = Noise::Charged, .move = Move::Heavy2}).particles.size() == 12);
        CHECK(spawned({.what = Noise::Charged, .move = Move::Shot}).particles.empty() && spawned({.what = Noise::Charged, .move = Move::Light1}).particles.empty());
        FxSettings off;
        off.motes = false;
        CHECK(spawned({.what = Noise::Charged, .move = Move::Heavy1}, off).particles.empty());
        off = {}, off.glow = false;
        CHECK(spawned({.what = Noise::Charged, .move = Move::Heavy1}, off).particles.empty());
    }
    {  // a body's flashes, by what they light, fade as their age squared; the strongest wins
        Fx fx;
        int b = 0, c = 0, d = 0;
        fx.flashes = {{.body = &b, .strength = 0.75f}, {.body = &d, .on = FlashOn::Guard}};
        Flash f = fx.flashes[0];
        CHECK(near(glm::vec3(flashTint(fx, &b, FlashOn::Body)), f.colour * kBright) && flashTint(fx, &b, FlashOn::Body).a == f.strength);
        CHECK(flashTint(fx, &b, FlashOn::Guard).a == 0 && flashTint(fx, &c, FlashOn::Body).a == 0 && flashTint(fx, &d, FlashOn::Guard).a == 1);
        Grid g = meadow();
        updateFx(fx, g, 0);
        CHECK(flashTint(fx, &b, FlashOn::Body).a == f.strength);  // paused
        updateFx(fx, g, f.length / 4), updateFx(fx, g, f.length / 4);  // (a frame counts at most 0.05 s)
        CHECK(std::abs(flashTint(fx, &b, FlashOn::Body).a - f.strength * 0.25f) < 1e-4f);
        fx.flashes.push_back({.body = &b, .strength = 0.6f});
        fx.flashes.push_back({.body = &b, .strength = 0.1f});
        CHECK(flashTint(fx, &b, FlashOn::Body).a == 0.6f);  // the strongest
        for (int i = 0; i < 6; ++i) updateFx(fx, g, f.length / 4);
        CHECK(fx.flashes.empty() && flashTint(fx, &b, FlashOn::Body).a == 0);
        // Through a real fight's sound: a riposte's Crit carries the struck body, whose tint flashes.
        Session s{.world = meadow(), .seed = 3};
        s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}}};
        startSession(s);
        Entity& e = spawnAhead(s.entities, s.world, s.player, EntityKind::Bandit, 1, 1.5f);
        Strike rip{1, 10, s.player.pos};
        rip.riposte = true;
        strike(s.player, e, rip, s.rng);
        s.sounds.clear();
        step(s, {}, kTick);
        Fx real;
        for (const Sound& h : s.sounds) spawnFx(real, h, s.player.pos);
        CHECK(flashTint(real, &s.entities[0].body, FlashOn::Body).a == FxSettings{}.flashStrength && flashTint(real, &s.player, FlashOn::Body).a == 0);
    }
    {  // the disc: a fan facing the eye, white in the middle to clear at the rim, growing as it fades, gone at its time
        Fx fx;
        fx.eye = {0, 1, -10};
        fx.discs = {{{0, 1, 0}, 0}};
        Grid g = meadow();
        updateFx(fx, g, 0);
        CHECK(fx.ribbon.size() == 48 && fx.ribbon[0].alpha > 0.8f && fx.ribbon[1].alpha == 0 && fx.ribbon[2].alpha == 0);
        glm::vec3 centre = fx.ribbon[0].pos;
        CHECK(centre.z < 0 && centre.z > -1);  // pulled toward the eye, out of the bodies
        float r0 = glm::distance(fx.ribbon[1].pos, centre);
        CHECK(std::abs(r0 - 0.6f * FxSettings{}.discSize) < 1e-3f && std::ranges::all_of(fx.ribbon, [&](const RibbonVertex& v) {
                  return std::abs(v.pos.z - centre.z) < 1e-4f && std::abs(glm::distance(v.pos, centre) - (v.alpha > 0 ? 0 : r0)) < 1e-3f;
              }));  // facing the eye, round
        updateFx(fx, g, FxSettings{}.discSeconds / 2);
        CHECK(glm::distance(fx.ribbon[1].pos, fx.ribbon[0].pos) > r0 && fx.ribbon[0].alpha < 0.5f);
        updateFx(fx, g, FxSettings{}.discSeconds);
        CHECK(fx.discs.empty() && fx.ribbon.empty());
    }
    {  // the charge glow: gold rising with the charge, white-gold in full until the hit, fading after or when let go early
        Player p;
        startMove(p, Move::Heavy1, {0, 1});
        FxSettings set;
        CHECK(chargeTint(p, set).a == 0);  // not yet charging
        p.moveTime = holdTime(p), p.holding = true, p.charge = gMelee.chargeSeconds / 2;
        glm::vec4 half = chargeTint(p, set);
        CHECK(near(glm::vec3(half), kChargeRgb) && std::abs(half.a - (set.glowLow + set.glowHigh) / 2) < 1e-4f);
        p.charge = gMelee.chargeSeconds * 0.01f;
        CHECK(std::abs(chargeTint(p, set).a - set.glowLow) < 0.01f);
        p.charge = gMelee.chargeSeconds, p.charged = true, p.holding = false;
        glm::vec4 full = chargeTint(p, set);
        CHECK(near(glm::vec3(full), kChargedRgb * kBright) && full.a == set.glowFull);
        p.moveTime = impactTime(p);
        CHECK(chargeTint(p, set).a == set.glowFull);  // until the hit
        p.moveTime = impactTime(p) + set.glowFade / 2;
        CHECK(std::abs(chargeTint(p, set).a - set.glowFull / 2) < 1e-3f);
        p.moveTime = impactTime(p) + set.glowFade * 1.01f;
        CHECK(chargeTint(p, set).a == 0);
        p.charged = false, p.charge = gMelee.chargeSeconds / 2, p.moveTime = holdTime(p) + set.glowFade / 2;  // let go early
        CHECK(std::abs(chargeTint(p, set).a - half.a / 2) < 1e-3f);
        p.moveTime = holdTime(p) + set.glowFade * 1.01f;
        CHECK(chargeTint(p, set).a == 0);
        p.moveTime = holdTime(p), p.holding = true;
        set.glow = false;
        CHECK(chargeTint(p, set).a == 0);
        set = {};
        Player light;
        startMove(light, Move::Light1, {0, 1});
        light.moveTime = holdTime(light), light.holding = true, light.charge = 0.3f;
        CHECK(chargeTint(light, set).a == 0 && chargeTint(Player{}, set).a == 0);  // a light doesn't charge; nor an idle body
        // Motes rise off the blade while it charges, about moteRate a second at full.
        Fx fx;
        Model sword = heldModel(Shortsword);
        glm::vec3 b0, t0;
        CHECK(bladeEdge(sword, b0, t0));
        p.charged = true, p.moveTime = holdTime(p);
        for (int i = 0; i < 600; ++i) chargeFx(fx, p, sword, glm::mat4(1), {50, 50, 50}, kTick);  // 10 s (motes live 0.35 s: no update, they stay)
        CHECK(std::abs(float(fx.particles.size()) - 10 * fx.set.moteRate) <= 40);
        CHECK(std::ranges::all_of(fx.particles, [&](const Particle& m) {
            return m.glow && m.gravity < 0 && m.life == 0.35f && glm::distance(m.pos, b0) < glm::distance(b0, t0) + 0.3f;  // on the blade, not the hand
        }));
        fx.particles.clear();
        p.charged = false, p.charge = gMelee.chargeSeconds * 0.01f, p.holding = true;  // just started: fewer, by the glow's share
        for (int i = 0; i < 600; ++i) chargeFx(fx, p, sword, glm::mat4(1), {50, 50, 50}, kTick);
        CHECK(std::abs(float(fx.particles.size()) - 10 * fx.set.moteRate * fx.set.glowLow / fx.set.glowFull) <= 40);
        fx.particles.clear();
        p.charged = true;
        for (int i = 0; i < 60; ++i) chargeFx(fx, p, ModelBow, glm::mat4(1), {50, 50, 50}, kTick);  // no blade: from the hand
        CHECK(!fx.particles.empty() && std::ranges::all_of(fx.particles, [](const Particle& m) { return glm::distance(m.pos, glm::vec3(50)) < 0.2f; }));
        fx.particles.clear();
        chargeFx(fx, p, sword, glm::mat4(1), {}, 0);
        for (int i = 0; i < 60; ++i) chargeFx(fx, light, sword, glm::mat4(1), {}, kTick);
        fx.set.motes = false;
        for (int i = 0; i < 60; ++i) chargeFx(fx, p, sword, glm::mat4(1), {}, kTick);
        CHECK(fx.particles.empty());  // paused, a light, motes off
    }
}

static void lastingDeaths() {
    namespace fs = std::filesystem;
    auto flat = [](int size) {  // grass, no water
        Grid g({size, 8, size});
        for (int z = 0; z < size; ++z)
            for (int x = 0; x < size; ++x) {
                for (int y = 0; y < 3; ++y) g.set({x, y, z}, Dirt);
                g.set({x, 3, z}, Grass);
            }
        return g;
    };
    auto wall = [](Grid& g, glm::ivec2 from, glm::ivec2 to) {  // x and z inclusive, 4 high
        for (int z = from.y; z <= to.y; ++z)
            for (int x = from.x; x <= to.x; ++x)
                for (int y = 4; y < 8; ++y) g.set({x, y, z}, Stone);
    };
    auto reason = [](const Entity& e) { return std::string_view(e.history[(e.transitions - 1) % e.history.size()].reason); };
    auto spawn = [](uint32_t id, const char* name, EntityKind kind, glm::vec3 pos, int count = 1) {
        return Marker{.id = id, .kind = MarkerKind::Spawn, .name = name, .pos = pos, .radius = 2, .entity = kind, .count = count};
    };
    std::mt19937 rng(3);

    {  // The dummy: a blow, poison and a bleed all show their numbers; its health never moves, and it's never killed.
        Grid g = flat(64);
        Player p;
        p.pos = {20.5f, 4, 20.5f};
        std::vector<Entity> es;
        Entity& d = spawnAhead(es, g, p, EntityKind::TrainingDummy, 1, 1.5f);
        const float full = float(maxHealth(sheet(d.body)));
        std::vector<GameEvent> events;
        strike(p, d, {.power = 40, .from = p.pos}, rng, kArmorK, &events);
        CHECK(d.body.alive() && d.body.health == full && d.body.lastDamage > 0 && events.empty());
        CHECK(damage(d, int(full) * 3, false, Status::Bleed) && d.body.health == full && d.body.alive());
        CHECK(std::ranges::any_of(d.body.numbers, [&](const DamageNumber& n) { return n.kind == Status::Bleed && n.amount == int(full) * 3; }));
        afflict(d.body, Status::Poison, "test");
        for (int i = 0; i < 3 * 60; ++i) updateCombat(p, es, kTick, rng, kArmorK, &events);
        CHECK(d.body.alive() && d.body.health == full && events.empty() &&
              std::ranges::any_of(d.body.numbers, [](const DamageNumber& n) { return n.kind == Status::Poison && n.amount > 0; }));
    }

    {  // A villager runs from a hit (away from the player, round a wall), a while longer from each new one, then walks home and heals.
        Grid g = flat(96);
        std::vector<Entity> vs;
        syncSpawns(vs, g, {spawn(1, "Wenna", EntityKind::Villager, {40.5f, 4, 40.5f})}, 1, {}, 0.5f);
        Entity& v = vs.at(0);
        Player hero;
        hero.pos = v.body.pos + glm::vec3(2, 0, 0);
        auto run = [&](float seconds) {
            for (int i = 0; i < int(seconds * 60); ++i) updateEntities(vs, g, hero, kTick), updateCombat(hero, vs, kTick, rng);
        };
        float start = glm::distance(v.body.pos, hero.pos);
        CHECK(damage(v, 10) && v.body.health < float(maxHealth(sheet(v.body))));
        run(kTick);
        CHECK(v.state == AiState::Flee && reason(v) == "damaged");
        run(2);
        CHECK(v.state == AiState::Flee && glm::distance(v.body.pos, hero.pos) > start + 6);
        CHECK(damage(v, 10));
        run(kTick);
        CHECK(v.state == AiState::Flee && reason(v) == "damaged" && v.stateTime < 0.1f);  // starts over
        run(kFleeSeconds + 0.1f);
        CHECK(v.state == AiState::Return && reason(v) == "fled");
        CHECK(damage(v, 10));
        run(kTick);
        CHECK(v.state == AiState::Flee && reason(v) == "damaged");  // hit on the way home: off again
        run(kFleeSeconds + 30);
        CHECK(v.state == AiState::Wander && v.body.health == float(maxHealth(sheet(v.body))));  // home, healed
        staggerHit(v, 1000, false);  // a reeling one runs once it's steady
        run(kTick);
        CHECK(v.state == AiState::Stagger);
        for (int i = 0; i < 600 && v.state == AiState::Stagger; ++i) run(kTick);
        CHECK(v.state == AiState::Flee && reason(v) == "steadied");
        // A wall behind it: it goes round, not stuck.
        Grid walled = flat(96);
        wall(walled, {45, 30}, {45, 50});
        std::vector<Entity> ws;
        syncSpawns(ws, walled, {spawn(1, "Wenna", EntityKind::Villager, {43.5f, 4, 40.5f})}, 1, {}, 0.5f);
        hero.pos = ws[0].body.pos + glm::vec3(-2, 0, 0.3f);
        glm::vec3 was = ws[0].body.pos;
        CHECK(damage(ws[0], 10));
        for (int i = 0; i < 2 * 60; ++i) updateEntities(ws, walled, hero, kTick);
        CHECK(ws[0].state == AiState::Flee && glm::distance(ws[0].body.pos, was) > 4);
    }

    {  // buryDead: a corpse with nothing on it goes once the player is gAi.corpseGone off and can't see it.
        Grid g = flat(200);
        std::vector<Entity> es;
        syncSpawns(es, g, {spawn(4, "Den", EntityKind::Boar, {20.5f, 4, 100.5f}, 2)}, 1, {}, 0.5f);
        CHECK(es.size() == 2 && damage(es[0], 10000));
        es[0].loot = NoItem;
        int slot = es[0].slot;
        Player p;
        DeadSlots dead;
        auto bury = [&](glm::vec3 at, const std::function<bool(glm::vec3)>& loaded = {}) {
            p.pos = at;
            buryDead(es, dead, g, p, loaded);
            return std::ranges::none_of(es, [](const Entity& e) { return !e.body.alive(); });
        };
        CHECK(!bury({120.5f, 4, 100.5f}) && dead.at(4) == 1u << slot);  // 100 off in the open: seen, it lies; but it's recorded
        CHECK(!bury({60.5f, 4, 100.5f}));
        wall(g, {80, 90}, {80, 110});
        CHECK(!bury({60.5f, 4, 100.5f}));  // behind nothing, 40 off: near
        CHECK(!bury({82.5f, 4, 100.5f}));  // 62 off behind the wall: near still
        es[0].loot = BoarSkin;
        CHECK(!bury({120.5f, 4, 100.5f}));  // behind the wall, far: but it holds loot
        es[0].loot = NoItem;
        CHECK(bury({120.5f, 4, 100.5f}) && es.size() == 1 && dead.at(4) == 1u << slot);  // far and unseen: gone, still dead
        CHECK(damage(es[0], 10000));
        es[0].loot = NoItem;
        CHECK(bury({40.5f, 4, 180.5f}, [](glm::vec3 at) { return at.z > 150; }) && es.empty() && dead.at(4) == 3);  // seen, but not loaded
        Entity& stray = spawnAhead(es, g, p, EntityKind::Wolf, 1, 3);  // not from a marker: goes the same, no record
        CHECK(damage(stray, 10000));
        stray.loot = NoItem;
        CHECK(bury({120.5f, 4, 180.5f}) == false && bury({150.5f, 4, 100.5f}, [](glm::vec3 at) { return at.x > 140; }) && dead.size() == 1);
        // syncSpawns never brings a dead slot back, not even when its marker changes.
        syncSpawns(es, g, {spawn(4, "Den", EntityKind::Boar, {20.5f, 4, 100.5f}, 2)}, 1, {}, 0.5f, dead);
        CHECK(es.empty());
        syncSpawns(es, g, {spawn(4, "Den", EntityKind::Boar, {30.5f, 4, 90.5f}, 3)}, 1, {}, 0.5f, dead);
        CHECK(es.size() == 1 && es[0].slot == 2);
        // An edited marker keeps a corpse that still holds loot (a quest's item), under the marker as it is now.
        std::vector<Entity> hs;
        Marker chief = spawn(9, "Hakon", EntityKind::BanditChief, {60.5f, 4, 60.5f});
        syncSpawns(hs, g, {chief}, 1, {}, 0.5f);
        CHECK(hs.size() == 1 && damage(hs[0], 100000) && hs[0].loot == IronHelm);
        uint32_t body = hs[0].id;
        DeadSlots hd;
        p.pos = {90.5f, 4, 60.5f};
        buryDead(hs, hd, g, p);
        chief.radius = 5;
        syncSpawns(hs, g, {chief}, 1, {}, 0.5f, hd);
        CHECK(hs.size() == 1 && hs[0].id == body && hs[0].loot == IronHelm && hs[0].spawn.radius == 5);
        syncSpawns(hs, g, {chief}, 1, {}, 0.5f, hd);  // (not stale again)
        CHECK(hs.size() == 1 && hs[0].id == body);
        hs[0].loot = NoItem;  // taken: the next edit clears it, and nobody comes back
        chief.radius = 6;
        syncSpawns(hs, g, {chief}, 1, {}, 0.5f, hd);
        CHECK(hs.empty());
    }

    {  // Bystanders: a villager can't be locked onto, and no backstab pulls an attack onto it; the dummy is still both.
        Grid g = flat(48);
        std::vector<Entity> es;
        es.reserve(4);
        Player at;
        at.pos = {24.5f, 4, 19.5f};
        Entity& v = spawnAhead(es, g, at, EntityKind::Villager, 1, 1);
        LockView view;
        view.eye = {24.5f, 6, 6};
        view.viewProj = glm::perspectiveRH_ZO(glm::radians(70.0f), view.aspect, 0.1f, 1000.0f) *
                        glm::lookAtRH(view.eye, glm::vec3(24.5f, 5, 30), glm::vec3(0, 1, 0));
        glm::vec3 me(24.5f, 4, 10.5f);
        CHECK(pickTarget(es, g, view, me, {}) == 0);
        at.pos = {28.5f, 4, 19.5f};
        Entity& d = spawnAhead(es, g, at, EntityKind::TrainingDummy, 2, 1);
        CHECK(pickTarget(es, g, view, me, {}) == d.id);
        for (Entity* e : {&v, &d}) {
            e->body.yaw = 0, e->body.hitTime = 99;  // facing +z, not hit lately
            Player behindIt;
            behindIt.pos = e->body.pos - glm::vec3(0, 0, 1.2f);
            CHECK(opensToCritical(*e, behindIt) == (e == &d));
            e->body.broken = true;  // nor a riposte
            CHECK(opensToCritical(*e, behindIt) == (e == &d));
            e->body.broken = false;
        }
    }

    {  // In play: the dead stay dead; the save keeps who (DEAD), and its corpse while it lies (NPCS).
        auto make = [&] {
            Session s{.world = flat(256), .seed = 3};
            wall(s.world, {100, 200}, {255, 200});
            s.markers = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {20.5f, 4, 20.5f}}, spawn(2, "Raiders", EntityKind::Bandit, {30.5f, 4, 60.5f}, 2)};
            startSession(s);
            return s;
        };
        auto slotOf = [](Session& s, int slot) -> Entity* {
            auto it = std::ranges::find(s.entities, slot, &Entity::slot);
            return it != s.entities.end() ? &*it : nullptr;
        };
        Session s = make();
        s.dead[77] = 1;  // a marker removed since (the editor): not saved
        s.player.teleport({230.5f, 4, 230.5f});  // far, behind the wall
        CHECK(s.entities.size() == 2 && damage(*slotOf(s, 0), 10000));
        slotOf(s, 0)->loot = NoItem;
        slotOf(s, 1)->calmTimer = 1e6f;
        step(s, {}, kTick);
        CHECK(s.entities.size() == 1 && !slotOf(s, 0) && s.dead.at(2) == 1);
        for (int i = 0; i < 130 * 60; ++i) step(s, {}, kTick);  // past the longest old respawn time (120 s)
        CHECK(s.entities.size() == 1 && slotOf(s, 1) && slotOf(s, 1)->body.alive());
        std::string path = (fs::temp_directory_path() / "flycoos_dead_test.flys").string();
        fs::remove(path), fs::remove(path + ".bak");
        CHECK(savePlayer(path.c_str(), s, "w.flyw", 5).empty());
        Session b = make();
        SaveReport r;
        CHECK(loadPlayer(path.c_str(), b, 5, r).empty() && r.npcs == 1 && r.droppedNpcs == 0 && r.notes.empty());
        s.dead.erase(77);
        CHECK(b.dead == s.dead && b.entities.size() == 1 && slotOf(b, 1) && !slotOf(b, 0));
        // A corpse holding loot lies on through a save; one in a save without its DEAD record (older saves) still stays dead.
        CHECK(damage(*slotOf(b, 1), 10000));
        slotOf(b, 1)->loot = Knife;
        step(b, {}, kTick);
        CHECK(b.dead.at(2) == 3 && b.entities.size() == 1);
        CHECK(savePlayer(path.c_str(), b, "w.flyw", 5).empty());  // the corpse with its record: it loads, loot and all
        {
            Session e = make();
            CHECK(loadPlayer(path.c_str(), e, 5, r).empty() && e.dead.at(2) == 3 && e.entities.size() == 1 && slotOf(e, 1) &&
                  !slotOf(e, 1)->body.alive() && slotOf(e, 1)->loot == Knife);
            startSession(e);  // a new game: everyone back, nobody dead
            CHECK(e.dead.empty() && e.entities.size() == 2 && slotOf(e, 0)->body.alive() && slotOf(e, 1)->body.alive());
        }
        b.dead.clear();
        CHECK(savePlayer(path.c_str(), b, "w.flyw", 5).empty());
        Session c = make();
        CHECK(loadPlayer(path.c_str(), c, 5, r).empty() && r.npcs == 1 && slotOf(c, 1) && !slotOf(c, 1)->body.alive() && slotOf(c, 1)->loot == Knife);
        step(c, {}, kTick);
        CHECK(c.dead.at(2) == 2 && slotOf(c, 1) && !slotOf(c, 1)->body.alive());  // its corpse told: recorded again
        CHECK(c.entities.size() == 2 && slotOf(c, 0)->body.alive());  // (slot 0's corpse had gone: without its record it's back)
        // Bad DEAD sections reject the file: marker 0, no slots, the same marker twice, a size mismatch.
        CHECK(savePlayer(path.c_str(), s, "w.flyw", 5).empty());
        std::vector<uint8_t> good;
        {
            std::ifstream in(path, std::ios::binary);
            good.assign(std::istreambuf_iterator<char>(in), {});
        }
        size_t at = std::string_view(reinterpret_cast<const char*>(good.data()), good.size()).find("DEAD");
        CHECK(at != std::string::npos && good[at + 4] == 9 && good[at + 12] == 1 && good[at + 16] == 2 && good[at + 20] == 1);
        auto broken = [&](auto&& edit) {
            std::vector<uint8_t> v = good;
            edit(v);
            uint32_t h = fnv1a(v.data(), v.size() - 4);
            for (int i = 0; i < 4; ++i) v[v.size() - 4 + size_t(i)] = uint8_t(h >> (8 * i));
            std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size()));
            fs::remove(path + ".bak");
            Session t = make();
            return !loadPlayer(path.c_str(), t, 5, r).empty();
        };
        CHECK(!broken([](std::vector<uint8_t>&) {}));  // (the patching itself is sound)
        CHECK(broken([&](std::vector<uint8_t>& v) { v[at + 16] = 0; }));     // marker 0
        CHECK(broken([&](std::vector<uint8_t>& v) { v[at + 20] = 0; }));     // no slots
        CHECK(broken([&](std::vector<uint8_t>& v) { v[at + 12] = 2; }));     // a count past the payload
        CHECK(broken([&](std::vector<uint8_t>& v) {                          // the same marker twice
            v.insert(v.begin() + long(at + 21), {2, 0, 0, 0, 2});
            v[at + 4] = 14, v[at + 12] = 2;
        }));
        CHECK(!broken([&](std::vector<uint8_t>& v) {                         // (two markers are fine)
            v.insert(v.begin() + long(at + 21), {3, 0, 0, 0, 2});
            v[at + 4] = 14, v[at + 12] = 2;
        }));
        fs::remove(path), fs::remove(path + ".bak");
    }

    {  // Quests: Torvald's Helm needs Torvald. Killed before it starts, it never starts; killed mid-way, it fails (a toast says why).
        const Defs& d = defs();
        const auto helm = size_t(d.questById.at("torvalds_helm"));
        CHECK(d.quests[helm].needs == std::vector<std::string>{"Torvald"});
        for (const Stage& st : d.quests[helm].stages) CHECK(!st.journal.contains("rises again"));
        for (const QuestLine& l : d.questLines) CHECK(!l.text.contains("gets up") && !l.text.contains("will be back"));
        std::vector<Marker> act1 = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {10.5f, 4, 10.5f}},
                                    spawn(2, "Maren Holt", EntityKind::Villager, {20.5f, 4, 20.5f}),
                                    spawn(3, "Torvald", EntityKind::Villager, {60.5f, 4, 20.5f}),
                                    spawn(4, "Captain Brenna", EntityKind::Guard, {100.5f, 4, 20.5f}),
                                    spawn(5, "Hakon the Red", EntityKind::BanditChief, {64.5f, 4, 100.5f}),
                                    {.id = 6, .kind = MarkerKind::Region, .name = "Greyfang Rise", .pos = {64.5f, 4, 100.5f}, .radius = 12}};
        auto make = [&] {
            Session s{.world = flat(128), .seed = 9};
            s.markers = act1;
            startSession(s);
            return s;
        };
        auto npc = [](Session& s, const char* name) -> Entity& {
            return *std::ranges::find_if(s.entities, [&](const Entity& e) { return e.spawn.name == name; });
        };
        SimInput interact;
        interact.interact = true;
        auto talkTo = [&](Session& s, const char* name) {
            s.player.teleport(npc(s, name).body.pos + glm::vec3(1.5f, 0, 0));
            step(s, interact, kTick);
        };
        auto kill = [&](Session& s, const char* name) {
            CHECK(damage(npc(s, name), 100000));
            step(s, {}, kTick);
        };
        Session s = make();
        CHECK(!npcDead(s, "Torvald") && !deadNeed(s, d.quests[helm]));
        kill(s, "Torvald");
        CHECK(npcDead(s, "Torvald") && deadNeed(s, d.quests[helm]) && *deadNeed(s, d.quests[helm]) == "Torvald");
        CHECK(s.quests[helm].failed && s.quests[helm].stage == 0 && s.toasts.empty());  // never started: no news
        talkTo(s, "Maren Holt");
        CHECK(s.quests[helm].stage == 0 && s.quests[helm].failed && s.toasts.empty());  // nor does it start now
        CHECK(npc(s, "Maren Holt").speech() && std::string_view(npc(s, "Maren Holt").speech()).starts_with("Torvald's forge is cold for good"));
        talkTo(s, "Captain Brenna");
        CHECK(npc(s, "Captain Brenna").speech() && std::string_view(npc(s, "Captain Brenna").speech()).starts_with("A smith dead"));
        std::erase_if(s.entities, [](const Entity& e) { return e.spawn.name == "Torvald"; });  // his corpse gone: still dead
        step(s, {}, kTick);
        CHECK(npcDead(s, "Torvald") && passes(s, {.kind = Condition::NpcDead, .name = "Torvald"}) && s.quests[helm].failed);
        CHECK(std::ranges::any_of(s.eventLog, [](const std::string& l) { return l.contains("failed torvalds_helm: Torvald is dead"); }));

        Session m = make();
        talkTo(m, "Maren Holt");
        CHECK(m.quests[helm].stage == 10);
        m.toasts.clear();
        kill(m, "Torvald");
        CHECK(m.quests[helm].failed && m.quests[helm].stage == 10 && !m.quests[helm].active());
        CHECK(m.toasts.size() == 1 && m.toasts[0].second == "Quest failed: Torvald's Helm (Torvald is dead)");
        kill(m, "Maren Holt");
        CHECK(m.toasts.size() == 1);  // failed once
        CHECK(npcDead(m, "Maren Holt") && !npcDead(m, "Captain Brenna") && !npcDead(m, "Nobody"));

        Session dn = make();  // a quest already done stays done
        dn.quests[helm].done = true, dn.quests[helm].stage = 40;
        kill(dn, "Torvald");
        CHECK(dn.quests[helm].done && !dn.quests[helm].failed && dn.toasts.empty());

        Session h = make();  // Maren isn't needed: with her dead it starts from Torvald, and with Hakon dead the helm is on his body for good
        kill(h, "Maren Holt");
        kill(h, "Hakon the Red");
        CHECK(!h.quests[helm].failed && npc(h, "Hakon the Red").loot == IronHelm);
        talkTo(h, "Torvald");
        CHECK(h.quests[helm].stage == 20 && h.quests[helm].active());
        h.player.teleport({120.5f, 4, 120.5f});
        for (int i = 0; i < 125 * 60; ++i) step(h, {}, kTick);
        CHECK(npc(h, "Hakon the Red").loot == IronHelm && !npc(h, "Hakon the Red").body.alive());  // two minutes on: still lying there with it
    }

    {  // The data: needs are spawn marker names, each once; checkQuestMarkers warns about one that names no marker.
        fs::path dir = fs::temp_directory_path() / "flycoos_needs_test";
        fs::remove_all(dir);
        fs::create_directories(dir / "quests");
        for (const char* f : {"items.json", "creatures.json", "skills.json", "talents.json", "weapon_classes.json"})
            fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / f, dir / f);
        std::string error;
        auto load = [&](std::string_view needs, Defs& out) {
            std::ofstream(dir / "quests" / "a.json", std::ios::trunc)
                << std::format(R"([{{"id": "a", "title": "A", {} "stages": [{{"id": 1, "journal": "One."}}]}}])", needs);
            return loadDefs(dir, out, error);
        };
        Defs ok;
        CHECK(load(R"("needs": ["Nobody", "Torvald"],)", ok) && ok.quests[0].needs.size() == 2);
        std::vector<std::string> w = checkQuestMarkers(ok, {spawn(3, "Torvald", EntityKind::Villager, {1, 4, 1})});
        CHECK(w.size() == 1 && w[0].contains("no spawn marker named 'Nobody'"));
        for (std::string_view bad : {R"("needs": ["Torvald", "Torvald"],)", R"("needs": "Torvald",)", R"("needs": [3],)", R"("needs": [""],)"}) {
            Defs out;
            CHECK(!load(bad, out) && error.contains("needs"));
        }
        fs::remove_all(dir);
    }
}

// Creature attacks (ADR-057): the boar's charge, the wolf's and bandit's leap, the chief's sweep, the ghoul's grab.
// Each telegraphs and hits through strikePlayer, so the guard, the parry, roll i-frames, hyperarmor and poise meet
// it as designed; the AI picks one by its range band, token, sight, room, cooldown and chance.
static void creatureAttacks() {
    namespace fs = std::filesystem;
    constexpr float kT = 1 / 60.0f;
    {  // The shipped kinds: who has which, and its numbers.
        auto only = [](EntityKind k) -> const CreatureAttack* {
            const std::vector<CreatureAttack>& a = typeOf(k).attacks;
            return a.size() == 1 ? &a[0] : nullptr;
        };
        const CreatureAttack *boar = only(EntityKind::Boar), *wolf = only(EntityKind::Wolf), *bandit = only(EntityKind::Bandit),
                             *chief = only(EntityKind::BanditChief), *ghoul = only(EntityKind::Ghoul);
        CHECK(boar && boar->type == AttackType::Charge && boar->rangeMin == 5 && boar->rangeMax == 14 && boar->speed == 14 &&
              boar->seconds == 1.2f && boar->hyperarmor && boar->blockStamina == 2 && boar->power == 1.6f && boar->chance == 0.7f);
        CHECK(wolf && wolf->type == AttackType::Leap && wolf->rangeMin == 7 && wolf->rangeMax == 12 && wolf->height == 7 && wolf->reach == 1.8f &&
              wolf->hyperarmor);
        CHECK(bandit && bandit->type == AttackType::Leap && bandit->rangeMin == 4 && bandit->rangeMax == 7 && !bandit->hyperarmor && bandit->reach == 2);
        CHECK(chief && chief->type == AttackType::Area && chief->radius == 3.2f && chief->blockStamina == 2.5f && !chief->hyperarmor);
        CHECK(ghoul && ghoul->type == AttackType::Grab && ghoul->hits == 3 && ghoul->interval == 0.45f && ghoul->reach == 1.7f && ghoul->stance == 0);
        for (EntityKind k : {EntityKind::TrainingDummy, EntityKind::Villager, EntityKind::Guard, EntityKind::BanditArcher})
            CHECK(typeOf(k).attacks.empty());
        CHECK(attackMove(Move::Charge) && attackMove(Move::Grab) && codeMove(Move::Leap) && !attackMove(Move::Throw) && !attackMove(Move::Count));
    }
    fs::path dir = fs::temp_directory_path() / "flycoos_attacks_test";
    fs::create_directories(dir);
    copyWeaponClasses(dir);
    fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
    std::ostringstream text;
    text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string creatures = text.str();
    creatures.erase(creatures.rfind(']'));
    std::string error;
    {  // The data's checks: file, record and field in every message.
        std::string five = R"([{"type": "area", "range": [0, 2], "radius": 2})";
        for (int i = 0; i < 4; ++i) five += R"(, {"type": "area", "range": [0, 2], "radius": 2})";
        five += "]";
        for (auto [extra, want] : {
                 std::pair<std::string, std::string>{R"("copy_from": "bandit", "attacks": [{"type": "kick", "range": [1, 2]}])",
                                                     "field 'attacks[0].type': must be one of charge, leap, area, grab"},
                 {R"("copy_from": "bandit", "attacks": {"type": "leap"})", "field 'attacks': must be a list of at most 4 attacks"},
                 {R"("copy_from": "bandit", "attacks": )" + five, "field 'attacks': must be a list of at most 4 attacks"},
                 {R"("copy_from": "bandit", "attacks": [3])", "field 'attacks[0]': must be an object"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap"}])", "field 'attacks[0].range': must be [min, max] blocks"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap", "range": [5]}])", "field 'attacks[0].range': must be [min, max] blocks"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap", "range": [5, 4]}])", "field 'attacks[0].range': must be 0 <= min < max <= 40"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap", "range": [-1, 4]}])", "field 'attacks[0].range': must be 0 <= min < max <= 40"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap", "range": [1, 41]}])", "field 'attacks[0].range': must be 0 <= min < max <= 40"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "charge", "range": [1, 9]}])", "field 'attacks[0].speed': missing"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "charge", "range": [1, 9], "speed": 10}])", "field 'attacks[0].seconds': missing"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "area", "range": [0, 2]}])", "field 'attacks[0].radius': missing"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "charge", "range": [1, 9], "speed": 10, "seconds": 1, "radius": 2}])",
                  "field 'attacks[0].radius': unknown field (for a charge)"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "grab", "range": [0, 2], "block_stamina": 2}])",
                  "field 'attacks[0].block_stamina': unknown field (for a grab)"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap", "range": [1, 5], "hits": 2}])", "field 'attacks[0].hits': unknown field (for a leap)"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "grab", "range": [0, 2], "hits": 1.5}])", "field 'attacks[0].hits': must be an integer"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "grab", "range": [0, 2], "chance": 2}])", "field 'attacks[0].chance': 2 is out of range 0..1"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap", "range": [1, 5], "windup": 0}])", "field 'attacks[0].windup': 0 is out of range"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "leap", "range": [1, 5], "hyperarmor": 1}])", "field 'attacks[0].hyperarmor': must be true or false"},
                 {R"("copy_from": "villager", "attacks": [{"type": "leap", "range": [1, 5]}])", "field 'attacks': harmless creatures don't fight"},
                 {R"("copy_from": "bandit", "attacks": [{"type": "grab", "range": [0, 2]}])", ""}}) {
            std::ofstream(dir / "creatures.json", std::ios::trunc) << creatures << R"(,{"id": "brute", "save_id": 10, )" << extra << "}]";
            Defs t;
            error.clear();
            bool loaded = loadDefs(dir, t, error);
            bool ok = want.empty() ? loaded && t.creatures[10].attacks.size() == 1 : !loaded && error.contains(want) && error.contains("record 10 'brute'");
            CHECK(ok);
            if (!ok) std::println(stderr, "FAIL attack data '{}': {}", extra, error);
            if (loaded && want.empty()) {  // the defaults
                const CreatureAttack& a = t.creatures[10].attacks[0];
                CHECK(a.type == AttackType::Grab && a.reach == 1.6f && a.hits == 3 && a.interval == 0.45f && a.cooldown == 6 && a.chance == 1 &&
                      a.windup == 0.8f && a.recover == 0.8f && a.power == 1 && a.stance == 20 && a.blockStamina == 1 && !a.hyperarmor);
            }
        }
    }
    // Test-only kinds that always attack (chance 1) once (a long cooldown); `sometimes` attacks half the time.
    std::ofstream(dir / "creatures.json", std::ios::trunc) << creatures << R"(,
        {"id": "charger", "save_id": 10, "copy_from": "boar", "attacks": [{"type": "charge", "range": [5, 14], "cooldown": 30, "windup": 0.6,
         "recover": 1, "power": 4, "stance": 35, "block_stamina": 2, "hyperarmor": true, "speed": 14, "seconds": 1.2}]},
        {"id": "leaper", "save_id": 11, "copy_from": "wolf", "dodge": null, "back_off": null, "attacks": [{"type": "leap", "range": [4, 12],
         "cooldown": 30, "windup": 0.5, "recover": 0.6, "power": 1.5, "stance": 18, "hyperarmor": true, "height": 7, "reach": 1.8}]},
        {"id": "sweeper", "save_id": 12, "copy_from": "bandit_chief", "guard": null, "back_off": null, "combo": null, "delay": null,
         "attacks": [{"type": "area", "range": [0, 3], "cooldown": 30, "windup": 0.8, "recover": 1, "power": 1.5, "stance": 30,
         "block_stamina": 2.5, "radius": 3.2}]},
        {"id": "grabber", "save_id": 13, "copy_from": "ghoul", "combo": null, "delay": null, "attacks": [{"type": "grab", "range": [0, 2.2],
         "cooldown": 30, "windup": 0.8, "recover": 1, "power": 0.6, "stance": 0, "reach": 1.7, "hits": 3, "interval": 0.45}]},
        {"id": "sometimes", "save_id": 14, "copy_from": "bandit", "parry": null, "dodge": null, "back_off": null, "combo": null,
         "attacks": [{"type": "leap", "range": [4, 9], "chance": 0.5, "cooldown": 5, "windup": 0.5, "height": 7}]}])";
    Defs test, shipped;
    CHECK(loadDefs(dir, test, error) && loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
    if (!error.empty()) std::println(stderr, "FAIL attacks data: {}", error);
    fs::remove_all(dir);
    setDefs(std::move(test));
    const EntityKind kCharger = EntityKind(10), kLeaper = EntityKind(11), kSweeper = EntityKind(12), kGrabber = EntityKind(13), kSometimes = EntityKind(14);
    const ItemId heater = defs().itemById.at("heater_shield");

    Grid g({64, 16, 64});
    for (int z = 0; z < 64; ++z)
        for (int x = 0; x < 64; ++x)
            for (int y = 0; y < 4; ++y) g.set({x, y, z}, Stone);
    std::mt19937 rng(9);
    Player player;
    std::vector<Entity> es;
    es.reserve(8);
    MeleeInput melee;  // the player's buttons and stick this tick (a roll and a parry are taps)
    Input move;
    int hurts = 0;  // hits the player took
    std::vector<int> hurtTicks;
    int ticks = 0;
    auto reset = [&] {
        player = Player{};
        player.teleport({32.5f, 4, 20.5f});
        player.onGround = true, player.offGround = 0;
        giveItem(player.inv, Shortsword);
        quickMove(player.inv, {SlotRef::Item, 0, 0});
        player.innate = {{Stat::BleedResist, ModKind::Flat, 1e5f}, {Stat::PoisonResist, ModKind::Flat, 1e5f}};  // the hits alone
        es.clear();
        melee = {}, move = {};
        hurts = ticks = 0, hurtTicks.clear();
    };
    auto spawn = [&](EntityKind k, float distance) -> Entity& {  // in front of the player (+z), facing it, fighting
        Entity& e = spawnAhead(es, g, player, k, 5, distance);
        e.body.onGround = true;
        setState(e, AiState::Chase, "forced");
        return e;
    };
    auto tick = [&] {
        float before = player.health;
        Input mv = move;
        updateMelee(player, melee, mv, kT);
        updatePlayer(player, g, mv, kT);
        updateEntities(es, g, player, kT);
        separate(player, g, es);
        updateCombat(player, es, kT, rng);
        if (player.health < before) ++hurts, hurtTicks.push_back(ticks);
        ++ticks;
        move.roll = false, melee.parry = false;
    };
    auto until = [&](auto pred, int most) {
        for (int n = 0; n < most; ++n) {
            if (pred()) return true;
            tick();
        }
        return bool(pred());
    };
    auto newest = [](const Entity& e) { return std::string_view(e.history[(e.transitions - 1) % e.history.size()].reason); };
    auto gone = [](const Entity& e) { return e.body.punching() && e.body.moveTime >= holdTime(e.body) && e.body.hold <= 0; };  // past its telegraph
    auto flat = [&](const Entity& e) { return glm::distance(glm::vec2(e.body.pos.x, e.body.pos.z), glm::vec2(player.pos.x, player.pos.z)); };
    auto face = [&](const Entity& e) { player.yaw = std::atan2(e.body.pos.x - player.pos.x, e.body.pos.z - player.pos.z); };
    auto parryNow = [&](const Entity& e) {  // the player's parry, in its window now
        player.inv.equipped[SlotOffHand] = heater;
        startMove(player, Move::Parry, {e.body.pos.x - player.pos.x, e.body.pos.z - player.pos.z});
        player.moveScale = parryOf(player);
        player.hitPending = false;
        player.moveTime = 0.15f * player.moveScale;
    };
    auto why = [&](const Entity& e, size_t i = 0) {
        const char* r = attackBlocked(e, g, player, i);
        return std::string(r ? r : "");
    };
    int plainMax = 0;  // the boar's hardest plain hit on the bare player (no crit)
    {
        std::mt19937 r(1);
        Player boarBody;
        boarBody.stats = typeOf(kCharger).stats, boarBody.level = typeOf(kCharger).level;
        for (int i = 0; i < 400; ++i) plainMax = std::max(plainMax, resolveHit(boarBody, Player{}, weaponAttack(NoItem), {r}));
    }

    {  // A charge: a held telegraph standing still, then a straight run that hits on contact, by its power, and the player
       // flinches; it stops there (no shoving along), in its recovery: the opening.
        reset();
        Entity& boar = spawn(kCharger, 10);
        CHECK(until([&] { return boar.attack >= 0; }, 5));
        CHECK(boar.state == AiState::Windup && newest(boar) == "charge" && boar.body.move == Move::Charge && boar.body.made(Noise::Windup));
        float hold = boar.body.hold + holdTime(boar.body);  // the telegraph: 0.6 x 0.85 .. 1.35
        CHECK(hold >= 0.6f * 0.85f - 1e-3f && hold <= 0.6f * 1.35f + 1e-3f && boar.attackWait[0] == 30);
        glm::vec3 at = boar.body.pos;
        CHECK(until([&] { return gone(boar); }, 90) && glm::distance(boar.body.pos, at) < 0.05f);  // stood through it
        CHECK(until([&] { return hurts > 0; }, 60));
        CHECK(player.lastDamage > plainMax && player.stagger > 0);  // x4, and 35 stance on no armor
        CHECK(flat(boar) < gAi.chargeContact + 0.3f);  // it stops on the hit
        CHECK(until([&] { return !boar.body.punching(); }, 120) && hurts == 1 && flat(boar) < gAi.chargeContact + 0.5f);
        CHECK(until([&] { return boar.state == AiState::Recover; }, 3) && newest(boar) == "charge");
        CHECK(until([&] { return boar.state != AiState::Recover; }, 120) && boar.attack == -1);
        float recovered = 0;
        for (uint32_t k = 0; k < 8; ++k)
            if (const AiTransition& h = boar.history[k]; h.from == AiState::Recover && std::string_view(h.reason) == "recovered") recovered = h.after;
        CHECK(recovered >= 1 - 1e-3f && recovered < 1.05f);  // its attack's 1 s, not its kind's
        CHECK(boar.attackWait[0] < 30 - 2 && boar.attackWait[0] > 0);  // its cooldown runs down
    }
    {  // Moved out of its line once it stopped turning (gMelee.trackSeconds before it goes): it runs past. Moved early, it turns and hits.
        for (bool early : {false, true}) {
            reset();
            Entity& boar = spawn(kCharger, 10);
            CHECK(until([&] { return boar.attack >= 0; }, 5));
            if (!early) CHECK(until([&] { return boar.body.hold <= gMelee.trackSeconds - 1e-3f; }, 90));
            player.teleport(player.pos + glm::vec3(2.5f, 0, 0));
            CHECK(until([&] { return !boar.body.punching(); }, 200));
            CHECK(hurts == (early ? 1 : 0));
        }
    }
    {  // Behind it as it runs: no hit (only what's in front of it).
        reset();
        Entity& boar = spawn(kCharger, 12);
        CHECK(until([&] { return gone(boar) && flat(boar) < 9; }, 200));
        glm::vec3 fwd(std::sin(boar.body.yaw), 0, std::cos(boar.body.yaw));
        player.teleport(boar.body.pos - fwd * 0.9f);
        for (int i = 0; i < 20; ++i) tick();
        CHECK(hurts == 0);
    }
    {  // A roll through it: its i-frames let it pass.
        reset();
        Entity& boar = spawn(kCharger, 10);
        CHECK(until([&] { return gone(boar) && flat(boar) < gAi.chargeContact + 2.5f; }, 200));
        move.roll = true, move.move = {0, 1};  // into it
        CHECK(until([&] { return !boar.body.punching(); }, 120));
        CHECK(hurts == 0 && boar.body.pos.z < player.pos.z);
    }
    {  // Guarded from the front: a block (part of the damage); a charge can't be parried (its kind): a parry is a weak guard.
        reset();
        melee.guard = true;
        spawn(kCharger, 10);
        CHECK(until([&] { return hurts > 0 || player.made(Noise::Block); }, 200) && player.made(Noise::Block) && !player.made(Noise::Parry));
        reset();
        Entity& boar2 = spawn(kCharger, 10);
        CHECK(until([&] { return gone(boar2) && flat(boar2) < gAi.chargeContact + 0.7f; }, 200));
        parryNow(boar2);
        CHECK(until([&] { return !boar2.body.hitPending; }, 30) && player.made(Noise::Block) && !player.made(Noise::Parry) && boar2.body.stagger == 0);
    }
    {  // Hyperarmor once it goes (data): a hit doesn't make it flinch, a stance break still stops it. In its telegraph it flinches.
        reset();
        Entity& boar = spawn(kCharger, 12);
        CHECK(until([&] { return boar.attack >= 0; }, 5) && boar.body.hold > 0);
        staggerHit(boar, 5, false);
        CHECK(boar.body.stagger > 0 && !boar.body.punching());
        tick();
        CHECK(boar.state == AiState::Stagger && newest(boar) == "flinched" && boar.attack == -1);
        reset();
        Entity& boar2 = spawn(kCharger, 12);
        CHECK(until([&] { return gone(boar2); }, 120));
        staggerHit(boar2, 5, false);
        CHECK(boar2.body.stagger == 0 && boar2.body.move == Move::Charge && boar2.body.stance == boar2.type().stance - 5);
        staggerHit(boar2, 100, false);
        CHECK(boar2.body.stagger > 0 && boar2.body.broken && !boar2.body.punching());
    }
    {  // Into a wall it crashes and reels (an opening, with a thud); at a drop it pulls up instead.
        for (bool wall : {true, false}) {
            reset();
            for (int x = 26; x < 40; ++x)  // behind the player: a wall, or a drop
                for (int y = 1; y < 7; ++y) g.set({x, y, 17}, wall ? Stone : Air);
            Entity& boar = spawn(kCharger, 10);
            CHECK(until([&] { return gone(boar); }, 120));
            player.teleport(player.pos + glm::vec3(2.5f, 0, 0));  // out of its line
            bool crashed = until([&] { return boar.state == AiState::Stagger; }, 240);
            CHECK(crashed == wall);
            if (wall) CHECK(newest(boar) == "crashed" && boar.body.stagger > gAi.crashSeconds - 0.05f && boar.body.made(Noise::Land) && boar.body.landSpeed == 12);
            else CHECK(boar.body.pos.y > 3.9f && boar.body.pos.z > 17.9f);
            for (int x = 26; x < 40; ++x)
                for (int y = 1; y < 7; ++y) g.set({x, y, 17}, y < 4 ? Stone : Air);
        }
    }
    {  // A leap: a crouch, then off the ground for where the player stood, landing with a hit within its reach.
        reset();
        Entity& wolf = spawn(kLeaper, 8);
        CHECK(until([&] { return wolf.attack >= 0; }, 5) && newest(wolf) == "leap" && wolf.body.move == Move::Leap);
        CHECK(until([&] { return wolf.leapt; }, 90) && wolf.body.made(Noise::Jump) && wolf.body.holding && !wolf.body.onGround);
        float high = 0;
        CHECK(until([&] { return high = std::max(high, wolf.body.pos.y), hurts > 0; }, 120));
        CHECK(high > 4.6f && wolf.body.onGround && flat(wolf) < 1.8f && player.stagger > 0);  // flinched: 18 stance
        CHECK(until([&] { return wolf.state == AiState::Recover; }, 60) && newest(wolf) == "leap");
    }
    {  // In the air its hyperarmor holds; a step aside after takeoff and it lands short.
        reset();
        Entity& wolf = spawn(kLeaper, 8);
        CHECK(until([&] { return wolf.leapt; }, 120));
        tick();
        staggerHit(wolf, 50, false);
        CHECK(wolf.body.stagger == 0 && wolf.body.holding);
        player.teleport(player.pos + glm::vec3(4, 0, 0));
        CHECK(until([&] { return !wolf.body.punching(); }, 120) && hurts == 0);
    }
    {  // Its kind loses the attack mid-leap (Reload defs): it's dropped, the move ended, nothing held in the air.
        reset();
        Entity& wolf = spawn(kLeaper, 8);
        CHECK(until([&] { return wolf.leapt; }, 120));
        tick();
        std::vector<CreatureAttack> kept = std::exchange(tuneDefs().creatures[size_t(kLeaper)].attacks, {});
        tick();
        CHECK(wolf.attack == -1 && !wolf.body.punching() && !wolf.body.holding);
        tuneDefs().creatures[size_t(kLeaper)].attacks = kept;
        setState(wolf, AiState::Chase, "forced");
        CHECK(until([&] { return wolf.body.onGround; }, 120) && hurts == 0);
    }
    {  // Forced out of its attack (the debug window) mid-grab: the move ends with it.
        reset();
        Entity& ghoul = spawn(kGrabber, 1.5f);
        CHECK(until([&] { return player.heldBy == ghoul.id; }, 200));
        setState(ghoul, AiState::Chase, "forced");
        CHECK(ghoul.attack == -1 && !ghoul.body.punching());
        tick();
        CHECK(player.heldBy == 0);
    }
    {  // Guarded, it's a block; parried as it lands, it's deflected and reels (a wolf can be parried); rolled through, it misses.
        reset();
        melee.guard = true;
        Entity& wolf = spawn(kLeaper, 8);
        CHECK(until([&] { return player.made(Noise::Block); }, 200) && !wolf.body.hitPending);
        reset();
        Entity& w2 = spawn(kLeaper, 8);
        CHECK(until([&] { return w2.leapt && w2.body.onGround; }, 200) && w2.body.hitPending);
        parryNow(w2);
        CHECK(until([&] { return !w2.body.hitPending; }, 30) && player.made(Noise::Parry) && hurts == 0 && w2.body.stagger > 0);
        reset();
        Entity& w3 = spawn(kLeaper, 8);
        CHECK(until([&] { return w3.leapt && w3.body.vel.y < 0 && w3.body.pos.y < 4.5f; }, 200));
        move.roll = true, move.move = {1, 0};
        CHECK(until([&] { return !w3.body.punching(); }, 90) && hurts == 0);
    }
    {  // The sweep hits all around: behind it as well, within its radius; not farther.
        for (float off : {-2.5f, -4.0f}) {
            reset();
            Entity& chief = spawn(kSweeper, 2.5f);
            CHECK(until([&] { return chief.attack >= 0; }, 10) && newest(chief) == "area" && chief.body.move == Move::Area);
            CHECK(until([&] { return gone(chief); }, 120));
            glm::vec3 fwd(std::sin(chief.body.yaw), 0, std::cos(chief.body.yaw));
            player.teleport(chief.body.pos + fwd * off);  // behind it
            CHECK(until([&] { return !chief.body.hitPending; }, 30));
            CHECK(hurts == (off > -3 ? 1 : 0));
            if (off > -3) CHECK(player.stagger > 0);
        }
    }
    {  // A guard takes it from the front, at its heavy stamina cost; from behind it doesn't; a parry is a weak guard, not a deflection; a roll passes.
        for (int mode : {0, 1, 2, 3}) {  // guard facing it, guard facing away, parry, roll
            reset();
            Entity& chief = spawn(kSweeper, 2.5f);
            face(chief);
            if (mode == 1) player.yaw += 3.14159f;
            melee.guard = mode < 2;
            CHECK(until([&] { return gone(chief); }, 120));
            if (mode == 2) {  // in its window at the hit
                CHECK(until([&] { return chief.body.moveTime >= impactTime(chief.body) - 2 * kT; }, 30));
                parryNow(chief);
            }
            if (mode == 3) move.roll = true, move.move = {1, 0};
            CHECK(until([&] { return !chief.body.hitPending; }, 30));
            if (mode == 0) CHECK(player.made(Noise::Block) && player.stamina < gMelee.staminaMax - 2.5f * gMelee.blockStamina * (1 - gMelee.guardBoost));
            if (mode == 1) CHECK(!player.made(Noise::Block) && hurts == 1);
            if (mode == 2) CHECK(player.made(Noise::Block) && !player.made(Noise::Parry) && chief.parried == 0 && chief.body.stagger == 0);
            if (mode == 3) CHECK(hurts == 0);
        }
    }
    {  // Hit in its telegraph by its poise or more, it flinches: no sweep.
        reset();
        Entity& chief = spawn(kSweeper, 2.5f);
        CHECK(until([&] { return chief.attack >= 0; }, 10) && !gone(chief));
        staggerHit(chief, chief.type().poise, false);
        CHECK(until([&] { return chief.state == AiState::Stagger; }, 2));
        CHECK(chief.attack == -1 && chief.body.move != Move::Area && !chief.body.hitPending && hurts == 0);
    }
    {  // A grab: caught in front, held (no guard, kept in front of it), bitten 3 times 0.45 s apart, then thrown down.
        reset();
        Entity& ghoul = spawn(kGrabber, 1.5f);
        CHECK(until([&] { return ghoul.attack >= 0; }, 10) && newest(ghoul) == "grab" && ghoul.body.move == Move::Grab);
        CHECK(until([&] { return player.heldBy == ghoul.id; }, 120) && hurts == 0);
        melee.guard = true;
        tick();
        CHECK(!player.guarding && player.stagger > 1 && std::abs(flat(ghoul) - gAi.grabGap) < 0.1f);
        CHECK(attackBlocked(ghoul, g, player, 0) != nullptr);
        CHECK(until([&] { return player.heldBy == 0; }, 200));
        CHECK(hurts == 3 && ghoul.bites == 3 && std::abs(player.stagger - gAi.grabThrow) < 0.05f && !player.made(Noise::Block));
        CHECK(hurtTicks.size() == 3 && std::abs(hurtTicks[1] - hurtTicks[0] - 27) <= 1 && std::abs(hurtTicks[2] - hurtTicks[1] - 27) <= 1);
        CHECK(until([&] { return ghoul.state == AiState::Recover; }, 60) && newest(ghoul) == "grab");
    }
    {  // Nothing stops it but a roll or distance: a guard and a parry are caught too.
        for (int mode : {0, 1, 2, 3, 4}) {  // guard, parry, roll, step back, step behind it
            reset();
            melee.guard = mode == 0;
            Entity& ghoul = spawn(kGrabber, 1.5f);
            face(ghoul);
            CHECK(until([&] { return gone(ghoul); }, 120));
            if (mode == 1) parryNow(ghoul);
            if (mode == 2) move.roll = true, move.move = {1, 0};
            if (mode == 3) player.teleport(player.pos - glm::vec3(0, 0, 3));
            if (mode == 4) player.teleport(ghoul.body.pos - glm::vec3(std::sin(ghoul.body.yaw), 0, std::cos(ghoul.body.yaw)) * 1.2f);
            CHECK(until([&] { return !ghoul.body.hitPending; }, 30));
            CHECK((player.heldBy == ghoul.id) == (mode < 2));
            if (mode < 2) CHECK(!player.made(Noise::Parry));
            if (mode >= 3) CHECK(until([&] { return ghoul.state == AiState::Recover; }, 22));  // a miss stumbles out in 0.3 s
        }
    }
    {  // It lets go when it flinches, dies, or the player is put elsewhere: no more bites.
        for (int mode : {0, 1, 2}) {
            reset();
            Entity& ghoul = spawn(kGrabber, 1.5f);
            CHECK(until([&] { return player.heldBy == ghoul.id; }, 200));
            if (mode == 0) staggerHit(ghoul, 50, false);
            if (mode == 1) damage(ghoul, 100000);
            if (mode == 2) player.teleport(player.pos + glm::vec3(10, 0, 0));
            tick();
            CHECK(player.heldBy == 0 && player.stagger <= 0.2f);
            for (int i = 0; i < 30; ++i) tick();  // (a bite was due within 0.45 s)
            CHECK(hurts == 0);
        }
    }
    {  // The others wait while one holds the player: a bandit beside starts nothing and lands nothing.
        reset();
        int attackers = std::exchange(gAi.attackers, 6);  // tokens for all: only the hold keeps them from swinging
        Entity& ghoul = spawn(kGrabber, 1.5f);
        Entity& bandit = spawnAhead(es, g, player, kSometimes, 6, -1.5f);  // (a bandit that doesn't back off or dodge)
        bandit.body.onGround = true;
        setState(bandit, AiState::Chase, "forced");
        CHECK(until([&] { return player.heldBy == ghoul.id; }, 200));
        tick();  // (kept in front of it now)
        glm::vec3 side(std::cos(ghoul.body.yaw), 0, -std::sin(ghoul.body.yaw));
        bandit.body.teleport(player.pos + side * 1.3f);  // in reach of the held player, its swing (the first of a string) on its way
        glm::vec2 at(player.pos.x - bandit.body.pos.x, player.pos.z - bandit.body.pos.z);
        bandit.body.yaw = std::atan2(at.x, at.y), bandit.body.swingTimer = 0, bandit.body.onGround = true;
        setState(bandit, AiState::Windup, "test");
        CHECK(startPunch(bandit.body, at));
        bandit.string = 2;
        Entity& chief = spawnAhead(es, g, player, kSweeper, 8, 1.8f);  // a sweep on its way too, started anyway
        chief.body.teleport(player.pos - side * 1.8f);
        startAttack(chief, 0, {});
        chief.body.hold = 0;  // (no telegraph: it lands while the player is held)
        Entity& archer = spawnAhead(es, g, player, EntityKind::BanditArcher, 9, -10);  // an archer in its band, and one mid-draw
        archer.body.onGround = true;
        setState(archer, AiState::Chase, "forced");
        Entity& drawn = spawnAhead(es, g, player, EntityKind::BanditArcher, 10, -9);
        drawn.body.onGround = true;
        setState(drawn, AiState::Windup, "test");
        startMove(drawn.body, Move::Shot, {0, 1}, 0.1f);
        bool loosed = false;
        hurts = 0;
        uint32_t starts = 0, shots = 0;
        while (player.heldBy) {
            uint32_t before = bandit.transitions;
            tick();
            for (uint32_t k = before; k < bandit.transitions; ++k)
                starts += bandit.history[k % 8].to == AiState::Windup;
            shots += archer.state == AiState::Windup;
            loosed |= drawn.body.loosed;
            if (player.heldBy && bandit.state == AiState::Chase) CHECK(why(bandit) == "player held");
        }
        CHECK(hurts == 3 && starts == 0 && shots == 0 && !loosed && !drawn.body.hitPending);
        gAi.attackers = attackers;
        for (int i = 0; i < 120 && starts == 0; ++i) {  // let go, the bandit swings again
            uint32_t before = bandit.transitions;
            tick();
            for (uint32_t k = before; k < bandit.transitions; ++k) starts += bandit.history[k % 8].to == AiState::Windup;
        }
        CHECK(starts > 0);
    }
    {  // Why it may not start one (the AI and the debug window read the same).
        reset();
        Entity& wolf = spawn(kLeaper, 8);
        wolf.token = true;
        CHECK(why(wolf) == "" && why(wolf, 1) == "none");
        wolf.token = false;
        CHECK(why(wolf) == "no token");
        wolf.token = true;
        wolf.attackWait[0] = 1;
        CHECK(why(wolf) == "cooling down");
        wolf.attackWait[0] = 0;
        wolf.body.stagger = 0.5f;
        CHECK(why(wolf) == "busy");
        wolf.body.stagger = 0;
        wolf.body.onGround = false;
        CHECK(why(wolf) == "busy");
        wolf.body.onGround = true;
        player.heldBy = 9999;
        CHECK(why(wolf) == "player held");
        player.heldBy = 0;
        wolf.body.teleport(player.pos + glm::vec3(0, 0, 3));
        CHECK(why(wolf) == "out of range");
        wolf.body.teleport(player.pos + glm::vec3(0, 0, 13));
        CHECK(why(wolf) == "out of range");
        wolf.body.teleport(player.pos + glm::vec3(0, 2, 8));
        CHECK(why(wolf) == "out of range");  // not about level
        wolf.body.teleport(player.pos + glm::vec3(0, 0, 8));
        for (int x = 26; x < 40; ++x)
            for (int y = 4; y < 7; ++y) g.set({x, y, 24}, Stone);  // a wall between
        CHECK(why(wolf) == "can't see");
        for (int x = 26; x < 40; ++x)
            for (int y = 4; y < 7; ++y) g.set({x, y, 24}, Air);
        for (int x = 26; x < 40; ++x)
            for (int y = 1; y < 4; ++y) g.set({x, y, 27}, Air);  // a drop right in front of it
        CHECK(why(wolf) == "no room");
        for (int x = 26; x < 40; ++x)
            for (int y = 1; y < 4; ++y) g.set({x, y, 27}, Stone);
        setState(wolf, AiState::Recover, "test");
        CHECK(why(wolf) == "not fighting");
        Entity& chief = spawn(kSweeper, 2.5f);  // a sweep needs no sight
        chief.token = true;
        for (int x = 26; x < 40; ++x)
            for (int y = 4; y < 7; ++y) g.set({x, y, 22}, Stone);
        CHECK(!canSee(g, chief.body, player) && why(chief) == "");

        for (int x = 26; x < 40; ++x)
            for (int y = 4; y < 7; ++y) g.set({x, y, 22}, Air), g.set({x, y, 21}, Stone);
        Entity& ghoul = spawn(kGrabber, 1.9f);  // a grab doesn't reach through a wall
        ghoul.token = true;
        CHECK(why(ghoul) == "can't see");
        for (int x = 26; x < 40; ++x)
            for (int y = 4; y < 7; ++y) g.set({x, y, 21}, Air);
        CHECK(why(ghoul) == "");
    }
    {  // By its chance: of 40 openings about half start it (its cooldown then), the rest wait attackRetry; no draw changes `rng`.
        int picked = 0, missed = 0;
        for (uint32_t seed = 0; seed < 40; ++seed) {
            reset();
            Entity& e = spawnAhead(es, g, player, kSometimes, seed, 6);
            e.body.onGround = true;
            setState(e, AiState::Chase, "forced");
            updateEntities(es, g, player, kT);
            if (e.attack >= 0) {
                ++picked;
                CHECK(e.attackWait[0] == 5 && newest(e) == "leap");
            } else {
                ++missed;
                CHECK(std::abs(e.attackWait[0] - gAi.attackRetry) < 1e-4f);
            }
        }
        CHECK(picked > 8 && missed > 8 && picked + missed == 40);
    }
    {  // Before a swing: in reach of both, its own attack comes first.
        reset();
        Entity& chief = spawn(kSweeper, 1.5f);
        CHECK(until([&] { return chief.state == AiState::Windup; }, 10) && newest(chief) == "area");
    }
    {  // The same fight twice is the same fight.
        auto fight = [&] {
            reset();
            rng.seed(3);
            spawn(kCharger, 10);
            spawn(kLeaper, 9);
            for (int i = 0; i < 6 * 60; ++i) tick(), player.health = std::max(player.health, 50.0f);
            return std::pair{hurtTicks, es[0].body.pos};
        };
        auto a = fight(), b = fight();
        CHECK(a.first == b.first && a.second == b.second && !a.first.empty());
    }
    {  // An archer's arrow already flying passes a player another holds (a held player isn't shot), and hits one it doesn't.
        for (bool held : {true, false}) {
            Session s{.world = Grid({32, 16, 32}), .seed = 5};
            for (int z = 0; z < 32; ++z)
                for (int x = 0; x < 32; ++x)
                    for (int y = 0; y < 4; ++y) s.world.set({x, y, z}, Stone);
            s.player.teleport({16.5f, 4, 16.5f});
            s.player.innate = {{Stat::BleedResist, ModKind::Flat, 1e5f}};
            Entity& archer = spawnAhead(s.entities, s.world, s.player, EntityKind::BanditArcher, 3, 10);
            s.player.heldBy = held ? archer.id + 1000 : 0;  // (some other NPC's grab)
            s.projectiles.push_back({{16.5f, 5.1f, 22.5f}, {}, {0, 0, -40}, archer.type().weapon, 1, 0, 0, false, archer.id});
            float before = s.player.health;
            for (int i = 0; i < 30; ++i) updateProjectiles(s, kT);
            CHECK((s.player.health < before) == !held);
        }
    }
    {  // strikePlayer's flags: an unguardable hit goes past a guard; only a parryable one is deflected; a block costs x blockStamina.
        reset();
        Entity& bandit = spawn(EntityKind::Bandit, 1.5f);
        face(bandit);
        player.inv.equipped[SlotOffHand] = heater;
        auto hit = [&](Strike s, int mode) {  // 0 open, 1 guarding, 2 parrying in the window
            Player p = player;
            Entity e = bandit;
            std::mt19937 r(5);
            if (mode == 1) p.guarding = true, p.guardTime = 1;
            if (mode == 2) {
                startMove(p, Move::Parry, {0, 1});
                p.moveScale = parryOf(p), p.hitPending = false, p.moveTime = 0.15f * p.moveScale;
            }
            strikePlayer(e, p, s, r);
            return std::pair{p, e.body.stagger};
        };
        Strike plain{1, 0, bandit.body.pos, DamageType::Slash};
        Strike unguardable = plain, unparryable = plain, heavy = plain, twice = plain;
        unguardable.guardable = false, unparryable.parryable = false, heavy.blockStamina = 2, twice.power = 2;
        float full = player.health - hit(plain, 0).first.health;
        auto [guarded, s1] = hit(plain, 1);
        auto [past, s2] = hit(unguardable, 1);
        auto [blocked2, s3] = hit(heavy, 1);
        auto [parried, s4] = hit(plain, 2);
        auto [weak, s5] = hit(unparryable, 2);
        float doubled = player.health - hit(twice, 0).first.health;
        CHECK(full > 0 && guarded.made(Noise::Block) && player.health - guarded.health < full);
        CHECK(!past.made(Noise::Block) && player.health - past.health == full);
        CHECK(std::abs((gMelee.staminaMax - blocked2.stamina) - 2 * (gMelee.staminaMax - guarded.stamina)) < 1e-3f);
        CHECK(parried.made(Noise::Parry) && parried.health == player.health && s4 > 0);
        CHECK(!weak.made(Noise::Parry) && weak.made(Noise::Block) && weak.health < player.health && s5 == 0);
        CHECK(doubled >= 2 * full - 1 && doubled <= 2 * full + 1);
    }
    setDefs(std::move(shipped));
}

// Bosses (ADR-059): Ketil's data and the phases' checks; a phase change at its threshold, the roar (stagger-proof, no
// damage), after a stagger, to the deepest phase reached; each phase's moves; the territory (aggro only inside, no
// search or giving up inside, home to full health and the first phase); the boss bar's rules and stance values;
// lasting death; a grab let go at a phase change; the save; the same fight twice.
static void bosses() {
    namespace fs = std::filesystem;
    const EntityKind kKetil = EntityKind(9);
    {  // Ketil the Unburied, as shipped.
        const EntityType& k = typeOf(kKetil);
        CHECK(defs().creatureIds[9] == "barrow_lord" && k.name && std::string_view(k.name) == "Ketil the Unburied" && k.boss && k.territory == 36 &&
              k.model == ModelBarrowLord && k.parries == 3 && k.stance == 260 && k.phaseAt == 1 && k.roar == 0 && k.phases.size() == 1);
        CHECK(k.attacks.size() == 2 && k.attacks[0].type == AttackType::Area && k.attacks[1].type == AttackType::Leap && k.combo == 2 &&
              k.windup == 0.75f && k.mods.empty() && k.weapon == defs().itemById.at("barrow_greataxe") && k.loot == k.weapon && k.lootChance == 1);
        const EntityType& p = k.phases[0];
        CHECK(p.phaseAt == 0.5f && p.roar == 2 && p.attacks.size() == 4 && p.attacks[0].type == AttackType::Area && p.attacks[1].type == AttackType::Leap &&
              p.attacks[2].type == AttackType::Charge && p.attacks[3].type == AttackType::Grab && p.windup == 0.5f && p.combo == 3 && p.delay == 0.45f &&
              p.chaseSpeed == 5.5f && p.mods.size() == 2 && p.mods[0].stat == Stat::Damage && p.mods[1].stat == Stat::Frost);
        CHECK(p.boss && p.territory == 36 && p.stance == k.stance && p.poise == k.poise && p.weapon == k.weapon && p.stats.stamina == k.stats.stamina &&
              p.level == k.level && p.model == k.model && p.name == k.name && p.phases.empty());  // the rest is the first phase's
        for (size_t c = 0; c < defs().creatures.size(); ++c)
            if (c != 9) CHECK(!defs().creatures[c].boss && defs().creatures[c].phases.empty() && defs().creatures[c].territory == 0);
    }
    fs::path dir = fs::temp_directory_path() / "flycoos_bosses_test";
    fs::create_directories(dir);
    copyWeaponClasses(dir);
    fs::copy_file(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "items.json", dir / "items.json", fs::copy_options::overwrite_existing);
    std::ostringstream text;
    text << std::ifstream(fs::path(FLYCOOS_SOURCE_DIR) / "data" / "creatures.json").rdbuf();
    std::string creatures = text.str();
    creatures.erase(creatures.rfind(']'));
    std::string error;
    {  // The data's checks: file, record and field in every message.
        std::string four = R"([{"at": 0.8}, {"at": 0.6}, {"at": 0.4}, {"at": 0.2}])";
        for (auto [extra, want] : {
                 std::pair<std::string, std::string>{R"("copy_from": "villager", "boss": true)", "field 'boss': a boss fights and can be hit"},
                 {R"("copy_from": "dummy", "boss": true)", "field 'boss': a boss fights and can be hit"},
                 {R"("copy_from": "bandit", "boss": 1)", "field 'boss': must be true or false"},
                 {R"("copy_from": "villager", "territory": 10)", "field 'territory': harmless creatures don't fight"},
                 {R"("copy_from": "bandit", "territory": -1)", "field 'territory': -1 is out of range"},
                 {R"("copy_from": "villager", "phases": [{"at": 0.5}])", "field 'phases': only creatures that fight and can be hit have phases"},
                 {R"("copy_from": "bandit", "phases": {"at": 0.5})", "field 'phases': must be a list of 1 to 3 phases"},
                 {R"("copy_from": "bandit", "phases": [])", "field 'phases': must be a list of 1 to 3 phases"},
                 {R"("copy_from": "bandit", "phases": )" + four, "field 'phases': must be a list of 1 to 3 phases"},
                 {R"("copy_from": "bandit", "phases": [3])", "field 'phases[0]': must be an object"},
                 {R"("copy_from": "bandit", "phases": [{"roar": 1}])", "field 'phases[0].at': missing"},
                 {R"("copy_from": "bandit", "phases": [{"at": 1}])", "field 'phases[0].at': 1 is out of range"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0}])", "field 'phases[0].at': 0 is out of range"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5}, {"at": 0.6}])", "field 'phases[1].at': must be below the phase before's"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5}, {"at": 0.5}])", "field 'phases[1].at': must be below the phase before's"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "model": "wolf"}])", "field 'phases[0].model': unknown field (a phase changes only"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "stats": {"stamina": 50}}])", "field 'phases[0].stats': unknown field"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "phases": []}])", "field 'phases[0].phases': unknown field"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "roar": 11}])", "field 'phases[0].roar': 11 is out of range"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "combo": 9}])", "field 'combo': 9 is out of range 1..4 (in phases[0])"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "attacks": [{"type": "kick", "range": [1, 2]}]}])",
                  "field 'attacks[0].type': must be one of charge, leap, area, grab (in phases[0])"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "mods": [{"stat": "max_health", "increased": 0.5}]}])",
                  "field 'phases[0].mods': a phase can't change max health"},
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "mods": [{"stat": "stamina", "flat": 5}]}])", "field 'phases[0].mods': a phase can't change max health"},
                 {R"("copy_from": "bandit", "mods": [{"stat": "stamina", "flat": 5}], "phases": [{"at": 0.5}])", ""},  // (inherited: fine)
                 {R"("copy_from": "bandit", "phases": [{"at": 0.5, "combo": null, "delay": null, "attacks": null}])", ""}}) {
            std::ofstream(dir / "creatures.json", std::ios::trunc) << creatures << R"(,{"id": "bad", "save_id": 10, )" << extra << "}]";
            Defs t;
            error.clear();
            bool loaded = loadDefs(dir, t, error);
            bool ok = want.empty() ? loaded : !loaded && error.contains(want) && error.contains("record 10 'bad'");
            CHECK(ok);
            if (!ok) std::println(stderr, "FAIL boss data '{}': {}", extra, error);
            if (loaded && want.empty() && extra.contains("null")) {  // a field set to null takes its default; the rest is inherited; the roar's default
                const EntityType& b = t.creatures[10];
                CHECK(b.phases.size() == 1 && b.phases[0].combo == 1 && b.phases[0].delay == 0 && b.phases[0].attacks.empty() && b.phases[0].roar == 2 &&
                      b.phases[0].chaseSpeed == 5.5f && b.phases[0].parry == b.parry && !b.boss && b.territory == 0 && b.attacks.size() == 1);
            }
        }
    }
    // Test-only bosses: `lord` with a leap, then a charge and a grab (each always taken), then quick swings; `keeper` with
    // a small territory; `wanderer` with phases and no territory (it pursues as any enemy).
    std::ofstream(dir / "creatures.json", std::ios::trunc) << creatures << R"(,
        {"id": "lord", "save_id": 10, "copy_from": "barrow_lord", "combo": null, "delay": null,
         "attacks": [{"type": "leap", "range": [6, 14], "cooldown": 30, "windup": 0.5, "height": 8}],
         "phases": [{"at": 0.5, "roar": 1, "chase_speed": 7, "stance": 200, "mods": [{"stat": "damage", "more": 0.5}],
                     "attacks": [{"type": "charge", "range": [8, 20], "cooldown": 30, "windup": 0.5, "speed": 12, "seconds": 1},
                                 {"type": "grab", "range": [0, 2.2], "cooldown": 30, "windup": 0.5, "reach": 1.9, "hits": 5, "interval": 0.5}]},
                    {"at": 0.25, "roar": 0.5, "attack_windup": 0.3}]},
        {"id": "keeper", "save_id": 11, "copy_from": "lord", "territory": 10},
        {"id": "wanderer", "save_id": 12, "copy_from": "lord", "boss": null, "territory": null}])";
    Defs test, shipped;
    CHECK(loadDefs(dir, test, error) && loadDefs(fs::path(FLYCOOS_SOURCE_DIR) / "data", shipped, error));
    if (!error.empty()) std::println(stderr, "FAIL bosses data: {}", error);
    fs::remove_all(dir);
    if (test.creatures.size() != 13) return;
    setDefs(std::move(test));
    const EntityKind kLord = EntityKind(10), kKeeper = EntityKind(11), kWanderer = EntityKind(12);
    CHECK(typeOf(kLord).phases.size() == 2 && typeOf(kLord).phases[1].attacks.size() == 1 && typeOf(kLord).phases[1].attacks[0].type == AttackType::Leap);  // each phase patches the first

    Grid g({160, 16, 160});
    auto field = [&] {
        for (int z = 0; z < 160; ++z)
            for (int x = 0; x < 160; ++x) {
                for (int y = 0; y < 3; ++y) g.set({x, y, z}, Dirt);
                g.set({x, 3, z}, Grass);
                for (int y = 4; y < 16; ++y) g.set({x, y, z}, Air);
            }
    };
    field();
    Player player;
    std::mt19937 rng(11);
    std::vector<Entity> es;
    es.reserve(4);
    const glm::vec3 home{80.5f, 4, 80.5f};
    const std::vector<Marker> markers = {{.id = 1, .kind = MarkerKind::Spawn, .pos = home, .radius = 3, .entity = kLord}};
    auto spawn = [&](EntityKind kind, bool fighting = true) {
        es.clear();
        syncSpawns(es, g, {{.id = 1, .kind = MarkerKind::Spawn, .pos = home, .radius = 3, .entity = kind}}, 1, player.pos, 0.5f);
        CHECK(es.size() == 1);
        es[0].body.teleport(home);
        if (fighting) setState(es[0], AiState::Chase, "forced");
        return &es[0];
    };
    auto reason = [](const Entity& e, uint32_t back = 0) {
        return std::string_view(e.history[(e.transitions - 1 - back) % e.history.size()].reason);
    };
    bool heal = true;  // the player's health back to full every tick (its hits aside)
    auto sim = [&](int ticks, auto&& each) {
        for (int i = 0; i < ticks; ++i) {
            each(i);
            if (heal && player.alive()) player.health = 100;
            updateEntities(es, g, player, kTick);
            separate(player, g, es);
            updateCombat(player, es, kTick, rng);
        }
    };
    auto none = [](int) {};
    auto to = [&](Entity& e, float share) {  // its health down to `share` of its max, through damage()
        int most = maxHealth(sheet(e.body));
        CHECK(damage(e, int(std::ceil(e.body.health - share * float(most)))));
    };
    auto reset = [&](glm::vec3 at) {
        player = Player{};
        player.teleport(at);
        player.onGround = true;
        heal = true;
    };

    // The phase change: at its threshold it roars (stagger-proof, taking no damage), then fights with the phase's numbers.
    reset(home + glm::vec3(0, 0, -3));
    Entity* b = spawn(kLord);
    sim(1, none);
    CHECK(b->phase == 0 && b->state == AiState::Chase && b->body.tuning.runSpeed == typeOf(kLord).chaseSpeed && b->body.innate.empty());
    to(*b, 0.51f);
    sim(1, none);
    CHECK(b->phase == 0 && b->state != AiState::Roar);  // just above it: no
    to(*b, 0.5f);
    sim(1, none);
    CHECK(b->state == AiState::Roar && reason(*b) == "phase_2" && b->phase == 1 && &b->type() == &typeOf(kLord).phases[0]);
    CHECK(b->body.tuning.runSpeed == 7 && b->body.innate.size() == 1 && b->body.innate[0].stat == Stat::Damage && std::string_view(b->body.innate[0].source) == "innate" &&
          b->body.roar > 0 && !b->body.punching() && b->body.stance == 200);  // (its stance no more than the phase's)
    float hp = b->body.health;
    CHECK(!damage(*b, 10) && b->body.health == hp);  // no damage...
    staggerHit(*b, 1000, false);
    CHECK(b->body.stagger == 0 && !b->body.broken && b->body.stance == 200);  // ...no stance damage, no flinch...
    staggerHit(*b, 0, true);
    CHECK(b->body.stagger == 0);  // ...no knock-down
    b->body.hitTime = 99;
    player.teleport(b->body.pos - glm::vec3(std::sin(b->body.yaw), 0, std::cos(b->body.yaw)) * 1.5f);
    player.yaw = b->body.yaw;
    CHECK(behind(*b, player) && !opensToCritical(*b, player));  // ...and no backstab...
    player.inv.equipped[SlotMainHand] = defs().itemById.at("iron_shortsword");
    startMove(player, Move::Riposte, glm::vec2(std::sin(b->body.yaw), std::cos(b->body.yaw)));
    holdCritical(es, player);
    CHECK(player.hitPending && b->body.stagger == 0);  // ...nor held by one already on its way
    player = Player{}, player.teleport(home + glm::vec3(0, 0, -3)), player.onGround = true;
    bool steady = true;
    sim(54, [&](int) { steady &= b->state == AiState::Roar && b->body.roar > 0 && b->body.roar <= 1; });
    CHECK(steady && b->body.health == hp);
    sim(8, none);
    CHECK(b->angry() && b->state != AiState::Roar && (reason(*b) == "roared" || reason(*b, 1) == "roared") && b->body.roar == 0 && b->phase == 1);
    CHECK(damage(*b, 1) && b->body.health == hp - 1);  // hurt again
    for (float& w : b->attackWait) w = 5;
    to(*b, 0.25f);  // the next phase: its numbers, and its attacks ready
    sim(1, none);
    CHECK(b->state == AiState::Roar && reason(*b) == "phase_3" && b->phase == 2 && b->type().windup == 0.3f && b->body.tuning.runSpeed == typeOf(kLord).chaseSpeed &&
          b->body.innate.empty() && b->attackWait == decltype(b->attackWait){});

    // A swing on its way when the threshold is crossed stops.
    reset(home + glm::vec3(0, 0, -1.8f));
    b = spawn(kLord);
    bool swinging = false;
    for (int i = 0; i < 300 && !swinging; ++i) {
        sim(1, none);
        swinging = b->state == AiState::Windup && b->attack < 0 && b->body.punching();
    }
    to(*b, 0.5f);
    sim(1, none);
    CHECK(swinging && b->phase == 1 && b->state == AiState::Roar && !b->body.punching());
    // Straight to the deepest phase its health reached; a forced roar from the debug window just fights on.
    reset(home + glm::vec3(0, 0, -3));
    b = spawn(kLord);
    to(*b, 0.2f);
    sim(1, none);
    CHECK(b->state == AiState::Roar && reason(*b) == "phase_3" && b->phase == 2 && phaseFor(*b) == 2);
    sim(40, none);
    CHECK(b->state == AiState::Chase && reason(*b) == "roared");
    // A stagger plays out first: a broken stance, then the roar.
    b = spawn(kLord);
    sim(1, none);
    staggerHit(*b, 300, false);
    to(*b, 0.4f);
    sim(1, none);
    CHECK(b->state == AiState::Stagger && b->body.broken && b->phase == 0);
    sim(int(gMelee.breakSeconds * 60) + 2, none);
    CHECK(b->state == AiState::Roar && reason(*b) == "phase_2" && reason(*b, 1) == "steadied" && b->phase == 1);
    // Not out of a fight: one wounded while calm roars only once it fights.
    reset(home + glm::vec3(0, 0, -30));
    b = spawn(kLord, false);
    b->body.health = 0.4f * float(maxHealth(sheet(b->body)));
    sim(30, none);
    CHECK(b->phase == 0 && b->state == AiState::Wander);
    player.teleport(home + glm::vec3(0, 0, -15));
    sim(1, none);
    CHECK(b->state == AiState::Roar && reason(*b) == "phase_2" && reason(*b, 1) == "saw_target");
    // Nor when a hit from outside its territory brings it out: it goes home, healed.
    reset(home + glm::vec3(-15, 0, 0));
    b = spawn(kKeeper, false);
    to(*b, 0.4f);
    sim(1, none);
    CHECK(b->phase == 0 && b->state == AiState::Wander && reason(*b) == "home" && reason(*b, 1) == "left_territory" &&
          b->body.health == float(maxHealth(sheet(b->body))));

    // Each phase its own moves: from 10 blocks the first leaps, the second charges (its list has no leap).
    reset(home + glm::vec3(0, 0, -10));
    b = spawn(kLord);
    int t0 = 0;
    sim(30, [&](int i) { if (!t0 && b->state == AiState::Windup) t0 = i + 1; player.teleport(b->body.pos + glm::vec3(0, 0, -10)); });
    CHECK(t0 > 0 && reason(*b) == "leap" && b->attack == 0 && b->body.move == Move::Leap);
    reset(home + glm::vec3(0, 0, -10));
    b = spawn(kLord);
    to(*b, 0.45f);
    sim(1, none);
    CHECK(b->state == AiState::Roar && b->attack == -1);
    int charged = 0;
    sim(90, [&](int i) {
        if (!charged && b->state == AiState::Windup) charged = i + 1;
        if (!charged) player.teleport(b->body.pos + glm::vec3(0, 0, -10));
    });
    CHECK(charged > 55 && reason(*b, b->state == AiState::Windup ? 0 : 1) == "charge" && b->type().attacks[0].type == AttackType::Charge);  // (after the roar)
    CHECK(std::string_view(attackBlocked(*b, g, player, 2)) == "none");  // two attacks in this phase
    // Close in, the second phase grabs; a phase change lets the player go.
    reset(home + glm::vec3(0, 0, -1.6f));
    b = spawn(kLord);
    to(*b, 0.45f);
    bool held = false;
    sim(200, [&](int) {
        if (!held) player.teleport(b->body.pos + glm::vec3(std::sin(b->body.yaw), 0, std::cos(b->body.yaw)) * 1.5f);
        held |= player.heldBy == b->id;
        if (held && b->phase == 1 && b->state == AiState::Windup) to(*b, 0.2f);
    });
    CHECK(held && b->phase == 2 && player.heldBy == 0);

    // The territory: it notices the player only inside it...
    reset(home + glm::vec3(-15, 0, 0));
    b = spawn(kKeeper, false);
    sim(120, none);
    CHECK(!b->angry());  // in sight and aggro range, outside its 10
    player.teleport(home + glm::vec3(-9, 0, 0));
    sim(1, none);
    CHECK(b->angry() && (reason(*b) == "saw_target" || reason(*b, 1) == "saw_target"));
    // ...always knows where they are inside it: behind a wall it doesn't search, up a pillar it doesn't give up...
    reset(home + glm::vec3(-20, 0, 0));
    b = spawn(kLord);
    for (int z = 70; z <= 90; ++z)
        for (int y = 4; y < 8; ++y) g.set({70, y, z}, Stone);
    bool lost = false;
    sim(15 * 60, [&](int) {
        player.teleport(home + glm::vec3(-20, 0, 0));
        lost |= b->state == AiState::Search || b->state == AiState::Return;
    });
    CHECK(!lost && b->unseen == 0 && b->angry());
    field();
    for (int y = 4; y < 10; ++y) g.set({90, y, 80}, Stone);
    reset({90.5f, 10, 80.5f});
    b = spawn(kLord);
    sim(15 * 60, [&](int) { lost |= b->state == AiState::Return || b->state == AiState::Search; });
    CHECK(!lost && b->angry() && b->lostTime > typeOf(kLord).giveUpSeconds);
    field();
    // ...and doesn't give up across its territory, past its chase range from it (60 blocks, its chase range 40).
    reset(home + glm::vec3(30, 0, 0));
    b = spawn(kLord);
    b->body.teleport(home + glm::vec3(-30, 0, 0));
    sim(1, none);
    CHECK(glm::distance(b->body.pos, player.pos) > typeOf(kLord).chaseRange && b->state == AiState::Chase);
    // ...the same kind without a territory pursues as any enemy does: out of reach, it gives up.
    b = spawn(kWanderer);
    sim(15 * 60, none);
    CHECK(b->state != AiState::Chase && b->transitions > 1);
    // A player who leaves it sends it home (a hit from outside doesn't call it back), full health and its first phase again.
    reset(home + glm::vec3(0, 0, -3));
    b = spawn(kLord);
    to(*b, 0.45f);
    sim(80, none);
    CHECK(b->phase == 1 && b->state != AiState::Roar);
    player.teleport(home + glm::vec3(37, 0, 0));
    sim(300, none);  // (an attack under way plays out, and its recovery)
    CHECK(b->state == AiState::Wander && reason(*b) == "home" && reason(*b, 1) == "left_territory" && b->phase == 0 &&
          b->body.health == float(maxHealth(sheet(b->body))) && b->body.tuning.runSpeed == typeOf(kLord).chaseSpeed);  // (it was home: no walk)
    b->body.teleport(home + glm::vec3(20, 0, 0));
    player.teleport(home + glm::vec3(25, 0, 0));
    to(*b, 0.45f);  // drawn out by hand, wounded, and back in its second phase
    setState(*b, AiState::Chase, "forced");
    sim(71, none);
    CHECK(b->phase == 1);
    player.teleport(home + glm::vec3(60, 0, 0));
    sim(300, none);
    CHECK(b->state == AiState::Return && reason(*b) == "left_territory");
    float before = b->body.health;
    uint32_t walked = b->transitions;
    CHECK(damage(*b, 5));
    sim(1, none);
    CHECK(b->state == AiState::Return && b->body.health == before - 5 && b->transitions == walked);  // not called back from outside (not even for a tick)
    sim(20 * 60, none);
    CHECK(b->state == AiState::Wander && reason(*b) == "home" && b->phase == 0 && b->body.health == float(maxHealth(sheet(b->body))));
    // The player falls: it goes home, healed, in its first phase.
    reset(home + glm::vec3(0, 0, -3));
    b = spawn(kLord);
    to(*b, 0.45f);
    sim(80, none);
    b->body.teleport(home + glm::vec3(10, 0, 0));
    player.teleport(home + glm::vec3(10, 0, -3));
    heal = false;
    player.health = 0;
    bool down = false;
    sim(300, [&](int) { down |= b->state == AiState::Return && reason(*b) == "target_down"; });
    CHECK(down && b->state == AiState::Return && !bossBar(es, player));
    sim(15 * 60, none);
    CHECK(b->state == AiState::Wander && b->phase == 0 && b->body.health == float(maxHealth(sheet(b->body))));

    // The boss bar: the nearest boss fighting the player, its health and its stance worn toward a break.
    reset(home + glm::vec3(0, 0, -30));
    b = spawn(kLord, false);
    b->calmTimer = 99;
    sim(1, none);
    CHECK(!bossBar(es, player));  // calm: none
    setState(*b, AiState::Chase, "forced");
    std::optional<BossBar> bar = bossBar(es, player);
    CHECK(bar && bar->boss == b && bar->health == 1 && bar->stance == 0);
    staggerHit(*b, 65, false);
    to(*b, 0.75f);
    bar = bossBar(es, player);
    CHECK(bar && std::abs(bar->stance - 0.25f) < 1e-5f && std::abs(bar->health - 0.75f) < 0.01f);  // 65 of its 260
    staggerHit(*b, 300, false);
    bar = bossBar(es, player);
    CHECK(b->body.broken && b->body.stance == b->type().stance && bar && bar->stance == 1);  // broken: full (its stance is back underneath)
    b->body.broken = false;
    bar = bossBar(es, player);
    CHECK(bar && bar->stance == 0);
    setState(*b, AiState::Roar, "forced");
    CHECK(bossBar(es, player));  // roaring
    setState(*b, AiState::Search, "forced");
    CHECK(bossBar(es, player));  // searching (forced: it never does inside its territory)
    setState(*b, AiState::Return, "forced");
    CHECK(!bossBar(es, player));
    setState(*b, AiState::Chase, "forced");
    Entity& far = spawnAhead(es, g, player, kLord, 3, 20);  // a second boss, farther
    setState(far, AiState::Chase, "forced");
    Entity& near = spawnAhead(es, g, player, kLord, 4, 5);
    CHECK(bossBar(es, player) && bossBar(es, player)->boss == &far);  // the first is 30 off, the second 20 (the third, 5 off, is calm)
    setState(near, AiState::Chase, "forced");
    CHECK(bossBar(es, player) && bossBar(es, player)->boss == &near);
    Entity& bandit = spawnAhead(es, g, player, EntityKind::Bandit, 5, 2);
    setState(bandit, AiState::Chase, "forced");
    CHECK(bossBar(es, player)->boss == &near);  // a plain fighter has none, even nearest
    damage(near, 100000);
    CHECK(bossBar(es, player) && bossBar(es, player)->boss == &far);  // dead: the next
    player.health = 0;
    CHECK(!bossBar(es, player));
    std::vector<Entity> bandits;
    setState(spawnAhead(bandits, g, player, EntityKind::Bandit, 6, 3), AiState::Chase, "forced");
    player.health = 100;
    CHECK(!bossBar(bandits, player));

    // Lasting death: a dead boss from a marker is recorded and never comes back; its corpse keeps its greataxe.
    reset(home + glm::vec3(0, 0, -3));
    es.clear();
    syncSpawns(es, g, markers, 1, player.pos, 0.5f);
    CHECK(es.size() == 1 && es[0].kind == kLord);
    damage(es[0], 100000);
    sim(1, none);
    DeadSlots dead;
    player.teleport(home + glm::vec3(70, 0, 0));
    buryDead(es, dead, g, player);
    CHECK(es.size() == 1 && es[0].state == AiState::Dead && es[0].loot == defs().itemById.at("barrow_greataxe") && dead.at(1) == 1u);
    es.clear();
    syncSpawns(es, g, markers, 1, player.pos, 0.5f, dead);
    CHECK(es.empty());

    // The save: a boss's fight starts over (full health, its first phase); a phased creature without a territory loads in its phase.
    {
        std::string path = (fs::temp_directory_path() / "flycoos_bosses.flys").string();
        std::vector<Marker> ms = {{.id = 1, .kind = MarkerKind::PlayerSpawn, .pos = {24.5f, 4, 8.5f}},
                                  {.id = 2, .kind = MarkerKind::Spawn, .pos = {8.5f, 4, 52.5f}, .radius = 3, .entity = kLord},
                                  {.id = 3, .kind = MarkerKind::Spawn, .pos = {24.5f, 4, 52.5f}, .radius = 3, .entity = kWanderer}};
        auto make = [&] {
            Session s{.world = meadow(), .seed = 5};
            s.markers = ms;
            startSession(s);
            return s;
        };
        Session a = make();
        CHECK(a.entities.size() == 2);
        for (Entity& e : a.entities) to(e, 0.3f), enterPhase(e, phaseFor(e));
        CHECK(savePlayer(path.c_str(), a, "w.flyw", 7).empty());
        Session c = make();
        SaveReport r;
        CHECK(loadPlayer(path.c_str(), c, 7, r).empty() && r.npcs == 2);
        for (const Entity& e : c.entities) {
            int most = maxHealth(sheet(e.body));
            if (e.kind == kLord) CHECK(e.body.health == float(most) && e.phase == 0);
            else CHECK(e.body.health < 0.31f * float(most) && e.phase == 1 && e.body.tuning.runSpeed == 7 && e.state != AiState::Roar);
        }
        fs::remove(path);
    }

    // The same fight twice is the same fight: the player trading blows through both phases.
    auto fight = [&] {
        reset(home + glm::vec3(0, 0, -2));
        rng.seed(11);
        Entity* f = spawn(kLord);
        for (int i = 0; i < 40 * 60; ++i) {
            if (i % 40 == 0) strike(player, *f, {1.5f, 25, player.pos}, rng);
            sim(1, none);
        }
        return std::tuple{f->body.health, f->phase, f->state, f->body.pos, f->transitions, player.health, rng()};
    };
    auto first = fight();
    CHECK(std::get<1>(first) == 2 && fight() == first);  // (it reached its last phase)
    setDefs(std::move(shipped));
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
    weaponClasses();
    criticals();
    statusEffects();
    flasks();
    scaling();
    greatWeapons();
    bows();
    archers();
    crossbows();
    throwables();
    hitEffects();
    flashes();
    arena();
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
    pursuit();
    lastingDeaths();
    creatureAttacks();
    bosses();
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
