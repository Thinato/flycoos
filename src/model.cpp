#include "model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>

#include "melee.h"
#include "player.h"

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
static bool quadruped(Model m) { return m == ModelBoar || m == ModelWolf; }
static const PartSpec* specs(Model m) { return m == ModelDummy ? kDummy : m == ModelBoar ? kBoar : m == ModelWolf ? kWolf : kHumanoid; }

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

static glm::vec3 heldGrip(Model m) { return m == ModelMace ? glm::vec3(2.5f, 2.5f, 2.5f) : glm::vec3(2.5f, 2.5f, 0.5f); }

Model heldModel(ItemId weapon) {
    switch (classOf(weapon)) {
    case WeaponClass::Dagger: return ModelDagger;
    case WeaponClass::Axe: return ModelAxe;
    case WeaponClass::Mace: return ModelMace;
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
        return (top && (v.x == 4 || v.x == 5)) || (front && v.y == 6) ? Steel : Plate;
    case Torso:  // cuirass over the tunic's skirt: steel collar and breastplate ridge
        if (v.y < 2) return Tunic;
        if (v.y == 2) return Belt;
        return top || (front && (v.x == 3 || v.x == 4)) ? Steel : Plate;
    default: return front && top ? Steel : Plate;  // gauntlet knuckles, sabaton toe caps
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

Grid buildPart(Model model, Part part) {
    if (model == ModelSword || model == ModelDagger || model == ModelAxe || model == ModelMace)
        return part != Head ? Grid({1, 1, 1})
             : model == ModelSword ? buildSword() : model == ModelDagger ? buildDagger() : model == ModelAxe ? buildAxe() : buildMace();
    if (model == ModelBuckler || model == ModelHeater || model == ModelTower) return part != Head ? Grid({1, 1, 1}) : buildShield(model);
    if (model == ModelFlask || model == ModelFlaskEmpty) return part != Head ? Grid({1, 1, 1}) : buildFlask(model == ModelFlask);
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
});
// Maces: the axe's chops as smashes, the chain's last one a big overhead smash.
static const Swings kMaceSwings = [] {
    Swings s = kAxeSwings;
    s[size_t(Move::Light4)] = {{-0.2f, 17, 2, -0.7f, 0, -0.3f, -0.25f, 0}, {0.1f, 4, 7, 2.5f, 0, 0.1f, 0.5f, 2}};
    return s;
}();
static const Swings& swings(WeaponClass c) {
    switch (c) {
    case WeaponClass::Dagger: return kDaggerSwings;
    case WeaponClass::Axe: return kAxeSwings;
    case WeaponClass::Mace: return kMaceSwings;
    default: return kSwordSwings;  // fists swing as a sword does
    }
}
static const Grip kGuard{0.35f, 10, 4, 0.15f, -1.2f, 0.25f, 0.05f, 0.8f};  // blade upright across the front
static const Grip kShieldGuard{-0.67f, 8, 3.2f, 1.3f, 0.67f, 0.15f, 0.05f, 0.8f};  // behind a shield: the blade forward at the side
// Where a raised shield's hand goes: in front of the chest (a tower shield's lower, so it covers the legs).
static glm::vec3 shieldGuardHand(const Shield& s) { return {1, s.size == ShieldSize::Great ? 7.0f : 9.5f, 5.5f}; }
constexpr float kShieldIdleYaw = 1.2f;  // at the side, its face turned out

// The swing pose now and how far it has taken over from the idle grip (0 .. 1): it winds up until
// the hold point, strikes through the hit to the follow-through, then settles back.
static std::pair<Grip, float> swingPose(WeaponClass c, Move move, float u) {
    if (move == Move::None || move == Move::Drink) return {{}, 0};
    const MoveSpec& m = spec(c, move);
    const Swing& sw = swings(c)[size_t(move)];
    float through = std::min(m.impact + 0.6f * (m.impact - m.holdAt), m.chain);
    if (u <= m.holdAt) return {sw.wind, smooth(u / m.holdAt)};
    if (u < through) return {mix(sw.wind, sw.through, smooth((u - m.holdAt) / (through - m.holdAt))), 1};
    return {sw.through, 1 - smooth((u - through) / (1 - through))};
}
// The same for the body: a chained swing winds up from where the one it cut short was (with a sword:
// fists change hands).
static std::pair<Grip, float> swingPose(const Player& p, bool armed) {
    WeaponClass c = classOf(p.swingWeapon);
    auto [g, w] = swingPose(c, p.move, moveProgress(p));
    const MoveSpec& m = spec(c, p.move);
    if (!armed || p.fromMove == Move::None || !p.punching() || moveProgress(p) > m.holdAt) return {g, w};
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
    if (Sip d = sip(p); d.raise > 0) {  // drinking: the flask to the lips, the head tipped back with it
        off[HandL] = glm::mix(off[HandL], kSipHand - kHumanoid[HandL].center, d.raise);
        rot[Head].x -= 0.4f * d.tip;
        off[Head].z -= 0.5f * d.tip;
        rot[Torso].x -= 0.08f * d.tip;
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
    off[Head] += glm::vec3(0, -0.8f, 3.0f) * g;
    rot[Head].x = -0.35f * g;
    off[Torso].z += 1.0f * g;
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
                                           glm::mat4* flask) {
    bool armed = model != ModelDummy && !quadruped(model) && p.weapon() != NoItem;
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
        if (!p.alive()) {  // falls over onto its side
            float fall = std::min(p.deadTime / 0.5f, 1.0f);
            glm::vec3 pivot(0, 4, 0);  // about half a body's thickness up, so it lies on the ground
            tilt = glm::translate(glm::mat4(1), pivot);
            tilt = glm::rotate(tilt, fall * fall * 1.5f, {0, 0, 1});
            tilt = glm::translate(tilt, -pivot);
        }
    }

    glm::mat4 base = glm::translate(glm::mat4(1), drawPos(p, alpha));
    base = glm::rotate(base, drawYaw(p, alpha), {0, 1, 0});
    // The chief is bigger and bulkier, the ghoul gaunt; only the drawing scales, not the body's box.
    glm::vec3 scale = model == ModelChief ? glm::vec3(1.25f, 1.12f, 1.25f) : model == ModelGhoul ? glm::vec3(0.82f, 1.06f, 0.82f) : glm::vec3(1);
    base = glm::scale(base, kVoxel * scale);
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
    auto [g, w] = swingPose(p, armed);
    const Shield* shield = model == ModelDummy || quadruped(model) ? nullptr : shieldOf(p);
    float guard = p.guardBlend * (1 - w);
    if (held && armed) {  // blade tilted forward from the fist; turned and pitched by the swing or the guard (a parry leaves it be)
        const Grip& gg = shield ? kShieldGuard : kGuard;
        float sw = p.move == Move::Parry && shield ? 0 : w;  // a knife's parry sweeps the blade across
        float yaw = (g.arc + g.yaw) * sw + (gg.arc + gg.yaw) * guard;
        float pitch = 1.0f + (g.pitch - 1.0f) * sw + (gg.pitch - 1.0f) * guard;
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
        glm::mat4 m = glm::translate(base * tilt, hand);
        m = glm::rotate(m, yaw, {0, 1, 0});
        *shieldPose = glm::translate(m, -shieldGrid(shieldModel(p.inv.equipped[SlotOffHand])).grip);
    }
    if (Sip d = sip(p); flask && d.raise > 0 && model != ModelDummy && !quadruped(model)) {  // upright in the hand, its neck tipped to the lips
        glm::mat4 m = glm::translate(base * tilt, parts[HandL].center + off[HandL] + glm::vec3(0, 1, 0));
        m = glm::rotate(m, -2.0f * d.tip, {1, 0, 0});
        *flask = glm::translate(m, -kFlaskGrip);
    }
    return out;
}
