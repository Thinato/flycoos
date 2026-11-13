#include "model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

#include "melee.h"
#include "player.h"
#include "projectile.h"

constexpr float kVoxel = 0.1f;  // character voxels are 1/10 of a world block

struct PartSpec { glm::ivec3 size; glm::vec3 center; };  // center in voxels, origin = feet
static const PartSpec kHumanoid[PartCount] = {
    {{10, 9, 9}, {0, 13.5f, 0.3f}},   // Head
    {{8, 7, 6}, {0, 5.5f, 0}},        // Torso
    {{3, 3, 3}, {5.8f, 5.5f, 0.4f}},  // HandL
    {{3, 3, 3}, {-5.8f, 5.5f, 0.4f}}, // HandR
    {{3, 2, 4}, {2.2f, 1, 0}},        // FootL
    {{3, 2, 4}, {-2.2f, 1, 0}},       // FootR
};
static const PartSpec kDummy[PartCount] = {
    {{7, 6, 7}, {0, 18, 0}},     // Head: burlap sack
    {{8, 7, 6}, {0, 11.5f, 0}},  // Torso: straw
    {{4, 2, 2}, {6, 13, 0}},     // HandL: straw arm
    {{4, 2, 2}, {-6, 13, 0}},    // HandR
    {{2, 8, 2}, {0, 6, 0}},      // FootL: post
    {{9, 2, 9}, {0, 1, 0}},      // FootR: cross-shaped base
};
static const PartSpec kBoar[PartCount] = {
    {{7, 6, 6}, {0, 7.5f, 8.5f}},    // Head, out front
    {{8, 7, 12}, {0, 6.5f, 0}},      // Torso: the body, lengthwise along z
    {{3, 4, 3}, {2.5f, 2, 4}},       // HandL: front left leg
    {{3, 4, 3}, {-2.5f, 2, 4}},      // HandR: front right leg
    {{3, 4, 3}, {2.5f, 2, -4}},      // FootL: hind left leg
    {{3, 4, 3}, {-2.5f, 2, -4}},     // FootR: hind right leg
};
// Longer legs and snout, ears, and a tail at the back of the torso's grid (the body starts at z 4).
static const PartSpec kWolf[PartCount] = {
    {{6, 7, 9}, {0, 11.5f, 9}},      // Head: skull at the back of the grid, snout out front, ears on top
    {{7, 6, 17}, {0, 8, -2}},        // Torso: the body, and the tail drooping from its back end
    {{3, 6, 3}, {2, 3, 4.5f}},       // HandL: front left leg
    {{3, 6, 3}, {-2, 3, 4.5f}},      // HandR
    {{3, 6, 3}, {2, 3, -4.5f}},      // FootL: hind left leg
    {{3, 6, 3}, {-2, 3, -4.5f}},     // FootR
};
// The chunky humanoid (ADR-061): a bigger head, mitten hands and bigger boots. The hands' centres are kHumanoid's,
// which the poses aim them by; the head clears the torso and the boots stand on the ground.
static const PartSpec kChunky[PartCount] = {
    {{11, 10, 10}, {0, 14.2f, 0.3f}},  // Head
    {{8, 7, 6}, {0, 5.5f, 0}},         // Torso
    {{4, 4, 4}, {5.8f, 5.5f, 0.4f}},   // HandL
    {{4, 4, 4}, {-5.8f, 5.5f, 0.4f}},  // HandR
    {{4, 3, 5}, {2.3f, 1.5f, 0.2f}},   // FootL
    {{4, 3, 5}, {-2.3f, 1.5f, 0.2f}},  // FootR
};
static bool quadruped(Model m) { return m == ModelBoar || m == ModelWolf; }
static const PartSpec* specs(Model m) {
    return m == ModelDummy ? kDummy : m == ModelBoar ? kBoar : m == ModelWolf ? kWolf : gModelShape.chunky ? kChunky : kHumanoid;
}

// The chief is bigger and bulkier, the ghoul gaunt, the boss towers (ADR-059); only the drawing scales, not the body's box.
static glm::vec3 drawScale(Model m) {
    return m == ModelChief ? glm::vec3(1.25f, 1.12f, 1.25f) : m == ModelGhoul ? glm::vec3(0.82f, 1.06f, 0.82f)
         : m == ModelBarrowLord ? glm::vec3(1.5f, 1.45f, 1.5f) : glm::vec3(1);
}

float overheadHeight(Model m) {
    float top = 0;  // the drawn top of its parts, standing (voxels)
    for (int p = 0; p < PartCount; ++p) top = std::max(top, specs(m)[p].center.y + float(specs(m)[p].size.y) / 2);
    float tuned = m == ModelBoar ? 1.6f : m == ModelWolf ? 1.9f : m == ModelChief ? 2.8f : 2.5f;  // (the heights before, by eye)
    return std::max(tuned, top * kVoxel * drawScale(m).y + 0.4f);
}

// Grey flanks under a brown saddle (plain greys read blue in the sky's light), pale chest and jaw,
// brown legs, dark ears, paws and tail tip.
static Block wolfVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    switch (part) {
    case Head:
        if (v.y >= 5) return v.z >= 1 && v.z <= 2 && (v.x <= 1 || v.x >= 4) && (v.y == 5 || v.x == 0 || v.x == 5) ? DarkHair : Air;
        if (v.z <= 4) return v.z == 4 && v.y == 3 && (v.x == 1 || v.x == 4) ? Eye : v.y == 4 ? Hide : Rock;
        if (v.x < 1 || v.x > 4 || v.y > 2) return Air;  // the snout
        if (v.z == s.z - 1 && v.y == 2 && (v.x == 2 || v.x == 3)) return Eye;  // nose
        return v.y == 0 ? Tusk : Rock;
    case Torso:
        if (v.z < 4) {  // the tail
            if (v.x < 2 || v.x > 4 || (v.y != v.z && v.y != v.z + 1)) return Air;
            return v.z == 0 ? DarkHair : Hide;
        }
        if (v.y >= s.y - 2) return v.y == s.y - 1 && v.x >= 2 && v.x <= 4 ? DarkHair : Hide;
        return v.z >= s.z - 3 && v.y <= 2 ? Tusk : Rock;
    default: return v.y == 0 ? DarkHair : Hide;
    }
}

static Block boarVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    bool front = v.z == s.z - 1;
    switch (part) {
    case Head:  // snout with nostrils, tusks either side of it, small eyes above
        if (front && v.x >= 2 && v.x <= 4 && v.y <= 2) return v.y == 1 && v.x != 3 ? Eye : Snout;
        if (front && (v.x == 1 || v.x == 5) && v.y <= 1) return Tusk;
        if (front && (v.x == 1 || v.x == 5) && v.y == 4) return Eye;
        return v.y == s.y - 1 && v.x == 3 ? DarkHair : Hide;
    case Torso: return v.y == s.y - 1 && (v.x == 3 || v.x == 4) ? DarkHair : Hide;  // bristly ridge
    default: return v.y == 0 ? DarkHair : Hide;  // legs with dark hooves
    }
}

static Block dummyVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    switch (part) {
    case Head:  // sack tied at the neck, two stitched eyes on the front
        if (v.y == 0) return Belt;
        return v.z == s.z - 1 && v.y == 3 && (v.x == 2 || v.x == 4) ? Eye : Burlap;
    case Torso: return v.y == 1 || v.y == 5 ? Belt : Straw;  // rope bindings
    case HandL: case HandR: return Straw;
    case FootL: return Wood;
    default: return std::abs(v.x - 4) <= 1 || std::abs(v.z - 4) <= 1 ? Wood : Air;  // base
    }
}

// One-handed weapons, up along +y, a blade's edges (an axe's head) along x, gripped at heldGrip.
static Grid buildSword() {
    Grid g({5, 15, 1});
    g.set({2, 0, 0}, Iron);                                    // pommel
    for (int y = 1; y <= 3; ++y) g.set({2, y, 0}, Belt);       // leather grip
    for (int x = 0; x < 5; ++x) g.set({x, 4, 0}, Iron);        // crossguard
    for (int y = 5; y <= 13; ++y)
        for (int x = 1; x <= 3; ++x) g.set({x, y, 0}, Steel);  // blade
    g.set({2, 14, 0}, Steel);                                  // tip
    return g;
}
static Grid buildDagger() {  // shorter and narrower, single-edged
    Grid g({5, 11, 1});
    g.set({2, 0, 0}, Iron);
    for (int y = 1; y <= 3; ++y) g.set({2, y, 0}, Belt);
    for (int x = 1; x <= 3; ++x) g.set({x, 4, 0}, Iron);
    for (int y = 5; y <= 9; ++y) g.set({2, y, 0}, Steel);
    for (int y = 5; y <= 8; ++y) g.set({3, y, 0}, Steel);
    g.set({2, 10, 0}, Steel);
    return g;
}
static Grid buildAxe() {  // a wooden haft, the head out to +x near the top: iron, a steel edge, a short poll behind
    Grid g({7, 15, 1});
    for (int y = 0; y <= 13; ++y) g.set({2, y, 0}, y <= 3 ? Belt : Wood);
    for (int y = 10; y <= 13; ++y) g.set({3, y, 0}, Iron), g.set({4, y, 0}, Iron);
    for (int y = 9; y <= 14; ++y) g.set({5, y, 0}, Iron);
    for (int y = 8; y <= 14; ++y) g.set({6, y, 0}, Steel);  // the edge flares past the head
    g.set({1, 11, 0}, Iron), g.set({1, 12, 0}, Iron);         // poll
    g.set({2, 14, 0}, Iron);
    return g;
}
static Grid buildMace() {  // a haft and a flanged iron head, the flanges out on four sides
    Grid g({5, 14, 5});
    for (int y = 0; y <= 9; ++y) g.set({2, y, 2}, y <= 3 ? Belt : Wood);
    g.set({2, 0, 2}, Iron);  // pommel
    for (int y = 9; y <= 12; ++y)
        for (int x = 1; x <= 3; ++x)
            for (int z = 1; z <= 3; ++z) g.set({x, y, z}, Iron);
    for (int y = 9; y <= 12; ++y) g.set({0, y, 2}, Steel), g.set({4, y, 2}, Steel), g.set({2, y, 0}, Steel), g.set({2, y, 4}, Steel);
    g.set({2, 13, 2}, Iron);
    return g;
}
// Great weapons (ADR-049), held in both hands: a long grip (both hands on it, the right at heldGrip
// under the guard, the left pommel() below), a greatsword's broad blade, a greataxe's bearded head.
static Grid buildGreatsword() {
    Grid g({7, 24, 1});
    g.set({3, 0, 0}, Iron);                                    // pommel
    for (int y = 1; y <= 5; ++y) g.set({3, y, 0}, Belt);       // grip
    for (int x = 0; x < 7; ++x) g.set({x, 6, 0}, Iron);        // crossguard
    for (int y = 7; y <= 22; ++y)
        for (int x = 2; x <= 4; ++x) g.set({x, y, 0}, Steel);  // blade
    g.set({3, 23, 0}, Steel);                                  // tip
    return g;
}
static Grid buildGreataxe() {  // the head out to +x near the top, its beard reaching down the haft, a spike behind
    Grid g({9, 27, 1});
    g.set({3, 0, 0}, Iron);
    for (int y = 1; y <= 25; ++y) g.set({3, y, 0}, y <= 7 ? Belt : Wood);
    for (int y = 19; y <= 24; ++y) g.set({4, y, 0}, Iron), g.set({5, y, 0}, Iron);
    for (int y = 17; y <= 25; ++y) g.set({6, y, 0}, Iron);
    for (int y = 15; y <= 26; ++y) g.set({7, y, 0}, Steel);
    for (int y = 16; y <= 25; ++y) g.set({8, y, 0}, Steel);
    g.set({1, 21, 0}, Iron), g.set({2, 21, 0}, Iron), g.set({2, 22, 0}, Iron);
    g.set({3, 26, 0}, Iron);
    return g;
}
// The healing flask (ADR-047): a round-bellied bottle up along +y, its liquid red (empty: glass), a
// neck and a cork; held at kFlaskGrip, the belly's middle.
static Grid buildFlask(bool full) {
    Grid g({5, 8, 5});
    for (int y = 0; y < 5; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 5; ++x)
                if (glm::length(glm::vec3(x, y, z) - 2.0f) < 2.3f) g.set({x, y, z}, full && y < 4 ? PetalRed : Steel);
    g.set({2, 5, 2}, Steel), g.set({2, 6, 2}, Steel);  // the neck
    g.set({2, 7, 2}, Wood);                          // the cork
    return g;
}
constexpr glm::vec3 kFlaskGrip{2.5f, 2.5f, 2.5f};
// A bow (ADR-050), upright along +y, its belly bowed out to +z and the string behind at z 0; drawn, the
// string is pulled back to a point at the middle (the stave 3 voxels further out). Gripped at the
// middle of the stave (bowGrip). An arrow, up along +y: fletching, the shaft, an iron head at the tip.
static int bowBelly(int y) { return int(std::lround(3 * (1 - (y - 7) * (y - 7) / 49.0))); }
static Grid buildBow(bool drawn) {
    int back = drawn ? 3 : 0;
    Grid g({1, 15, 4 + back});
    for (int y = 0; y <= 14; ++y) {
        g.set({0, y, bowBelly(y) + back}, y >= 6 && y <= 8 ? Belt : Wood);
        if (y > 0 && y < 14) g.set({0, y, drawn ? int(std::lround(3.0 * std::abs(y - 7) / 7)) : 0}, Straw);  // the string
    }
    return g;
}
static Grid buildArrow() {
    Grid g({3, 10, 1});
    for (int y = 0; y <= 8; ++y) g.set({1, y, 0}, Wood);
    g.set({0, 0, 0}, Tunic), g.set({2, 0, 0}, Tunic), g.set({0, 1, 0}, Tunic), g.set({2, 1, 0}, Tunic);  // fletching
    g.set({1, 9, 0}, Iron);
    return g;
}
constexpr glm::vec3 kArrowTip{1.5f, 10, 0.5f};  // its nock is at y 0, 10 voxels back
static glm::vec3 bowGrip(bool drawn) { return {0.5f, 7.5f, drawn ? 6.5f : 3.5f}; }
// A crossbow (ADR-052), level along +z (its nose): the stock down the middle (x 6) from the butt at z 0,
// the prod across the front swept back to its tips, and the string between them, slack across the front
// or spanned back to the latch. A bolt: an arrow's shorter cousin in the arrow's grid (tipped where it is).
static Grid buildCrossbow(bool spanned) {
    Grid g({13, 3, 13});
    for (int z = 0; z <= 11; ++z) g.set({6, 1, z}, Wood);
    for (int z = 0; z <= 2; ++z) g.set({6, 0, z}, Wood);  // the butt, deeper
    g.set({6, 1, 2}, Belt), g.set({6, 1, 3}, Belt);       // the grip
    g.set({6, 0, 4}, Iron);                               // the trigger
    for (int x = 0; x <= 12; ++x) g.set({x, 1, 10 - int(std::lround((x - 6) * (x - 6) / 18.0))}, x == 6 ? Iron : Wood);  // the prod
    g.set({5, 1, 12}, Iron), g.set({6, 1, 12}, Iron), g.set({7, 1, 12}, Iron);  // the stirrup
    for (int x = 1; x <= 11; ++x) {  // over the stock, the string rides on top of it
        int z = spanned ? 4 + int(std::lround(4.0 * std::abs(x - 6) / 6)) : 8;
        g.set({x, std::abs(x - 6) <= 1 ? 2 : 1, z}, Straw);
    }
    return g;
}
static Grid buildBolt() {
    Grid g({3, 10, 1});
    for (int y = 3; y <= 8; ++y) g.set({1, y, 0}, Wood);
    g.set({0, 3, 0}, Belt), g.set({2, 3, 0}, Belt);  // vanes
    g.set({1, 9, 0}, Iron);
    return g;
}
// Its points (model voxels): the trigger hand's grip, the fore hand under the stock, the string slack
// and at the latch (the right hand's way through a reload), and the bolt's tip in the groove.
constexpr glm::vec3 kCrossbowGrip{6.5f, 0.5f, 3.5f}, kCrossbowFore{6.5f, 0.5f, 8.5f}, kStringSlack{6.5f, 2.5f, 8.5f},
    kStringLatch{6.5f, 2.5f, 4.5f}, kBoltTip{6.5f, 2.5f, 11};

// Throwables (ADR-053). A throwing knife up along +y in the arrow's grid, its tip where an arrow's is: a
// wrapped grip, a crossguard, a steel blade. A bomb: a dark round shell with a fuse and its spark. A pot:
// clay, a green band, a neck and a green stopper. Both held and tumbled about their middle (kRoundMiddle).
static Grid buildThrowingKnife() {
    Grid g({3, 10, 1});
    for (int y = 0; y <= 2; ++y) g.set({1, y, 0}, Leather);
    g.set({0, 3, 0}, Iron), g.set({1, 3, 0}, Iron), g.set({2, 3, 0}, Iron);
    for (int y = 4; y <= 9; ++y) g.set({1, y, 0}, Steel);
    return g;
}
static Grid buildRound(bool bomb) {
    Grid g({5, 8, 5});
    for (int y = 0; y < 5; ++y)
        for (int z = 0; z < 5; ++z)
            for (int x = 0; x < 5; ++x)
                if (glm::length(glm::vec3(x, y, z) - 2.0f) < 2.3f) g.set({x, y, z}, bomb ? Slate : y == 2 ? Blade : Clay);
    if (bomb) g.set({2, 5, 2}, Straw), g.set({2, 6, 2}, Straw), g.set({2, 7, 2}, Ember);  // the fuse, lit
    else g.set({2, 5, 2}, Clay), g.set({2, 6, 2}, OakLeaves3);                          // the neck, the stopper
    return g;
}
constexpr glm::vec3 kRoundMiddle{2.5f, 2.5f, 2.5f};
// A burst's puff: a lumpy ball, fire and smoke or a green cloud, the same every time; 13 voxels across.
static Grid buildBurst(bool poison) {
    Grid g({13, 13, 13});
    for (int z = 0; z < 13; ++z)
        for (int y = 0; y < 13; ++y)
            for (int x = 0; x < 13; ++x) {
                uint32_t h = uint32_t(x * 73856093 ^ y * 19349663 ^ z * 83492791) * 2654435761u >> 16;
                float d = glm::length(glm::vec3(x, y, z) - 6.0f), lump = float(h % 9) / 8;
                if (d > 5 + lump * 1.4f) continue;
                Block fire[] = {Lantern, PetalYellow, Ember, PetalYellow, Ember, Ember}, cloud[] = {PetalWhite, BirchLeaves3, BirchLeaves3, OakLeaves3};  // a pale mist over the grass
                bool skin = d > 4.6f;  // the outside: wisps of smoke round the fire, darker green round the cloud
                g.set({x, y, z}, poison ? cloud[skin ? 2 + h % 2 : h % 2] : skin && h % 6 == 0 ? Slate : fire[(d < 3 ? 0 : skin ? 3 : 2) + h % 2]);
            }
    return g;
}

static glm::vec3 heldGrip(Model m) {
    switch (m) {
    case ModelMace: return {2.5f, 2.5f, 2.5f};
    case ModelGreatsword: return {3.5f, 4.5f, 0.5f};
    case ModelGreataxe: return {3.5f, 6.5f, 0.5f};
    default: return {2.5f, 2.5f, 0.5f};
    }
}
// Two-handed (ADR-048), the left hand this many voxels below the right, down the weapon's length:
// at a one-handed weapon's pommel, on a great weapon's long grip.
static float pommel(Model m) { return m == ModelGreatsword ? 3.0f : m == ModelGreataxe ? 3.5f : 2.7f; }

Model heldModel(ItemId weapon) {
    switch (classOf(weapon)) {
    case WeaponClass::Bow: return ModelBow;
    case WeaponClass::Crossbow: return ModelCrossbow;
    case WeaponClass::Dagger: return ModelDagger;
    case WeaponClass::Axe: return ModelAxe;
    case WeaponClass::Mace: return ModelMace;
    case WeaponClass::Greatsword: return ModelGreatsword;
    case WeaponClass::Greataxe: return ModelGreataxe;
    default: return ModelSword;
    }
}

// Shields face +z; the hand holds them at `grip`, just behind the face.
struct ShieldGrid { glm::ivec3 size; glm::vec3 grip; };
static ShieldGrid shieldGrid(Model m) {
    switch (m) {
    case ModelBuckler: return {{7, 7, 2}, {3.5f, 3.5f, -1.5f}};
    case ModelHeater: return {{9, 11, 1}, {4.5f, 6, -1.5f}};
    default: return {{10, 16, 1}, {5, 5.5f, -1.5f}};  // tower: held low, so it clears the ground at the side
    }
}
static Grid buildShield(Model m) {
    ShieldGrid s = shieldGrid(m);
    Grid g(s.size);
    int w = s.size.x, h = s.size.y;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Block b = Air;
            if (m == ModelBuckler) {  // a round oak board with an iron rim and a steel boss
                float r2 = (x - 3.0f) * (x - 3.0f) + (y - 3.0f) * (y - 3.0f);
                b = r2 > 11 ? Air : r2 > 6 ? Iron : Planks;
                if (r2 <= 1) g.set({x, y, 1}, Steel);
            } else if (m == ModelHeater) {  // flat-topped, pointed below: red field, steel rim, a plate band
                float half = y >= 5 ? 4.5f : 1 + 0.75f * float(y);
                float d = std::abs(float(x) - 4);
                b = d > half ? Air : d > half - 1 || y == h - 1 ? Steel : y == 7 ? Plate : Hood;
            } else {  // tower: oak planks, iron rim and two bands, the top corners cut
                bool corner = y == h - 1 && (x == 0 || x == w - 1);
                bool rim = x == 0 || x == w - 1 || y == 0 || y == h - 1 || y == 3 || y == 12;
                b = corner ? Air : rim ? Iron : x % 2 ? DarkPlanks : Planks;
            }
            g.set({x, y, 0}, b);
        }
    return g;
}

Model shieldModel(ItemId shield) {
    switch (itemType(shield).shield.size) {
    case ShieldSize::Medium: return ModelHeater;
    case ShieldSize::Great: return ModelTower;
    default: return ModelBuckler;
    }
}

static_assert(bodyHeight(ModelPlayer) == kHeight);

bool plated(Model model, const Inventory& inv) { return model == ModelGuard || model == ModelBarrowLord || partModels(model, inv)[Torso] == ModelIronArmor; }

bool bladeEdge(Model held, glm::vec3& base, glm::vec3& tip) {  // voxel centres across, from the guard to past the tip
    switch (held) {
    case ModelSword: base = {2.5f, 5, 0.5f}, tip = {2.5f, 15, 0.5f}; return true;
    case ModelDagger: base = {2.5f, 5, 0.5f}, tip = {2.5f, 11, 0.5f}; return true;
    case ModelAxe: base = {6.5f, 8, 0.5f}, tip = {6.5f, 15, 0.5f}; return true;          // the edge
    case ModelMace: base = {2.5f, 9, 2.5f}, tip = {2.5f, 14, 2.5f}; return true;         // the head
    case ModelGreatsword: base = {3.5f, 7, 0.5f}, tip = {3.5f, 24, 0.5f}; return true;
    case ModelGreataxe: base = {7.5f, 15, 0.5f}, tip = {7.5f, 27, 0.5f}; return true;
    default: return false;
    }
}

std::array<Model, PartCount> partModels(Model model, const Inventory& inv) {
    std::array<Model, PartCount> out;
    out.fill(model);
    if (model == ModelDummy || quadruped(model)) return out;
    auto& e = inv.equipped;
    if (e[SlotHead] == IronHelm) out[Head] = ModelIronArmor;
    if (e[SlotChest] == IronCuirass) out[Torso] = ModelIronArmor;
    if (e[SlotHands] == IronGauntlets) out[HandL] = out[HandR] = ModelIronArmor;
    if (e[SlotFeet] == IronSabatons) out[FootL] = out[FootR] = ModelIronArmor;
    return out;
}

// Iron plate (lighter than the Iron block, which reads black in shade) with steel trim, on the
// humanoid's part sizes so poses don't change.
static Block armorVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    bool front = v.z == s.z - 1, top = v.y == s.y - 1;
    switch (part) {
    case Head:  // helm with a crest; the face shows through an opening in the front
        if (front && v.y >= 2 && v.y <= 5 && v.x >= 2 && v.x <= s.x - 3)
            return (v.y == 3 || v.y == 4) && (v.x == 2 || v.x == s.x - 3) ? Eye : Skin;
        return (top && std::abs(2 * v.x - (s.x - 1)) <= 1) || (front && v.y == 6) ? Steel : Plate;  // the crest down the middle
    case Torso:  // cuirass over the tunic's skirt: steel collar and breastplate ridge
        if (v.y < 2) return Tunic;
        if (v.y == 2) return Belt;
        return top || (front && (v.x == 3 || v.x == 4)) ? Steel : Plate;
    default: return front && v.y == s.y - 2 ? Steel : Plate;  // gauntlet knuckles, sabaton toe caps (a row below the top, which rounding cuts)
    }
}

// The guard: the iron helm and gauntlets, a cuirass under a dark red tabard, boots.
static Block guardVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    if (part == FootL || part == FootR) return Boots;
    if (part == Torso && v.z == s.z - 1 && v.x >= 2 && v.x <= s.x - 3 && v.y != 2) return Hood;
    return armorVoxel(part, v, s);
}

// The bandit chief: a dark red hood with an iron mask, a hide jerkin with an iron mantle and a strap.
static Block chiefVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    bool front = v.z == s.z - 1;
    switch (part) {
    case Head:
        if (front && (v.y == 3 || v.y == 4) && (v.x == 2 || v.x == s.x - 3)) return Eye;
        if (v.y <= 2 && v.z >= s.z - 2) return Iron;
        return v.y >= 6 || v.z <= 2 || v.x == 0 || v.x == s.x - 1 ? Hood : Skin;
    case Torso:
        if (v.y == s.y - 1) return Iron;
        if (v.y == 2) return Belt;
        return front && v.x == v.y ? Belt : Hide;
    case HandL: case HandR: return v.y == s.y - 1 ? Iron : Skin;
    default: return Boots;
    }
}

// The barrow ghoul: a skull with dark sockets and teeth, ribs under iron-grey rags, bony hands.
static Block ghoulVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    bool front = v.z == s.z - 1;
    switch (part) {
    case Head:
        if (front && (v.y == 3 || v.y == 4) && (v.x == 2 || v.x == 3 || v.x == s.x - 4 || v.x == s.x - 3)) return Eye;
        if (front && v.y == 1 && v.x >= 2 && v.x <= s.x - 3) return v.x % 2 ? Eye : Tusk;  // teeth
        return v.z <= 2 && v.y >= 4 ? Iron : Tusk;  // a rag of a hood at the back
    case Torso:
        if (v.y == 0 && v.x % 2) return Air;  // tattered hem
        return front && (v.y == 3 || v.y == 5) && v.x >= 1 && v.x <= s.x - 2 ? Tusk : Iron;
    case HandL: case HandR: return Tusk;
    default: return Iron;
    }
}

// The barrow lord (ADR-059): a skull with ember eyes under an iron crown, a dark mantle over rusted mail, bony
// hands in iron cuffs.
static Block barrowLordVoxel(Part part, glm::ivec3 v, glm::ivec3 s) {
    bool front = v.z == s.z - 1;
    switch (part) {
    case Head:
        if (v.y == s.y - 1) return v.x % 3 == 0 ? Iron : Air;  // the crown's points
        if (v.y == s.y - 2) return Iron;                         // its band
        if (front && (v.y == 3 || v.y == 4) && (v.x == 2 || v.x == 3 || v.x == s.x - 4 || v.x == s.x - 3)) return Ember;
        if (front && v.y == 1 && v.x >= 2 && v.x <= s.x - 3) return v.x % 2 ? Eye : Tusk;  // teeth
        return v.z <= 2 && v.y >= 2 ? DarkHair : Tusk;  // a skull, the mantle's hood at the back
    case Torso:
        if (v.y >= s.y - 2 || v.z == 0) return DarkHair;  // the mantle over the shoulders and down the back
        if (v.y == 2) return Belt;
        return front && v.x == s.x - 1 - v.y ? Belt : Iron;  // a strap across rusted mail
    case HandL: case HandR: return v.y == s.y - 1 ? Iron : Tusk;
    default: return Iron;
    }
}

bool isBody(Model m) {
    return m == ModelPlayer || m == ModelVillager || m == ModelDummy || m == ModelBoar || m == ModelBandit || m == ModelIronArmor ||
           m == ModelWolf || m == ModelGuard || m == ModelChief || m == ModelGhoul || m == ModelBarrowLord;
}

// Rounding (ADR-061): the voxels of g's box [lo, hi) whose centres lie outside the rounded box of radius r go. Eyes
// and snouts are never cut.
static void roundBox(Grid& g, glm::ivec3 lo, glm::ivec3 hi, float r) {
    glm::vec3 half = glm::vec3(hi - lo) * 0.5f;
    for (int z = lo.z; z < hi.z; ++z)
        for (int y = lo.y; y < hi.y; ++y)
            for (int x = lo.x; x < hi.x; ++x) {
                Block b = g.get({x, y, z});
                if (b == Air || b == Eye || b == Ember || b == Snout) continue;
                glm::vec3 p = glm::vec3(x, y, z) + 0.5f - glm::vec3(lo) - half;  // from the box's middle
                glm::vec3 out = glm::max(glm::abs(p) - glm::max(half - r, 0.0f), 0.0f);  // past the inner box
                if (glm::length(out) > r + 0.01f) g.set({x, y, z}, Air);
            }
}
// The radius a box of `size` is rounded with, by its thinnest side: the shape's radius from 6, up to 2 at 4 or 5,
// 1.5 up to 3 (a 3x3x3 hand loses its 8 corners; the dummy's thin arms and post nothing).
static float roundRadius(glm::ivec3 size) {
    int thin = std::min({size.x, size.y, size.z});
    return thin <= 3 ? 1.5f : thin <= 5 ? std::min(2.0f, gModelShape.radius) : gModelShape.radius;
}
// Rounds a body part's main boxes: the whole part, but the wolf's skull, snout and body apart (its ears and tail
// stay as they are), the boar's head gently (its tusks and eyes sit on the front corners) and the barrow lord's
// skull under its crown (the crown stays square).
static void roundPart(Grid& g, Model model, Part part) {
    glm::ivec3 s = g.size;
    auto box = [&](glm::ivec3 lo, glm::ivec3 hi) { roundBox(g, lo, hi, roundRadius(hi - lo)); };
    if (model == ModelWolf && part == Head) box({0, 0, 0}, {6, 5, 5}), box({1, 0, 5}, {5, 3, 9});
    else if (model == ModelWolf && part == Torso) box({0, 0, 4}, {7, 6, 17});
    else if (model == ModelBoar && part == Head) roundBox(g, {0, 0, 0}, s, std::min(2.0f, gModelShape.radius));
    else if (model == ModelBarrowLord && part == Head) box({0, 0, 0}, {s.x, s.y - 2, s.z});
    else box({0, 0, 0}, s);
}

static Grid buildFlatPart(Model model, Part part);
Grid buildPart(Model model, Part part) {
    Grid g = buildFlatPart(model, part);
    if (gModelShape.round && isBody(model)) roundPart(g, model, part);
    return g;
}

// A part's voxels before rounding.
static Grid buildFlatPart(Model model, Part part) {
    if (model == ModelSword || model == ModelDagger || model == ModelAxe || model == ModelMace || model == ModelGreatsword || model == ModelGreataxe) {
        if (part != Head) return Grid({1, 1, 1});
        switch (model) {
        case ModelSword: return buildSword();
        case ModelDagger: return buildDagger();
        case ModelAxe: return buildAxe();
        case ModelMace: return buildMace();
        case ModelGreatsword: return buildGreatsword();
        default: return buildGreataxe();
        }
    }
    if (model == ModelBuckler || model == ModelHeater || model == ModelTower) return part != Head ? Grid({1, 1, 1}) : buildShield(model);
    if (model == ModelFlask || model == ModelFlaskEmpty) return part != Head ? Grid({1, 1, 1}) : buildFlask(model == ModelFlask);
    if (model == ModelBow || model == ModelBowDrawn) return part != Head ? Grid({1, 1, 1}) : buildBow(model == ModelBowDrawn);
    if (model == ModelArrow) return part != Head ? Grid({1, 1, 1}) : buildArrow();
    if (model == ModelCrossbow || model == ModelCrossbowSpanned) return part != Head ? Grid({1, 1, 1}) : buildCrossbow(model == ModelCrossbowSpanned);
    if (model == ModelBolt) return part != Head ? Grid({1, 1, 1}) : buildBolt();
    if (model == ModelThrowingKnife) return part != Head ? Grid({1, 1, 1}) : buildThrowingKnife();
    if (model == ModelBomb || model == ModelPoisonPot) return part != Head ? Grid({1, 1, 1}) : buildRound(model == ModelBomb);
    if (model == ModelBurst || model == ModelPoisonBurst) return part != Head ? Grid({1, 1, 1}) : buildBurst(model == ModelPoisonBurst);
    glm::ivec3 s = specs(model)[part].size;
    Grid g(s);
    Block tunic = model == ModelVillager ? Robe : model == ModelBandit ? Leather : Tunic;
    Block hair = model == ModelVillager ? DarkHair : model == ModelBandit ? Hood : Hair;
    for (int z = 0; z < s.z; ++z)
        for (int y = 0; y < s.y; ++y)
            for (int x = 0; x < s.x; ++x) {
                Block b = Skin;
                if (model == ModelDummy) {
                    b = dummyVoxel(part, {x, y, z}, s);
                } else if (model == ModelBoar) {
                    b = boarVoxel(Part(part), {x, y, z}, s);
                } else if (model == ModelWolf) {
                    b = wolfVoxel(part, {x, y, z}, s);
                } else if (model == ModelGuard) {
                    b = guardVoxel(part, {x, y, z}, s);
                } else if (model == ModelChief) {
                    b = chiefVoxel(part, {x, y, z}, s);
                } else if (model == ModelGhoul) {
                    b = ghoulVoxel(part, {x, y, z}, s);
                } else if (model == ModelBarrowLord) {
                    b = barrowLordVoxel(part, {x, y, z}, s);
                } else if (model == ModelIronArmor) {
                    b = armorVoxel(part, {x, y, z}, s);
                } else if (part == Head) {
                    // Hair cap and back, sideburns; two tall eyes on the +Z (front) face. The
                    // bandit's hood is its hair, and a mask covers the lower face.
                    if (y >= 6 || z <= 2 || ((x == 0 || x == s.x - 1) && y >= 3)) b = hair;
                    if (model == ModelBandit && y <= 2 && z >= s.z - 2) b = Hood;
                    if (z == s.z - 1 && (y == 3 || y == 4) && (x == 2 || x == s.x - 3)) b = Eye;
                } else if (part == Torso) {
                    b = y == 2 ? Belt : tunic;
                } else if (part == FootL || part == FootR) {
                    b = Boots;
                }
                g.set({x, y, z}, b);
            }
    return g;
}

static float smooth(float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return a * a * (3 - 2 * a);
}

// The sword arm's pose (ADR-035): the hand on an arc around the right shoulder (`arc` radians from
// straight ahead toward the left hand, `reach` voxels out, `height` voxels up), the blade pitched
// `pitch` forward from upright and turned with the arc plus `yaw`, the torso turned `twist` (toward the
// left hand) and leaning `lean` forward, and the body sunk `dip` voxels; the weapon turned `roll` about
// its own length (an axe's head leads the swing: out to the side for 0, ahead of a chop for -pi/2).
struct Grip { float arc = 0, height = 9, reach = 4, pitch = 1, yaw = 0, twist = 0, lean = 0, dip = 0, roll = 0; };
static Grip mix(const Grip& a, const Grip& b, float t) {
    auto m = [t](float x, float y) { return x + (y - x) * t; };
    return {m(a.arc, b.arc), m(a.height, b.height), m(a.reach, b.reach), m(a.pitch, b.pitch), m(a.yaw, b.yaw),
            m(a.twist, b.twist), m(a.lean, b.lean), m(a.dip, b.dip), m(a.roll, b.roll)};
}
// Each move swings from its windup pose to its follow-through (unarmed: the fist does the same), by
// weapon class (ADR-044).
struct Swing { Grip wind, through; };
using Swings = std::array<Swing, size_t(Move::Count)>;
// A class's table, one row per Move (a missing row would draw at the origin, so it doesn't build).
template <size_t N> constexpr Swings rows(const Swing (&r)[N]) {
    static_assert(N == size_t(Move::Count), "a swing pose for every Move");
    Swings out{};
    for (size_t i = 0; i < N; ++i) out[i] = r[i];
    return out;
}
constexpr Swing kParrySwing{{0.6f, 9, 3, 1, 0, 0.4f, 0, 0.3f}, {-1.3f, 10, 5, 1, 0, -0.4f, 0.1f, 0.6f}};  // the shield hand, across the body and out
static const Swings kSwordSwings = rows({
    {},                                                                                     // none
    {{-1.2f, 13, 5, 0.9f, 0, -0.5f, 0, 0}, {1.2f, 5, 5.5f, 2.0f, 0, 0.5f, 0.2f, 0.5f}},     // light 1: diagonal, high right to low left
    {{1.3f, 10, 4, 1.4f, 0, 0.5f, 0, 0}, {-1.4f, 8.5f, 6.5f, 1.6f, 0, -0.5f, 0.1f, 0}},     // light 2: backhand
    {{-1.7f, 8, 6.5f, 1.55f, 0, -0.6f, 0, 0.5f}, {1.7f, 8.5f, 6.5f, 1.55f, 0, 0.7f, 0.15f, 0.5f}},  // light 3: wide sweep
    {{-0.3f, 9, 1, 1.57f, 0, -0.4f, -0.1f, 0}, {0, 9, 9.5f, 1.57f, 0, 0.3f, 0.25f, 0.5f}},   // light 4: thrust
    {{-0.6f, 10, 0, 1.57f, 0, -0.7f, -0.2f, 1}, {0, 9, 10.5f, 1.57f, 0, 0.4f, 0.4f, 1.5f}},  // heavy 1: lunging thrust
    {{-0.3f, 17, 2, -0.7f, 0, -0.2f, -0.25f, 0}, {0.1f, 5, 7, 2.3f, 0, 0.1f, 0.45f, 2}},     // heavy 2: overhead chop
    {{-1.6f, 10, 6, 1.5f, 0, -0.6f, 0.3f, 0}, {1.8f, 8, 6.5f, 1.55f, 0, 0.7f, 0.35f, 0.5f}}, // running: leaping sweep
    {{-0.2f, 5, 2, 1.3f, 0, -0.3f, 0.3f, 3}, {0, 10, 9, 1.3f, 0, 0.2f, 0.2f, 0.5f}},         // rolling: rising thrust
    {{-0.2f, 16, 2, -0.4f, 0, -0.2f, -0.2f, 0}, {0.1f, 6, 6.5f, 2.1f, 0, 0.1f, 0.3f, 1}},    // jump light: chop
    {{-0.2f, 18, 1.5f, -0.9f, 0, -0.2f, -0.3f, 0}, {0.1f, 3.5f, 7, 2.6f, 0, 0.1f, 0.55f, 3}}, // jump heavy: big chop
    {{1.0f, 5, 5, 2.0f, 0, 0.5f, 0.1f, 1}, {-1.2f, 14, 5, 0.6f, 0, -0.6f, -0.05f, 0}},       // guard counter: rising slash
    {{-0.4f, 10, 0, 1.57f, 0, -0.6f, -0.2f, 0.5f}, {0, 9, 10, 1.57f, 0, 0.3f, 0.4f, 1}},     // riposte: deep thrust
    kParrySwing,
    {{-1.5f, 9.5f, 5, 1.5f, 0, -0.6f, 0.2f, 0.4f}, {1.6f, 8.5f, 6.5f, 1.55f, 0, 0.6f, 0.3f, 0.5f}},  // backstep: a lunging sweep back in
    {},  // drink: its own pose (sip)
    {},  // shot: a bow's own pose (ADR-050)
    {},  // reload: a crossbow's own pose (ADR-052)
    {},  // throw: its own pose (ADR-053)
    {}, {}, {}, {},  // a creature's charge, leap, area attack and grab: their own poses (ADR-057)
});
// Daggers: tight slashes close in, and stabs (thrusts) for most of the rest; ice-pick stabs from above.
static const Swings kDaggerSwings = rows({
    {},
    {{-1.0f, 12, 4, 1.0f, 0, -0.4f, 0, 0}, {0.9f, 7, 5, 1.8f, 0, 0.4f, 0.15f, 0.3f}},           // light 1: quick slash
    {{1.0f, 10, 3.5f, 1.4f, 0, 0.4f, 0, 0}, {-1.1f, 8.5f, 5.5f, 1.6f, 0, -0.4f, 0.1f, 0}},       // light 2: backhand slash
    {{-0.2f, 9, 1, 1.57f, 0, -0.3f, -0.05f, 0}, {0, 9, 8.5f, 1.57f, 0, 0.25f, 0.2f, 0.3f}},      // light 3: stab
    {{-0.3f, 9.5f, 0.5f, 1.57f, 0, -0.5f, -0.1f, 0.5f}, {0, 8.5f, 9.5f, 1.57f, 0, 0.35f, 0.35f, 1}},  // light 4: lunging stab
    {{-0.5f, 10, 0, 1.57f, 0, -0.7f, -0.2f, 1}, {0, 9, 10, 1.57f, 0, 0.4f, 0.45f, 1.5f}},        // heavy 1: lunging stab
    {{-0.1f, 16, 3, 2.7f, 0, -0.2f, -0.2f, 0}, {0.1f, 6, 6, 2.9f, 0, 0.1f, 0.45f, 2}},           // heavy 2: stab down from above
    {{-0.3f, 9, 1, 1.57f, 0, -0.4f, 0.3f, 0}, {0, 9, 9.5f, 1.57f, 0, 0.3f, 0.4f, 0.5f}},         // running: dashing stab
    {{-0.2f, 5, 2, 1.3f, 0, -0.3f, 0.3f, 3}, {0, 10, 9, 1.3f, 0, 0.2f, 0.2f, 0.5f}},             // rolling: rising stab
    {{-0.1f, 15, 3, 2.6f, 0, -0.2f, -0.2f, 0}, {0.1f, 5, 6, 2.9f, 0, 0.1f, 0.4f, 1.5f}},         // jump light: plunging stab
    {{-0.1f, 17, 2.5f, 2.6f, 0, -0.2f, -0.3f, 0}, {0.1f, 3.5f, 6.5f, 3.0f, 0, 0.1f, 0.55f, 3}},  // jump heavy
    {{0.9f, 5, 4.5f, 2.0f, 0, 0.5f, 0.1f, 1}, {-1.0f, 13, 4.5f, 0.6f, 0, -0.5f, -0.05f, 0}},     // guard counter: rising slash
    {{-0.4f, 10, 0, 1.57f, 0, -0.6f, -0.2f, 0.5f}, {0, 9, 10, 1.57f, 0, 0.3f, 0.4f, 1}},         // riposte: deep stab
    kParrySwing,
    {{-1.3f, 9.5f, 4.5f, 1.5f, 0, -0.5f, 0.2f, 0.4f}, {1.4f, 8.5f, 6, 1.55f, 0, 0.5f, 0.3f, 0.5f}},  // backstep: a slash back in
    {},  // drink
    {},  // shot: a bow's own pose (ADR-050)
    {},  // reload: a crossbow's own pose (ADR-052)
    {},  // throw: its own pose (ADR-053)
    {}, {}, {}, {},  // a creature's charge, leap, area attack and grab: their own poses (ADR-057)
});
// Axes: chops from high, the head turned to lead (roll), a sweeping cleave, no thrusts.
constexpr float kChop = -1.57f;  // the head ahead of a downward chop
static const Swings kAxeSwings = rows({
    {},
    {{-1.0f, 15, 4, 0.2f, 0, -0.5f, -0.1f, 0, -1.2f}, {0.9f, 4, 6, 2.3f, 0, 0.5f, 0.3f, 1, -1.2f}},       // light 1: diagonal chop
    {{1.2f, 12, 4, 0.9f, 0, 0.5f, 0, 0, -2.3f}, {-1.3f, 6, 6.5f, 2.0f, 0, -0.5f, 0.2f, 0.5f, -2.3f}},     // light 2: backhand chop
    {{-0.3f, 17, 2, -0.6f, 0, -0.2f, -0.25f, 0, kChop}, {0.1f, 4.5f, 7, 2.4f, 0, 0.1f, 0.45f, 1.5f, kChop}},  // light 3: overhead chop
    {{0.9f, 4, 5, 2.2f, 0, 0.5f, 0.2f, 1, 2.3f}, {-1.0f, 15, 5, 0.4f, 0, -0.5f, -0.05f, 0, 2.3f}},        // light 4: rising chop
    {{-0.3f, 18, 1, -0.9f, 0, -0.3f, -0.3f, 0, kChop}, {0.1f, 3.5f, 7.5f, 2.6f, 0, 0.1f, 0.55f, 2.5f, kChop}},  // heavy 1: overhead cleave
    {{-1.8f, 10, 6.5f, 1.5f, 0, -0.8f, 0.1f, 0.5f}, {1.9f, 7, 7, 1.7f, 0, 0.8f, 0.3f, 1}},                // heavy 2: sweeping cleave
    {{-0.3f, 16, 3, -0.4f, 0, -0.3f, 0.2f, 0, kChop}, {0.1f, 5, 7, 2.3f, 0, 0.1f, 0.5f, 1.5f, kChop}},    // running: leaping chop
    {{0.6f, 5, 4, 2.0f, 0, 0.4f, 0.3f, 3, 2.0f}, {-0.6f, 14, 5, 0.5f, 0, -0.4f, 0, 0.5f, 2.0f}},          // rolling: rising chop
    {{-0.2f, 16, 2, -0.4f, 0, -0.2f, -0.2f, 0, kChop}, {0.1f, 6, 6.5f, 2.1f, 0, 0.1f, 0.3f, 1, kChop}},   // jump light: chop
    {{-0.2f, 18, 1.5f, -0.9f, 0, -0.2f, -0.3f, 0, kChop}, {0.1f, 3.5f, 7, 2.6f, 0, 0.1f, 0.55f, 3, kChop}},  // jump heavy: big chop
    {{1.0f, 5, 5, 2.0f, 0, 0.5f, 0.1f, 1, 2.3f}, {-1.2f, 14, 5, 0.6f, 0, -0.6f, -0.05f, 0, 2.3f}},        // guard counter: rising chop
    {{-0.2f, 17, 2, -0.6f, 0, -0.2f, -0.2f, 0.5f, kChop}, {0.1f, 5, 7, 2.5f, 0, 0.1f, 0.5f, 2, kChop}},   // riposte: a hack from above
    kParrySwing,
    {{-1.5f, 9.5f, 5, 1.5f, 0, -0.6f, 0.2f, 0.4f}, {1.6f, 8.5f, 6.5f, 1.55f, 0, 0.6f, 0.3f, 0.5f}},      // backstep: a sweep back in
    {},  // drink
    {},  // shot: a bow's own pose (ADR-050)
    {},  // reload: a crossbow's own pose (ADR-052)
    {},  // throw: its own pose (ADR-053)
    {}, {}, {}, {},  // a creature's charge, leap, area attack and grab: their own poses (ADR-057)
});
// Maces: the axe's chops as smashes, the chain's last one a big overhead smash.
static const Swings kMaceSwings = [] {
    Swings s = kAxeSwings;
    s[size_t(Move::Light4)] = {{-0.2f, 17, 2, -0.7f, 0, -0.3f, -0.25f, 0}, {0.1f, 4, 7, 2.5f, 0, 0.1f, 0.5f, 2}};
    return s;
}();
// Greatswords (ADR-049): wide sweeps both ways, a diagonal, overhead smashes; the blade ends its
// downswings above the ground (it's long).
static const Swings kGreatswordSwings = rows({
    {},
    {{-1.9f, 9, 5.5f, 1.5f, 0, -0.8f, 0, 0.5f}, {1.8f, 8, 6.5f, 1.6f, 0, 0.8f, 0.2f, 1}},             // light 1: wide sweep, right to left
    {{1.6f, 10, 4.5f, 1.45f, 0, 0.7f, 0, 0.5f}, {-1.8f, 8.5f, 6.5f, 1.6f, 0, -0.8f, 0.15f, 1}},       // light 2: sweep back
    {{-1.3f, 15, 4.5f, 0.5f, 0, -0.6f, -0.1f, 0}, {1.3f, 6, 6, 1.75f, 0, 0.6f, 0.3f, 1.2f}},          // light 3: diagonal, high right to low left
    {{-0.3f, 18, 1.5f, -0.8f, 0, -0.3f, -0.3f, 0}, {0.1f, 7, 7.5f, 1.8f, 0, 0.1f, 0.5f, 2}},          // light 4: overhead smash
    {{-0.3f, 19, 1, -1.0f, 0, -0.3f, -0.35f, 0}, {0.1f, 7, 8, 1.8f, 0, 0.1f, 0.6f, 2.5f}},            // heavy 1: great overhead smash
    {{-2.1f, 9, 5, 1.5f, 0, -1.0f, 0.1f, 1}, {2.1f, 7.5f, 7, 1.65f, 0, 1.0f, 0.3f, 1.5f}},            // heavy 2: the widest sweep
    {{-1.8f, 10, 6, 1.5f, 0, -0.7f, 0.3f, 0}, {1.9f, 8, 6.5f, 1.6f, 0, 0.8f, 0.35f, 1}},              // running: leaping sweep
    {{-0.2f, 5, 2, 1.3f, 0, -0.3f, 0.3f, 3}, {0, 10, 9, 1.35f, 0, 0.2f, 0.2f, 0.5f}},                 // rolling: rising thrust
    {{-0.2f, 17, 2, -0.5f, 0, -0.2f, -0.2f, 0}, {0.1f, 7, 7, 1.8f, 0, 0.1f, 0.35f, 1}},               // jump light: chop
    {{-0.2f, 19, 1.5f, -1.0f, 0, -0.2f, -0.3f, 0}, {0.1f, 7.5f, 7.5f, 1.8f, 0, 0.1f, 0.55f, 3}},      // jump heavy: big chop
    {{1.1f, 6, 5, 1.75f, 0, 0.6f, 0.1f, 1}, {-1.3f, 15, 5, 0.5f, 0, -0.7f, -0.05f, 0}},               // guard counter: rising slash
    {{-0.4f, 10, 0, 1.57f, 0, -0.6f, -0.2f, 0.5f}, {0, 9, 10, 1.57f, 0, 0.3f, 0.4f, 1}},              // riposte: deep thrust
    kParrySwing,
    {{-1.7f, 9.5f, 5, 1.5f, 0, -0.7f, 0.2f, 0.4f}, {1.8f, 8.5f, 6.5f, 1.6f, 0, 0.7f, 0.3f, 0.6f}},    // backstep: a sweep back in
    {},  // drink
    {},  // shot: a bow's own pose (ADR-050)
    {},  // reload: a crossbow's own pose (ADR-052)
    {},  // throw: its own pose (ADR-053)
    {}, {}, {}, {},  // a creature's charge, leap, area attack and grab: their own poses (ADR-057)
});
// Greataxes (ADR-049): the axe's chops, bigger, the chain's last one a wide horizontal cleave.
static const Swings kGreataxeSwings = rows({
    {},
    {{-1.1f, 16, 4, 0.1f, 0, -0.6f, -0.15f, 0, -1.2f}, {0.9f, 8.5f, 6.5f, 1.6f, 0, 0.6f, 0.35f, 1.2f, -1.2f}},    // light 1: diagonal chop
    {{1.3f, 12, 4, 0.9f, 0, 0.6f, 0, 0.3f, -2.3f}, {-1.4f, 7.5f, 6.5f, 1.75f, 0, -0.6f, 0.2f, 0.8f, -2.3f}},  // light 2: backhand chop
    {{-0.3f, 18, 1.5f, -0.7f, 0, -0.25f, -0.3f, 0, kChop}, {0.1f, 8.5f, 7.5f, 1.6f, 0, 0.1f, 0.5f, 2, kChop}},   // light 3: overhead chop
    {{-2.0f, 10, 5.5f, 1.5f, 0, -0.9f, 0.1f, 0.5f}, {2.0f, 8, 7, 1.6f, 0, 0.9f, 0.3f, 1.2f}},                 // light 4: wide cleave
    {{-0.3f, 19, 1, -1.0f, 0, -0.35f, -0.35f, 0, kChop}, {0.1f, 8.5f, 8, 1.6f, 0, 0.1f, 0.6f, 2.8f, kChop}},     // heavy 1: great overhead cleave
    {{-2.2f, 9.5f, 5, 1.5f, 0, -1.0f, 0.1f, 1}, {2.2f, 7.5f, 7, 1.65f, 0, 1.0f, 0.35f, 1.5f}},                // heavy 2: the widest cleave
    {{-0.3f, 17, 3, -0.5f, 0, -0.3f, 0.2f, 0, kChop}, {0.1f, 8.5f, 7.5f, 1.6f, 0, 0.1f, 0.5f, 1.5f, kChop}},     // running: leaping chop
    {{0.6f, 7, 4, 1.75f, 0, 0.4f, 0.3f, 3, 2.0f}, {-0.6f, 15, 5, 0.5f, 0, -0.4f, 0, 0.5f, 2.0f}},             // rolling: rising chop
    {{-0.2f, 17, 2, -0.5f, 0, -0.2f, -0.2f, 0, kChop}, {0.1f, 8.5f, 7, 1.6f, 0, 0.1f, 0.35f, 1, kChop}},         // jump light: chop
    {{-0.2f, 19, 1.5f, -1.0f, 0, -0.2f, -0.3f, 0, kChop}, {0.1f, 9, 7.5f, 1.6f, 0, 0.1f, 0.55f, 3, kChop}},  // jump heavy: big chop
    {{1.0f, 7, 5, 1.75f, 0, 0.5f, 0.1f, 1, 2.3f}, {-1.2f, 15, 5, 0.5f, 0, -0.6f, -0.05f, 0, 2.3f}},           // guard counter: rising chop
    {{-0.2f, 18, 2, -0.7f, 0, -0.2f, -0.2f, 0.5f, kChop}, {0.1f, 8.5f, 7.5f, 1.6f, 0, 0.1f, 0.5f, 2, kChop}},    // riposte: a hack from above
    kParrySwing,
    {{-1.7f, 9.5f, 5, 1.5f, 0, -0.7f, 0.2f, 0.4f}, {1.8f, 8.5f, 6.5f, 1.6f, 0, 0.7f, 0.3f, 0.6f}},           // backstep: a cleave back in
    {},  // drink
    {},  // shot: a bow's own pose (ADR-050)
    {},  // reload: a crossbow's own pose (ADR-052)
    {},  // throw: its own pose (ADR-053)
    {}, {}, {}, {},  // a creature's charge, leap, area attack and grab: their own poses (ADR-057)
});
static const Swings& swings(WeaponClass c) {
    switch (c) {
    case WeaponClass::Dagger: return kDaggerSwings;
    case WeaponClass::Axe: return kAxeSwings;
    case WeaponClass::Mace: return kMaceSwings;
    case WeaponClass::Greatsword: return kGreatswordSwings;
    case WeaponClass::Greataxe: return kGreataxeSwings;
    default: return kSwordSwings;  // fists swing as a sword does
    }
}
static const Grip kGuard{0.35f, 10, 4, 0.15f, -1.2f, 0.25f, 0.05f, 0.8f};  // blade upright across the front
static const Grip kShieldGuard{-0.67f, 8, 3.2f, 1.3f, 0.67f, 0.15f, 0.05f, 0.8f};  // behind a shield: the blade forward at the side
// Where a raised shield's hand goes: in front of the chest (a tower shield's lower, so it covers the legs).
static glm::vec3 shieldGuardHand(const Shield& s) { return {1, s.size == ShieldSize::Great ? 7.0f : 9.5f, 5.5f}; }
constexpr float kShieldIdleYaw = 1.2f;  // at the side, its face turned out
// Two-handing (ADR-048): at rest the weapon is held in front, the tip up and forward; the left hand
// below the right on the hilt, pommel() voxels down the weapon's length (in every pose: swings, the guard).
static const Grip kTwoHandIdle{0.7f, 7.5f, 3.5f, 0.75f, -0.55f, 0.2f, 0.05f, 0.3f};
// Where a shield hangs on the back while two-handing: its grip this far from the torso's middle.
constexpr glm::vec3 kShieldBack{0, 1, -1.5f};

// A creature's sweep all around (ADR-057): the blade drawn back low, then held out at the side while the body
// turns a full circle (poseModel turns it).
constexpr Swing kAreaSwing{{-2.0f, 6, 5, 1.5f, 0, -0.9f, 0.15f, 1.5f}, {-1.57f, 8, 7.5f, 1.57f, 0, 0, 0.1f, 1}};

// The swing pose now and how far it has taken over from the idle grip (0 .. 1): it winds up until
// the hold point, strikes through the hit to the follow-through, then settles back.
static std::pair<Grip, float> swingAlong(const Swing& sw, const MoveSpec& m, float u) {
    float through = std::min(m.impact + 0.6f * (m.impact - m.holdAt), m.chain);
    if (u <= m.holdAt) return {sw.wind, smooth(u / m.holdAt)};
    if (u < through) return {mix(sw.wind, sw.through, smooth((u - m.holdAt) / (through - m.holdAt))), 1};
    return {sw.through, 1 - smooth((u - through) / (1 - through))};
}
static std::pair<Grip, float> swingPose(WeaponClass c, Move move, float u) {
    if (move == Move::None || move == Move::Drink || move == Move::Shot || move == Move::Reload || move == Move::Throw || attackMove(move))
        return {{}, 0};
    return swingAlong(swings(c)[size_t(move)], spec(c, move), u);
}
// The same for the body: a chained swing winds up from where the one it cut short was (with a sword:
// fists change hands).
static std::pair<Grip, float> swingPose(const Player& p, bool armed) {
    WeaponClass c = classOf(p.swingWeapon);
    if (attackMove(p.move)) {  // a creature's own (ADR-057): a leap chops as a jump attack, a sweep goes round; a charge or grab, no swing
        if (!armed || (p.move != Move::Leap && p.move != Move::Area)) return {{}, 0};
        return swingAlong(p.move == Move::Leap ? swings(c)[size_t(Move::JumpLight)] : kAreaSwing, spec(p), moveProgress(p));
    }
    auto [g, w] = swingPose(c, p.move, moveProgress(p));
    const MoveSpec& m = spec(c, p.move);
    if (!armed || p.fromMove == Move::None || !p.punching() || moveProgress(p) > m.holdAt) return {g, w};
    if (p.move == Move::Shot || p.move == Move::Reload || p.move == Move::Drink || p.move == Move::Throw) return {{}, 0};  // their own poses: no swing to blend from
    auto [from, fromW] = swingPose(p.fromClass, p.fromMove, p.fromU);  // with the weapon it swung
    float k = smooth(moveProgress(p) / m.holdAt);
    return {mix(fromW > 0 ? from : g, g, k), fromW + (1 - fromW) * k};
}

// A drink from the flask (ADR-047): how far the left hand has raised it (in by the hold point, down
// after the drink frame) and how far it's tipped to the lips (from halfway up to the drink frame).
struct Sip { float raise = 0, tip = 0; };
static Sip sip(const Player& p) {
    if (p.move != Move::Drink) return {};
    float u = moveProgress(p), hold = kDrinkSpec.holdAt, at = kDrinkSpec.impact, down = smooth((1 - u) / (1 - at - 0.1f));
    return {smooth(u / hold) * down, smooth((u - hold * 0.5f) / (at - hold * 0.5f)) * down};
}
constexpr glm::vec3 kSipHand{1.2f, 12.5f, 9};  // the left hand out in front of the face, the neck at the lips

// A throw (ADR-053): how far the left arm is drawn back (by the hold point, gone again at the release) and how
// far it's whipped through (out at the release, down again over the recovery).
struct Toss { float back = 0, through = 0; };
static Toss toss(const Player& p) {
    if (p.move != Move::Throw) return {};
    float u = moveProgress(p), hold = kThrowSpec.holdAt, at = kThrowSpec.impact;
    if (u < hold) return {smooth(u / hold), 0};
    if (u < at) return {1 - smooth((u - hold) / (at - hold)), smooth((u - hold) / (at - hold))};
    return {0, 1 - smooth((u - at) / (1 - at))};
}
constexpr glm::vec3 kTossBack{7.5f, 14.5f, -3.5f}, kTossOut{2, 10.5f, 9.5f};  // the left hand up behind the shoulder; out in front

// A bow (ADR-050): how far it's raised in front (aiming, or a shot: up by the hold point, lowered over
// the last of the recovery), how far the string is drawn (most of the way by the hold point, the rest
// with the draw, let go at the release) and whether an arrow is on the string (until the release; aiming).
struct Nock { float raise = 0, draw = 0; bool arrow = false; };
static Nock nock(const Player& p) {
    Nock n{p.aimBlend, 0, p.aiming && p.arrows > 0 && !p.punching()};
    if (p.move != Move::Shot) return n;
    const MoveSpec& m = spec(p);
    float u = moveProgress(p);
    n.raise = std::max(n.raise, u < m.impact ? smooth(u / m.holdAt) : smooth(2 * (1 - u) / (1 - m.impact)));
    n.draw = u < m.holdAt ? 0.6f * smooth(u / m.holdAt) : u < m.impact ? 0.6f + 0.4f * drawOf(p) : 0;
    n.arrow = p.hitPending;
    return n;
}
// Where the hands go for it (body voxels): the bow out in front of the face, the string hand on the
// string at rest and back by the cheek at full draw; at rest the bow hangs from the left hand, tip forward.
constexpr glm::vec3 kBowHand{-4, 11, 11}, kStringRest{-4, 11, 8}, kStringDrawn{-4, 11.2f, 5};
constexpr float kBowDown = 2.0f;  // radians the bow is pitched forward at rest (raised: upright)

// A crossbow (ADR-052): how far it's up at the shoulder (aiming, or a shot: up by its hold point, down
// over the last of the recovery) and how far tipped down to reload (in by its hold point, back up over
// its last 15%), the right hand's way from the slack string to the latch (the hold point to the seat),
// then on to lay the bolt in.
// Chained (a reload after a shot, a shot pressed through a reload), it eases from where that one was.
struct Span { float raise = 0, tip = 0, pull = 0, lay = 0; };
static float shotRaise(const MoveSpec& m, float u) { return u < m.impact ? smooth(u / m.holdAt) : smooth(2 * (1 - u) / (1 - m.impact)); }
static float reloadTip(const MoveSpec& m, float u) { return smooth(u / m.holdAt) * smooth((1 - u) / 0.15f); }
static Span span(const Player& p) {
    Span n{p.aimBlend};
    float u = moveProgress(p);
    if (p.move == Move::Shot) {
        n.raise = std::max(n.raise, shotRaise(spec(p), u));
        if (p.fromMove == Move::Reload) n.tip = reloadTip(spec(p.fromClass, Move::Reload), p.fromU) * (1 - n.raise);
    } else if (p.move == Move::Reload) {
        const MoveSpec& m = spec(p);
        n.tip = reloadTip(m, u);
        n.pull = smooth((u - m.holdAt) / (m.impact - m.holdAt));
        n.lay = smooth((u - m.impact) / 0.1f);
        if (p.fromMove == Move::Shot) n.raise = std::max(n.raise, shotRaise(spec(p.fromClass, Move::Shot), p.fromU));
        n.raise *= 1 - n.tip;
    }
    return n;
}
// Where it is (body voxels: its grip there, pitched nose down by `pitch` radians) at the shoulder and
// tipped to reload; at rest it hangs from the right hand, nose a little down.
constexpr glm::vec3 kCrossbowRaised{-5.5f, 10, 3.5f}, kCrossbowTipped{-2.5f, 9.5f, 6};
constexpr float kCrossbowRestPitch = 0.5f, kCrossbowTipPitch = 1.0f;

Model heldModel(const Player& p) {
    Model m = heldModel(p.weapon());
    if (m == ModelCrossbow && (p.loaded == p.weapon() || (p.move == Move::Reload && !p.hitPending && p.alive()))) return ModelCrossbowSpanned;
    return m == ModelBow && nock(p).draw >= 0.5f ? ModelBowDrawn : m;
}

glm::mat4 arrowPose(glm::vec3 tip, glm::vec3 dir) {
    glm::vec3 y = glm::normalize(dir), x = glm::cross(y, std::abs(y.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0));
    x = glm::normalize(x);
    glm::vec3 z = glm::cross(x, y);
    glm::mat4 m(glm::vec4(x, 0), glm::vec4(y, 0), glm::vec4(z, 0), glm::vec4(tip, 1));
    return glm::translate(glm::scale(m, glm::vec3(kVoxel)), -kArrowTip);
}

Model throwableModel(ItemId item) {
    switch (itemType(item).thrown.look) {
    case ThrowLook::Bomb: return ModelBomb;
    case ThrowLook::Pot: return ModelPoisonPot;
    default: return ModelThrowingKnife;
    }
}
Model burstModel(ItemId item) { return itemType(item).thrown.look == ThrowLook::Pot ? ModelPoisonBurst : ModelBurst; }

glm::mat4 tumblePose(glm::vec3 at, float spin) {
    glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1), at), spin, glm::normalize(glm::vec3(1, 0, 0.4f)));
    return glm::translate(glm::scale(m, glm::vec3(kVoxel)), -kRoundMiddle);
}

glm::mat4 burstPose(glm::vec3 at, float radius, float u) {
    float grow = u < 0.7f ? smooth(u / 0.7f) * 0.3f + 0.7f : 1 - smooth((u - 0.7f) / 0.3f);  // out fast, then gone
    glm::mat4 m = glm::scale(glm::translate(glm::mat4(1), at), glm::vec3(2 * radius * grow / 12));  // 12 voxels across, about
    return glm::translate(m, -glm::vec3(6.5f));
}

// How hard the body reels now (0 .. 1): in fast, held, out over the last of it.
static float reel(const Combatant& c) {
    if (c.stagger <= 0 || c.staggerLength <= 0) return 0;
    float in = std::min((c.staggerLength - c.stagger) / 0.08f, 1.0f), out = std::min(c.stagger / std::min(0.3f, c.staggerLength * 0.6f), 1.0f);
    return smooth(in) * smooth(out);
}

// A roll: how tucked the body is (in over the first 15%, out over the last 20%) and how far it has
// turned forward (one full turn, easing in and out).
struct Roll { float tuck = 0, turn = 0; };
static Roll roll(const Body& p) {
    if (!p.rolling() || p.backstep) return {};
    float u = p.rollTime / p.rollLength;
    return {smooth(u / 0.15f) * smooth((1 - u) / 0.2f), 2 * std::numbers::pi_v<float> * smooth(u)};
}

// Procedural walk/breathe/airborne/attack animation as per-part offsets (voxels) and rotations.
static void animate(const Player& p, bool armed, glm::vec3 (&off)[PartCount], glm::vec3 (&rot)[PartCount]) {
    float s = std::sin(p.gaitPhase), c = std::cos(p.gaitPhase);
    float a = p.gaitAmount, air = p.airAmount, a1 = std::min(a, 1.0f);
    float breathe = std::sin(p.breathPhase) * (1 - a1);
    float bob = std::abs(s) * 0.8f * a;

    off[Torso] = {0, bob + breathe * 0.25f, 0};
    rot[Torso] = {0.12f * a1, s * 0.12f * a, 0};
    off[Head] = {0, bob + breathe * 0.35f, 0.6f * a1};
    // Hands counter-swing against the feet; arms go up while airborne.
    off[HandL] = {0, bob + breathe * 0.3f + air * 2.5f, -s * 2.6f * a};
    off[HandR] = {0, bob + breathe * 0.3f + air * 2.5f, s * 2.6f * a};
    // Feet swing forward/back and lift on the forward half of their swing.
    off[FootL] = {0, std::max(0.0f, c) * 1.6f * a + air * 0.8f, s * 3.0f * a + air * 1.2f};
    off[FootR] = {0, std::max(0.0f, -c) * 1.6f * a + air * 0.8f, -s * 3.0f * a - air * 1.2f};
    rot[FootL].x = -s * 0.35f * a;
    rot[FootR].x = s * 0.35f * a;

    if (float sw = p.swimAmount; sw > 0) {  // swimming: a crawl while moving, treading water while still
        float t = p.breathPhase * 2;          // treading: hands sweep out and in at the sides, slow kicks
        glm::vec3 tread[PartCount]{};
        tread[Torso] = {0, breathe * 0.25f, 0};
        tread[Head] = {0, breathe * 0.35f, 0};
        tread[HandL] = {1.5f + 1.2f * std::sin(t), 3, 1.5f + std::cos(t)};
        tread[HandR] = {-1.5f - 1.2f * std::sin(t), 3, 1.5f + std::cos(t)};
        tread[FootL] = {0, 0.8f + 0.6f * std::sin(t), 0};
        tread[FootR] = {0, 0.8f - 0.6f * std::sin(t), 0};
        glm::vec3 crawl[PartCount]{};  // arms circle overhead in turn, feet flutter behind
        float g = p.gaitPhase;  // whole multiples only: it wraps at a turn
        crawl[HandL] = {-1.5f, 4 + 4 * std::sin(g), 4 * std::cos(g)};
        crawl[HandR] = {1.5f, 4 - 4 * std::sin(g), -4 * std::cos(g)};
        crawl[FootL] = {0, 1 + std::sin(g * 3), -1.5f};
        crawl[FootR] = {0, 1 - std::sin(g * 3), -1.5f};
        for (int i = 0; i < PartCount; ++i) {
            off[i] = glm::mix(off[i], glm::mix(tread[i], crawl[i], a1), sw);
            rot[i] = glm::mix(rot[i], glm::vec3(0), sw);
        }
    }

    if (float k = roll(p).tuck; k > 0) {  // rolling: head to the chest, knees up, hands on the shins
        static const glm::vec3 tuck[PartCount] = {{0, -4, 2.5f}, {0, -1, 0.5f}, {-2.3f, 1, 4}, {2.3f, 1, 4}, {0, 3, 2.5f}, {0, 3, 2.5f}};
        for (int i = 0; i < PartCount; ++i) {
            off[i] = glm::mix(off[i], tuck[i], k);
            rot[i] = glm::mix(rot[i], glm::vec3(i == Torso ? 0.5f : 0, 0, 0), k);
        }
    }

    if (p.rolling() && p.backstep) {  // a backstep: a small hop, leaning back, the hands up in front
        float u = p.rollTime / p.rollLength, k = smooth(u / 0.2f) * smooth((1 - u) / 0.35f);
        float hop = 1.5f * std::sin(std::numbers::pi_v<float> * std::min(u / 0.8f, 1.0f));
        for (int i = 0; i < PartCount; ++i) off[i].y += hop;
        rot[Torso].x -= 0.3f * k;
        off[Head] += glm::vec3(0, 0, -1.2f) * k;
        off[HandL] += glm::vec3(0, 2, 2) * k, off[HandR] += glm::vec3(0, 2, 2) * k;
        off[FootL] += glm::vec3(0, 0.5f, 1.5f) * k, off[FootR] += glm::vec3(0, 0.3f, -1.5f) * k;  // landing in a stance
    }

    // The torso turned toward the left hand, leaning forward, the upper body sunk.
    auto posture = [&](float twist, float lean, float dip) {
        rot[Torso].y += twist;
        rot[Head].y += 0.5f * twist;
        off[HandL].z -= 2.5f * twist;
        rot[Torso].x += lean;
        off[Head] += glm::vec3(0, -lean, 2.5f * lean);
        for (int i : {Head, Torso, HandL, HandR}) off[i].y -= dip;
    };
    auto [g, w] = swingPose(p, armed);
    const Shield* shield = shieldOf(p);
    if (w > 0) {  // the swing: the sword hand (unarmed, the fists in turn; a parry, the shield hand or the left fist:
                  // the left one mirrored; a knife's parry, the knife hand) and the body with it
        bool left = p.move == Move::Parry ? shield || !armed : !armed && !p.punchRight;
        float side = left ? -1.0f : 1.0f;
        int hand = left ? HandL : HandR;
        glm::vec3 at(side * (-1.5f + g.reach * std::sin(g.arc)), g.height, 1.5f + g.reach * std::cos(g.arc));
        if (p.holding && p.moveTime >= holdTime(p)) at += glm::vec3(0.25f * std::sin(p.charge * 70), 0.2f * std::cos(p.charge * 55), 0);  // straining
        off[hand] += (at - kHumanoid[hand].center) * w;
        posture(side * g.twist * w, g.lean * w, g.dip * w);
    }
    if (float k = p.guardBlend * (1 - w); k > 0) {  // the guard: shield up, else blade upright in front (unarmed, fists up), braced
        const Grip& gg = shield ? kShieldGuard : kGuard;
        glm::vec3 r = armed ? glm::vec3(-1.5f + gg.reach * std::sin(gg.arc), gg.height, 1.5f + gg.reach * std::cos(gg.arc))
                            : glm::vec3(-2.5f, 11.5f, 4);
        glm::vec3 l = shield ? shieldGuardHand(*shield) : glm::vec3(armed ? 2.0f : 2.5f, armed ? 7.0f : 11.5f, 4);
        off[HandR] += (r - kHumanoid[HandR].center) * k;
        off[HandL] += (l - kHumanoid[HandL].center) * k;
        posture(gg.twist * k, gg.lean * k, gg.dip * k);
        if (p.blockTime < 0.2f) {  // a blocked hit knocks it back
            float jolt = k * (1 - p.blockTime / 0.2f);
            off[HandR].z -= 2 * jolt, off[HandL].z -= 2 * jolt;
            rot[Torso].x -= 0.25f * jolt;
        }
    }
    bool shoots = armed && ranged(p), bow = shoots && !loads(p);  // (a crossbow: poseModel puts the hands on it, ADR-052)
    if (float k = armed && !shoots ? p.gripBlend * (1 - w) * (1 - p.guardBlend) * (1 - roll(p).tuck) * (1 - p.swimAmount) : 0; k > 0) {  // two-handed at rest: the weapon in front
        const Grip& gg = kTwoHandIdle;
        glm::vec3 r(-1.5f + gg.reach * std::sin(gg.arc), gg.height, 1.5f + gg.reach * std::cos(gg.arc));
        off[HandR] += (r - kHumanoid[HandR].center) * k;
        posture(gg.twist * k, gg.lean * k, gg.dip * k);
    }
    if (float k = bow ? nock(p).raise * (1 - roll(p).tuck) * (1 - p.swimAmount) : 0; k > 0) {  // a bow raised (ADR-050): side on, drawing
        posture(0.45f * k, 0.05f * k, 0);
        off[HandL] = glm::mix(off[HandL], kBowHand - kHumanoid[HandL].center, k);
        off[HandR] = glm::mix(off[HandR], glm::mix(kStringRest, kStringDrawn, nock(p).draw) - kHumanoid[HandR].center, k);
        if (p.holding && p.charged) off[HandR] += glm::vec3(0.15f * std::sin(p.charge * 60), 0.1f * std::cos(p.charge * 47), 0) * k;  // straining
    }
    if (Sip d = sip(p); d.raise > 0) {  // drinking: the flask to the lips, the head tipped back with it
        off[HandL] = glm::mix(off[HandL], kSipHand - kHumanoid[HandL].center, d.raise);
        rot[Head].x -= 0.4f * d.tip;
        off[Head].z -= 0.5f * d.tip;
        rot[Torso].x -= 0.08f * d.tip;
    }
    if (Toss t = toss(p); t.back + t.through > 0) {  // a throw: the arm drawn back, the body turned with it, then whipped through
        off[HandL] = glm::mix(off[HandL], kTossBack - kHumanoid[HandL].center, t.back);
        off[HandL] = glm::mix(off[HandL], kTossOut - kHumanoid[HandL].center, t.through);
        posture(0.35f * t.back - 0.25f * t.through, -0.05f * t.back + 0.12f * t.through, 0);
    }
    if (p.move == Move::Grab) {  // a grab (ADR-057): both arms raised wide through the windup, thrust out at the catch, held out biting
        const MoveSpec& m = spec(p);
        float u = moveProgress(p), out = smooth((u - m.holdAt) / (m.impact - m.holdAt)), up = smooth(u / m.holdAt) * (1 - out);
        float k = std::max(up, out) * smooth((1 - u) / 0.12f);
        for (auto [h, side] : {std::pair{HandL, 1.0f}, std::pair{HandR, -1.0f}}) {
            glm::vec3 to = glm::mix(glm::vec3(6.5f * side, 21, -1), glm::vec3(3.5f * side, 9.5f, 8), out);  // (up over the head, then out in front)
            off[h] = glm::mix(off[h], to - kHumanoid[h].center, k);
        }
        posture(0, (0.35f * out - 0.15f * up) * k, 0);
        if (u > m.impact) off[Head].z += 1.2f * k * std::max(0.0f, std::sin((u - m.impact) * m.seconds * 14));  // the bites
    }
    if (p.move == Move::Leap && p.hitPending && (moveProgress(p) < spec(p).holdAt || p.hold > 0)) {  // crouched to spring (ADR-057)
        float k = smooth(moveProgress(p) / spec(p).holdAt);
        posture(0, 0.3f * k, 2 * k);
        off[FootL].z += k, off[FootR].z -= k;
    }
    if (p.move == Move::Charge) {  // a charge (ADR-059's follow-ups): through the telegraph a shoulder turned in and dropped, a foot back; leaning into the run
        const MoveSpec& m = spec(p);
        float u = moveProgress(p), k = smooth(u / m.holdAt) * smooth((1 - u) / 0.2f);
        bool running = u >= m.holdAt && p.hold <= 0;  // (and fading out after it: no sinking back into the telegraph)
        posture(0.6f * k, (running ? 0.45f : 0.3f) * k, (running ? 0.5f : 1.5f) * k);
        off[FootL].z += 1.5f * k, off[FootR].z -= 1.5f * k;
        rot[Head].x += 0.25f * k;
    }
    if (float k = p.roar > 0 ? smooth(p.roar / 0.15f) * smooth((1 - p.roar) / 0.15f) : 0; k > 0) {  // a boss's roar (ADR-059): arms flung wide, head back
        for (auto [h, side] : {std::pair{HandL, 1.0f}, std::pair{HandR, -1.0f}})
            off[h] = glm::mix(off[h], glm::vec3(8 * side, 17, 1) - kHumanoid[h].center, k);
        posture(0, -0.2f * k, 1 * k);
        rot[Head].x -= 0.5f * k;
        off[Head].z -= 0.8f * k;
    }
    if (float r = reel(p); r > 0) {
        if (p.broken) {  // stance broken: sunk to a knee, hunched, arms hanging
            posture(0, 0.6f * r, 4 * r);
            off[Head] += glm::vec3(0, -1, 1) * r;
            off[HandL] += glm::vec3(-1, -1.5f, 2) * r, off[HandR] += glm::vec3(1, -1.5f, 2) * r;
        } else {  // a flinch: rocked back, head thrown back, arms up
            rot[Torso].x -= 0.35f * r;
            off[Head] += glm::vec3(0, 0.5f, -1.2f) * r;
            off[HandL].y += 1.5f * r, off[HandR].y += 1.5f * r;
        }
    }
}

// A quadruped's gore: the head draws back through the windup, lunges out at the hit, comes back.
static float gore(const Player& p) {
    if (!p.punching()) return 0;
    const MoveSpec& m = spec(p);
    float u = moveProgress(p);
    if (u <= m.holdAt) return -0.5f * smooth(u / m.holdAt);
    if (u <= m.impact) return -0.5f + 1.5f * smooth((u - m.holdAt) / (m.impact - m.holdAt));
    return 1 - smooth((u - m.impact) / (1 - m.impact));
}

// Trot (diagonal legs together), breathing, and a goring head thrust.
static void animateBoar(const Player& p, glm::vec3 (&off)[PartCount], glm::vec3 (&rot)[PartCount]) {
    float s = std::sin(p.gaitPhase * 1.5f), c = std::cos(p.gaitPhase * 1.5f);  // shorter legs, quicker steps
    float a = std::min(p.gaitAmount, 1.4f), breathe = std::sin(p.breathPhase) * (1 - std::min(a, 1.0f));
    float bob = std::abs(s) * 0.4f * a + breathe * 0.2f;
    off[Torso] = {0, bob, 0};
    off[Head] = {0, bob + std::sin(p.gaitPhase) * 0.3f * a, 0};
    for (auto [leg, sign] : {std::pair{HandL, 1.0f}, std::pair{FootR, 1.0f}, std::pair{HandR, -1.0f}, std::pair{FootL, -1.0f}}) {
        off[leg] = {0, std::max(0.0f, sign * c) * 1.2f * a, sign * s * 2.2f * a};
        rot[leg].x = -sign * s * 0.4f * a;
    }
    float g = gore(p);  // head down and forward, then tusks up at impact
    float low = 0, paw = 0, crouch = 0, stretch = 0;  // a creature attack's (ADR-057)
    if (attackMove(p.move)) {
        const MoveSpec& m = spec(p);
        float u = moveProgress(p), wind = smooth(u / m.holdAt);
        bool telegraph = p.hitPending && (u < m.holdAt || p.hold > 0);
        if (p.move == Move::Charge) {  // head down, a forefoot pawing the ground through the telegraph; head low through the run
            g = 0;
            low = u < m.impact ? wind : smooth((1 - u) / (1 - m.impact));
            paw = telegraph ? wind : 0;
        } else if (p.move == Move::Leap && telegraph) {  // crouched to spring, then stretched out in the air
            crouch = wind, g = 0;
        } else if (p.move == Move::Leap && p.holding) {
            stretch = 1, g = 0.6f;
        }
    }
    off[Head] += glm::vec3(0, -0.8f, 3.0f) * g;
    rot[Head].x = -0.35f * g;
    off[Torso].z += 1.0f * g;
    off[Head] += glm::vec3(0, -1.4f, 1.2f) * low;
    rot[Head].x += 0.35f * low;
    off[Torso].y -= 0.4f * low;
    float scrape = std::sin(p.hold * 16);  // (the move's clock stands still while it holds)
    off[HandL] += glm::vec3(0, std::max(0.0f, scrape) * 1.2f, scrape * 1.5f) * paw;
    for (int i : {Head, Torso}) off[i].y -= 1.5f * crouch;
    off[FootL].z -= 0.8f * crouch, off[FootR].z -= 0.8f * crouch;
    for (int i : {HandL, HandR}) off[i].z += 2.5f * stretch;
    for (int i : {FootL, FootR}) off[i].z -= 2.5f * stretch;
    rot[Torso].x -= 0.15f * stretch;
    if (float r = reel(p); r > 0) {  // a flinch throws the head up; a stance break drops the body low
        if (p.broken) {
            for (int i : {Head, Torso}) off[i].y -= 2.5f * r;
            off[Head] += glm::vec3(0, -1, 0) * r;
        } else {
            off[Head] += glm::vec3(0, 1, -1.5f) * r;
            rot[Head].x -= 0.3f * r;
        }
    }
}

std::array<glm::mat4, PartCount> poseModel(const Player& p, Model model, glm::mat4* held, float alpha, glm::mat4* shieldPose,
                                           glm::mat4* flask, glm::mat4* arrow) {
    bool armed = model != ModelDummy && !quadruped(model) && p.weapon() != NoItem;
    bool shoots = armed && ranged(p), bow = shoots && !loads(p), crossbow = shoots && loads(p);
    const PartSpec* parts = specs(model);
    glm::vec3 off[PartCount]{}, rot[PartCount]{};  // offsets in voxels, rotations (pitch x, yaw y)
    glm::mat4 tilt(1);  // the dummy above its base: wobbles back when hit, topples when out of health
    if (model == ModelDummy) {
        float fall = std::min(p.deadTime / 0.6f, 1.0f);
        float wobble = 0.3f * std::sin(p.hitTime * 18) * std::exp(-p.hitTime * 5);
        glm::vec3 pivot(0, 2, 0);  // top of the base, in voxels
        tilt = glm::translate(glm::mat4(1), pivot);
        tilt = glm::rotate(tilt, -(fall * fall * 1.45f + wobble), {1, 0, 0});
        tilt = glm::translate(tilt, -pivot);
    } else {
        if (quadruped(model)) animateBoar(p, off, rot);
        else animate(p, armed, off, rot);
        if (!quadruped(model) && p.swimAmount > 0) {  // leans into a crawl, about the waist
            glm::vec3 pivot(0, 6, 0);
            float lean = p.swimAmount * std::min(p.gaitAmount, 1.0f) * 0.6f;
            tilt = glm::translate(glm::mat4(1), pivot);
            tilt = glm::rotate(tilt, lean, {1, 0, 0});
            tilt = glm::translate(tilt, -pivot);
        }
        if (Roll r = roll(p); !quadruped(model) && r.tuck > 0) {  // turns forward about the tucked body's middle
            glm::vec3 pivot(0, 6, 1);
            tilt = glm::translate(glm::mat4(1), pivot);
            tilt = glm::rotate(tilt, r.turn, {1, 0, 0});
            tilt = glm::translate(tilt, -pivot);
        }
        if (p.move == Move::Area) {  // a sweep all around (ADR-057): the body turns a full circle through it
            const MoveSpec& m = spec(p);
            float u = moveProgress(p), through = std::min(m.impact + 0.6f * (m.impact - m.holdAt), 1.0f);
            tilt = tilt * glm::rotate(glm::mat4(1), 2 * std::numbers::pi_v<float> * smooth((u - m.holdAt) / (through - m.holdAt)), {0, 1, 0});
        }
        if (!p.alive()) {  // falls over onto its side
            float fall = std::min(p.deadTime / 0.5f, 1.0f);
            glm::vec3 pivot(0, 4, 0);  // about half a body's thickness up, so it lies on the ground
            tilt = glm::translate(glm::mat4(1), pivot);
            tilt = glm::rotate(tilt, fall * fall * 1.5f, {0, 0, 1});
            tilt = glm::translate(tilt, -pivot);
        }
    }

    // The weapon's turn and pitch from the fist: by the swing, the guard or two-handing's rest pose (a parry leaves it be).
    auto [g, w] = swingPose(p, armed);
    const Shield* shield = model == ModelDummy || quadruped(model) ? nullptr : shieldOf(p);
    float guard = p.guardBlend * (1 - w), rest = armed ? p.gripBlend * (1 - w) * (1 - p.guardBlend) * (1 - roll(p).tuck) * (1 - p.swimAmount) : 0;
    const Grip& gg = shield ? kShieldGuard : kGuard;
    float sw = p.move == Move::Parry && shield ? 0 : w;  // a knife's parry sweeps the blade across
    float yaw = (g.arc + g.yaw) * sw + (gg.arc + gg.yaw) * guard + (kTwoHandIdle.arc + kTwoHandIdle.yaw) * rest;
    float pitch = 1.0f + (g.pitch - 1.0f) * sw + (gg.pitch - 1.0f) * guard + (kTwoHandIdle.pitch - 1.0f) * rest;
    // A crossbow (ADR-052): from the right hand at rest to the shoulder or tipped to reload, the hands put on it.
    glm::mat4 crossbowAt(1);
    if (crossbow) {
        Span n = span(p);
        float k = std::min(n.raise + n.tip, 1.0f) * (1 - roll(p).tuck) * (1 - p.swimAmount) * (1 - sip(p).raise), tipped = n.tip / std::max(n.raise + n.tip, 1e-4f);
        glm::vec3 at = glm::mix(parts[HandR].center + off[HandR], glm::mix(kCrossbowRaised, kCrossbowTipped, tipped), k);
        crossbowAt = glm::translate(glm::mat4(1), at);
        crossbowAt = glm::rotate(crossbowAt, glm::mix(kCrossbowRestPitch, tipped * kCrossbowTipPitch, k), {1, 0, 0});
        crossbowAt = glm::translate(crossbowAt, -kCrossbowGrip);
        glm::vec3 reload = n.lay > 0 ? glm::mix(kStringLatch, kBoltTip - glm::vec3(0, 0, 3), n.lay) : glm::mix(kStringSlack, kStringLatch, n.pull);
        glm::vec3 right = glm::mix(kCrossbowGrip, reload, tipped);
        off[HandR] = glm::mix(off[HandR], glm::vec3(crossbowAt * glm::vec4(right, 1)) - parts[HandR].center, k);
        off[HandL] = glm::mix(off[HandL], glm::vec3(crossbowAt * glm::vec4(kCrossbowFore, 1)) - parts[HandL].center, k);
    }
    Toss thrown = toss(p);
    if (float k = armed && !shoots ? p.gripBlend * (1 - sip(p).raise) * (1 - std::max(thrown.back, thrown.through)) * (1 - roll(p).tuck) * (1 - p.swimAmount) : 0; k > 0) {
        // Two-handing (ADR-048): the left hand below the right on the hilt, down the weapon's length.
        glm::vec3 along(std::sin(pitch) * std::sin(yaw), std::cos(pitch), std::sin(pitch) * std::cos(yaw));
        glm::vec3 at = parts[HandR].center + off[HandR] - pommel(heldModel(p.weapon())) * along;
        off[HandL] = glm::mix(off[HandL], at - parts[HandL].center, k);
    }

    glm::mat4 base = glm::translate(glm::mat4(1), drawPos(p, alpha));
    base = glm::rotate(base, drawYaw(p, alpha), {0, 1, 0});
    base = glm::scale(base, kVoxel * drawScale(model));
    std::array<glm::mat4, PartCount> out;
    for (int i = 0; i < PartCount; ++i) {
        bool tilted = !(model == ModelDummy && i == FootR);
        glm::mat4 m = glm::translate(tilted ? base * tilt : base, parts[i].center + off[i]);
        m = glm::rotate(m, rot[i].y, {0, 1, 0});
        m = glm::rotate(m, rot[i].x, {1, 0, 0});
        out[i] = glm::translate(m, -glm::vec3(parts[i].size) * 0.5f);
    }
    if (roll(p).tuck > 0) {  // rolls on its lowest corner: neither sinks into the ground nor floats over it
        float lowest = 1e9f;
        for (int i = 0; i < PartCount; ++i)
            for (int c = 0; c < 8; ++c) {
                glm::vec3 corner(c & 1 ? parts[i].size.x : 0, c & 2 ? parts[i].size.y : 0, c & 4 ? parts[i].size.z : 0);
                lowest = std::min(lowest, (out[i] * glm::vec4(corner, 1)).y);
            }
        glm::mat4 lift = glm::translate(glm::mat4(1), {0, base[3].y - lowest, 0});
        for (glm::mat4& m : out) m = lift * m;
        base = lift * base;
    }
    if (held && bow) {  // in the left hand: upright when raised, pitched forward at the side
        Nock n = nock(p);
        glm::mat4 m = glm::translate(base * tilt, parts[HandL].center + off[HandL]);
        m = glm::rotate(m, kBowDown * (1 - n.raise), {1, 0, 0});
        *held = glm::translate(m, -bowGrip(heldModel(p) == ModelBowDrawn));
        if (arrow && n.arrow) {  // nocked at the string hand, through the bow hand
            glm::vec3 nockAt = parts[HandR].center + off[HandR], through = parts[HandL].center + off[HandL];
            glm::mat4 world = base * tilt;
            glm::vec3 a = world * glm::vec4(nockAt, 1), b = world * glm::vec4(through, 1);
            if (glm::length(b - a) > 1e-4f) *arrow = arrowPose(a + glm::normalize(b - a) * (10 * kVoxel), glm::normalize(b - a));
        }
    } else if (held && crossbow) {  // ADR-052: where the hands put it; a bolt in the groove
        *held = base * tilt * crossbowAt;
        if (arrow && p.loaded == p.weapon()) *arrow = base * tilt * crossbowAt * glm::inverse(glm::scale(glm::mat4(1), glm::vec3(kVoxel))) *
                                                       arrowPose(kBoltTip * kVoxel, {0, 0, 1});
    } else if (held && armed) {  // blade tilted forward from the fist
        glm::mat4 m = glm::translate(base * tilt, parts[HandR].center + off[HandR]);
        m = glm::rotate(m, yaw, {0, 1, 0});
        m = glm::rotate(m, pitch, {1, 0, 0});
        m = glm::rotate(m, g.roll * sw, {0, 1, 0});
        Model model = heldModel(p.weapon());
        *held = glm::translate(m, -heldGrip(model));
    }
    if (shieldPose && shield) {  // its face out at the side, turned forward by the guard, swept along a parry's arc
        float yaw = kShieldIdleYaw * (1 - guard);
        if (p.move == Move::Parry) yaw += (-g.arc - yaw) * w;
        glm::vec3 hand = parts[HandL].center + off[HandL];
        if (Sip d = sip(p); d.raise > 0) hand = glm::mix(hand, parts[HandL].center + glm::vec3(0.5f, 1, 1), d.raise);  // let down at the side to drink
        if (float t = std::max(thrown.back, thrown.through); t > 0) hand = glm::mix(hand, parts[HandL].center + glm::vec3(0.5f, 1, 1), t);  // and to throw
        glm::mat4 m = glm::translate(base * tilt, hand);
        m = glm::rotate(m, yaw, {0, 1, 0});
        *shieldPose = glm::translate(m, -shieldGrid(shieldModel(p.inv.equipped[SlotOffHand])).grip);
    } else if (shieldPose && model != ModelDummy && !quadruped(model) && twoHanding(p) &&
               itemType(p.inv.equipped[SlotOffHand]).shield.size != ShieldSize::None) {  // two-handing: on the back, its face out
        glm::mat4 m = glm::translate(base * tilt, parts[Torso].center + off[Torso]);
        m = glm::rotate(m, rot[Torso].y, {0, 1, 0});
        m = glm::rotate(m, rot[Torso].x, {1, 0, 0});
        m = glm::translate(m, kShieldBack);
        m = glm::rotate(m, std::numbers::pi_v<float>, {0, 1, 0});
        *shieldPose = glm::translate(m, -shieldGrid(shieldModel(p.inv.equipped[SlotOffHand])).grip);
    }
    if (Sip d = sip(p); flask && d.raise > 0 && model != ModelDummy && !quadruped(model)) {  // upright in the hand, its neck tipped to the lips
        glm::mat4 m = glm::translate(base * tilt, parts[HandL].center + off[HandL] + glm::vec3(0, 1, 0));
        m = glm::rotate(m, -2.0f * d.tip, {1, 0, 0});
        *flask = glm::translate(m, -kFlaskGrip);
    }
    if (flask && p.move == Move::Throw && p.throwing && model != ModelDummy && !quadruped(model)) {  // in the hand until it lets go
        glm::mat4 m = glm::translate(base * tilt, parts[HandL].center + off[HandL] + glm::vec3(0, 1, 0.5f));
        if (throwableModel(p.throwing) == ModelThrowingKnife)  // held by the grip, the blade up behind, swung over to point ahead
            *flask = glm::translate(glm::rotate(m, -0.4f - 1.2f * thrown.through, {1, 0, 0}), -glm::vec3(1.5f, 1.5f, 0.5f));
        else
            *flask = glm::translate(m, -kRoundMiddle);
    }
    return out;
}
