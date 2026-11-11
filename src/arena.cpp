#include "arena.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>

#include "edit.h"

namespace {

uint32_t hash(int x, int z) {  // a fixed scatter for the rough ground and the mossy stones
    uint32_t h = uint32_t(x) * 0x8DA6B343u ^ uint32_t(z) * 0xD8163841u;
    h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12;
    return h;
}

}  // namespace

std::string buildArena(Grid& g, const PrefabLibrary& lib, std::vector<Instance>& instances, std::vector<Marker>& markers) {
    constexpr int F = kArenaFloor, R = kArenaRadius;
    const glm::ivec2 mid = worldMiddle(g);
    auto put = [&](int x, int y, int z, Block b) { g.set({mid.x + x, y, mid.y + z}, b); };
    auto column = [&](int x, int z, int from, int to, Block b) { for (int y = from; y <= to; ++y) put(x, y, z, b); };

    // The plain: stone, three of dirt, grass at the floor's height, air above.
    std::ranges::fill(g.cells, Air);
    for (int y = 0; y <= F; ++y)
        for (int z = g.lo.z; z <= g.hi().z; ++z)
            for (int x = g.lo.x; x <= g.hi().x; ++x) g.set({x, y, z}, y == F ? Grass : y >= F - 3 ? Dirt : Stone);

    // The floor, the wall (its top a walkway), the stands (nine tiers a block up and two deep, the
    // front of each a plank seat) and the outer wall with merlons. Distances from the middle corner.
    for (int z = -80; z < 80; ++z)
        for (int x = -80; x < 80; ++x) {
            float d = std::hypot(float(x) + 0.5f, float(z) + 0.5f);
            if (d < R) {
                column(x, z, F - 2, F, d >= 11.5f && d < 12.5f ? Clay : Sand);  // the duel ring
                if (x >= -1 && x <= 0 && z >= -1 && z <= 0) put(x, F, z, Clay);  // (0, 0)
            } else if (d < R + 2) {
                column(x, z, F + 1, F + 6, Cobble), put(x, F + 7, z, Slate);
            } else if (d < R + 20) {
                int k = int((d - float(R + 2)) / 2), top = F + 8 + k;
                column(x, z, F + 1, top - 1, Cobble);
                put(x, top, z, d - float(R + 2 + 2 * k) < 1 ? Planks : Cobble);
            } else if (d < R + 22) {
                float turn = std::atan2(float(z) + 0.5f, float(x) + 0.5f) + std::numbers::pi_v<float>;
                column(x, z, F + 1, F + 18, Brick);
                if (int(turn * float(R + 21) / 2) % 2 == 0) put(x, F + 19, z, Brick);
            }
        }

    // The Hill (north): a plateau 5 up, a ramp a block up every two from the south, stairs a block
    // up every one from the east, a sheer drop to the west.
    for (int z = 24; z <= 46; ++z)
        for (int x = -10; x <= 15; ++x) {
            int top = x <= 10 ? (z >= 34 ? F + 5 : F + std::min(5, (z - 22) / 2)) : z >= 34 ? F + 5 - (x - 10) : F;
            if (top > F) column(x, z, F + 1, top - 1, Dirt), put(x, top, z, Grass);
        }

    // The Pool (west): a shallow end a block deep, a deep end four deep, a plank bridge on posts across.
    for (int z = -12; z <= 12; ++z)
        for (int x = -46; x <= -24; ++x) {
            int bed = x >= -31 ? F - 1 : F - 4;
            column(x, z, bed + 1, F, Water);
            put(x, bed, z, Sand);
        }
    for (int x = -47; x <= -23; ++x) put(x, F + 1, 0, DarkPlanks);
    for (int x : {-44, -38, -32, -27}) column(x, 0, F - 4, F, Wood);

    // The Pillars (east): a wall three high that hides you, a kerb you step over; columns and crates below.
    for (int z = -4; z <= 4; ++z) column(29, z, F + 1, F + 3, Cobble), column(30, z, F + 1, F + 3, Cobble);
    for (int x = 39; x <= 46; ++x) put(x, F + 1, -1, Cobble), put(x, F + 1, 0, Cobble);

    // The Rough (north-east): bumps up to two high, some of them rock.
    for (int z = 18; z <= 40; ++z)
        for (int x = 18; x <= 40; ++x) {
            if (std::hypot(float(x) + 0.5f, float(z) + 0.5f) >= 50) continue;
            uint32_t h = hash(x >> 1, z >> 1);
            int up = int(h % 6) / 2;  // 0, 0, 1, 1, 2, 2 (in pairs of columns)
            if (up) column(x, z, F, F + up - 1, Dirt), put(x, F + up, z, up == 2 && (h >> 8) % 3 == 0 ? Rock : Grass);
        }

    // The Narrows (north-west): an L-shaped corridor two wide between walls four high, in from the
    // south and out to the east.
    for (int z = 19; z <= 36; ++z)
        for (int x = -29; x <= -18; ++x)
            if (x == -29 || z == 36 || (x == -26 && z <= 33) || (z == 33 && x >= -26))
                for (int y = F + 1; y <= F + 4; ++y) put(x, y, z, hash(x * 7 + y, z) % 4 ? Cobble : MossyCobble);

    // Prefab instances: placed like the editor does, so they can be moved or removed later.
    instances.clear();
    EditHistory history;
    uint32_t seed = 1;
    auto place = [&](const char* name, int x, int z, int rotation = 0, std::optional<int> y = std::nullopt) -> std::string {
        const Prefab* p = lib.find(name);
        if (!p) return std::format("no prefab {} (prefabs/{}.txt)", name, name);
        Instance in{.prefab = name, .pos = {mid.x + x, 0, mid.y + z}, .rotation = rotation, .seed = seed++};
        EditResult r = placePrefab(g, *p, in, y, instances, history);
        return r.error ? std::format("prefab {} at {}, {}: {}", name, x, z, r.error) : "";
    };
    std::string error;
    auto add = [&](std::string e) { if (error.empty()) error = std::move(e); };
    add(place("arena_gate", 0, -R - 2, 0, F));  // its front (the bars) in the wall's inner face
    add(place("arena_gate", 0, R + 1, 2, F));
    for (auto [x, z] : {std::pair{26, -10}, {26, 10}, {34, 0}, {42, -10}, {42, 10}}) add(place("arena_pillar", x, z));
    for (auto [x, z] : {std::pair{37, 5}, {31, -14}, {-14, -30}}) add(place("crate_stack", x, z));
    for (auto [x, z] : {std::pair{20, -30}, {28, -26}, {36, -22}}) add(place("archery_target", x, z));
    for (auto [x, z] : {std::pair{3, -41}, {36, 36}, {-37, 36}, {36, -37}, {-37, -37}}) add(place("brazier", x, z));
    for (int k = 0; k < 8; ++k) {  // poles on the walkway between the gates, cloths along it
        float a = (22.5f + 45.0f * float(k)) * std::numbers::pi_v<float> / 180;
        glm::ivec2 pole(glm::floor(float(R + 1) * glm::vec2(std::cos(a), std::sin(a))));
        // The quarter turn whose cloth (+x unturned; each turn takes +x toward -z) runs nearest the
        // tangent; the footprint's centre is a cell from the pole along it.
        constexpr glm::ivec2 kAlong[4] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}};
        glm::vec2 tangent(-std::sin(a), std::cos(a));
        int turn = 0;
        for (int r = 1; r < 4; ++r)
            if (glm::dot(glm::vec2(kAlong[r]), tangent) > glm::dot(glm::vec2(kAlong[turn]), tangent)) turn = r;
        glm::ivec2 c = pole + kAlong[turn];
        add(place("banner", c.x, c.y, turn, F + 7));
    }
    if (!error.empty()) return error;

    // Markers: where the player starts, three dummies, the arena's region and a named point per zone.
    markers.clear();
    auto mark = [&](MarkerKind kind, const char* name, int x, int z, float radius, EntityKind entity = EntityKind::Villager, int count = 1) {
        float y = float(groundTop(g, mid.x + x, mid.y + z) + 1);
        markers.push_back({.id = uint32_t(markers.size() + 1), .kind = kind, .name = name,
                           .pos = {float(mid.x + x) + 0.5f, y, float(mid.y + z) + 0.5f}, .radius = radius, .entity = entity, .count = count});
    };
    mark(MarkerKind::PlayerSpawn, "Arena start", 0, -40, 0);
    mark(MarkerKind::Spawn, "Training dummies", -8, -34, 0, EntityKind::TrainingDummy, 3);
    mark(MarkerKind::Region, "The Arena", 0, 0, float(R));
    mark(MarkerKind::Poi, "Arena centre", 0, 0, 6);
    mark(MarkerKind::Poi, "The Hill", 0, 40, 8);
    mark(MarkerKind::Poi, "The Pillars", 34, -6, 8);
    mark(MarkerKind::Poi, "The Pool", -28, -6, 8);
    mark(MarkerKind::Poi, "The Rough", 29, 29, 8);
    mark(MarkerKind::Poi, "The Narrows", -27, 16, 5);
    mark(MarkerKind::Poi, "The Targets", 26, -36, 6);
    return "";
}
