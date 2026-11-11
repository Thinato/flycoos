#include "fx.h"

#include <algorithm>
#include <cmath>

#include "melee.h"
#include "model.h"

namespace {

// How a kind of bit flies and looks: two colours (each bit takes one), its size, speed and spread, how much
// it goes up, its gravity, and how long it lasts in the air (s) and on the ground (a setting).
struct Look {
    glm::vec3 a, b;
    float size, speed, spread, up, gravity, air;
    bool glow;
};
constexpr Look kBlood{{0.62f, 0.05f, 0.04f}, {0.42f, 0.03f, 0.03f}, 0.1f, 3.5f, 0.7f, 0.7f, 22, 1.5f, false};
constexpr Look kSpark{{1.00f, 0.60f, 0.15f}, {1.00f, 0.95f, 0.75f}, 0.08f, 6.0f, 1.0f, 0.7f, 14, 0, true};
constexpr Look kGraveDust{{0.33f, 0.31f, 0.28f}, {0.80f, 0.78f, 0.70f}, 0.09f, 2.5f, 0.5f, 0.6f, 18, 1.5f, false};  // dust and bone
constexpr Look kPuff{{0.62f, 0.58f, 0.52f}, {0.55f, 0.51f, 0.46f}, 0.06f, 1.5f, 0.6f, 0.4f, 6, 0.5f, false};
glm::vec3 palette(Block b) { return {kPalette[b][0], kPalette[b][1], kPalette[b][2]}; }
Look chips(Block a, Block b) { return {palette(a), palette(b), 0.08f, 3.0f, 0.5f, 0.6f, 20, 1.5f, false}; }

float uniform(std::mt19937& rng, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

void add(Fx& fx, const Particle& p) {
    size_t cap = size_t(std::max(fx.set.cap, 0));
    if (!cap) return;
    if (fx.particles.size() < cap) return fx.particles.push_back(p);
    fx.particles[fx.oldest % fx.particles.size()] = p;  // slots in turn (removals reorder them: not strictly the oldest)
    fx.oldest = (fx.oldest + 1) % cap;
}

// `n` bits of `look` from `at`, flung mostly along `dir` (level; zero: any way), lasting `rest` on the ground.
void fling(Fx& fx, const Look& look, glm::vec3 at, glm::vec3 dir, float n, float rest) {
    int count = int(std::lround(n * fx.set.amount));
    for (int i = 0; i < count; ++i) {
        glm::vec3 r(uniform(fx.rng, -1, 1), uniform(fx.rng, -1, 1), uniform(fx.rng, -1, 1));
        float speed = look.speed * uniform(fx.rng, 0.5f, 1);
        Particle p;
        p.pos = at + r * 0.08f;
        p.vel = (dir + r * look.spread + glm::vec3(0, look.up, 0)) * speed;
        p.size = look.size * uniform(fx.rng, 0.75f, 1.25f);
        p.life = look.glow ? fx.set.sparkSeconds * uniform(fx.rng, 0.5f, 1) : look.air;
        p.rest = rest;
        p.gravity = look.gravity;
        p.colour = uniform(fx.rng, 0, 1) < 0.6f ? look.a : look.b;
        p.glow = look.glow;
        add(fx, p);
    }
}

glm::vec3 level(glm::vec3 v) {
    v.y = 0;
    float l = glm::length(v);
    return l > 1e-3f ? v / l : glm::vec3(0);
}

bool stony(Block b) { return b == Stone || b == Rock || b == Cobble || b == MossyCobble || b == Brick || b == Slate; }

}  // namespace

float drawnSize(const Particle& p) {
    float s = p.glow && !p.resting ? p.size * (1 - 0.6f * p.age / p.life) : p.size;
    return s * std::clamp((p.life - p.age) / (0.25f * p.life), 0.0f, 1.0f);
}

void spawnFx(Fx& fx, const Sound& s, glm::vec3 eye) {
    if (glm::distance(s.pos, eye) > fx.set.reach) return;
    const FxSettings& set = fx.set;
    Model model = s.kind < 0 ? ModelPlayer : typeOf(EntityKind(s.kind)).model;
    glm::vec3 toward = level(s.from - s.pos);  // where the blow came from (a guard: ahead of it)
    glm::vec3 guardAt = s.pos + glm::vec3(0, 1.2f, 0) + toward * 0.35f;  // a shield or blade held up in front
    auto sparks = [&](glm::vec3 at, float n) {  // up and out to the sides, a little back toward the blow: the bodies would hide the rest
        if (set.sparks) fling(fx, kSpark, at, toward * 0.3f, n, 0);
    };
    bool smallShield = s.with && itemType(s.with).shield.size == ShieldSize::Small;  // a buckler is wood
    switch (s.what) {
        case Noise::Hurt: case Noise::Death: case Noise::Crit: {
            // Where the blow lands, halfway to the hitter (at most 0.8 out: a long boar's snout), spraying out of
            // the wound wide and up: the body would hide the far side.
            float reach = std::min(0.5f * glm::length(glm::vec2(s.from.x - s.pos.x, s.from.z - s.pos.z)), 0.8f);
            glm::vec3 at = s.pos + glm::vec3(0, 0.6f * bodyHeight(model), 0) + toward * reach, out = toward * 0.3f;
            float n = s.what == Noise::Death ? 22 : s.what == Noise::Crit ? 14 : 6 + 6 * std::clamp(s.amount / 30, 0.0f, 1.0f);
            Blood blood = s.kind < 0 ? Blood::Red : typeOf(EntityKind(s.kind)).blood;
            if (set.blood) {
                if (blood == Blood::Red) fling(fx, kBlood, at, out, n, set.bloodSeconds);
                else if (blood == Blood::Dust) fling(fx, kGraveDust, at, out, n, set.chipSeconds);
                else fling(fx, chips(Wood, Straw), at, out, n, set.chipSeconds);
            }
            if (s.plated && s.what != Noise::Crit) sparks(at, 2.5f);  // off plate (a crit comes with its hurt, which sparked)
            break;
        }
        case Noise::Block:
            if (!s.with) {  // bare arms
                if (set.chips) fling(fx, kPuff, guardAt, toward, 5, 0);
            } else if (smallShield) {
                if (set.chips) fling(fx, chips(Wood, Planks), guardAt, toward, 6, set.chipSeconds);
                sparks(guardAt, 2);
            } else {
                sparks(guardAt, 10);
            }
            break;
        case Noise::Parry: sparks(guardAt, 18); break;
        case Noise::PerfectGuard: sparks(guardAt, 10); break;
        case Noise::Bounce: sparks(guardAt, 8); break;
        case Noise::GuardBreak:
            sparks(guardAt, 16);
            if (smallShield && set.chips) fling(fx, chips(Wood, Planks), guardAt, toward, 8, set.chipSeconds);
            break;
        case Noise::Thunk:  // an arrow into terrain (a burst's has no block: its puff is its own)
            if (s.ground == Air) break;
            if (set.chips) fling(fx, chips(s.ground, s.ground), s.pos, toward, 5, set.chipSeconds);
            if (stony(s.ground)) sparks(s.pos, 3);
            break;
        default: break;
    }
}

bool swingsBlade(const Player& b) {
    switch (b.move) {
        case Move::None: case Move::Parry: case Move::Drink: case Move::Shot: case Move::Reload: case Move::Throw: return false;
        default: return b.moveTime >= holdTime(b) && b.moveTime <= impactTime(b) + kSmearAfter * moveLength(b);  // the strike, not the windup
    }
}

void trailFx(Fx& fx, const void* body, Model model, const glm::mat4& held, bool swinging, bool frozen) {
    glm::vec3 b0, t0;
    if (!bladeEdge(model, b0, t0)) return;
    glm::vec3 base(held * glm::vec4(b0, 1)), tip(held * glm::vec4(t0, 1));
    auto it = std::ranges::find(fx.trails, body, &Trail::body);
    if (it == fx.trails.end()) {
        fx.trails.push_back({body, {}, base, tip, 0, true, frozen});
        return;
    }
    Trail& t = *it;
    t.seen = true, t.frozen = frozen;
    float moved = glm::distance(tip, t.tip);
    if (moved < 1e-4f) return;  // the pose changes once a tick: frames between see the same edge
    bool fast = swinging && fx.set.trails && moved < 1.5f && moved >= fx.set.trailSpeed * std::max(t.since, 1e-3f);  // (a jump: another body)
    if (fast) {
        bool joined = !t.samples.empty() && t.samples.back().tip == t.tip;
        if (!joined) t.samples.push_back({t.base, t.tip, 0, true});  // a smear starts from where the blade was
        t.samples.push_back({base, tip, 0});
    }
    t.base = base, t.tip = tip, t.since = 0;
}

void updateFx(Fx& fx, const Grid& world, float dt) {
    const FxSettings& set = fx.set;
    dt = std::min(dt, 0.05f);  // a stalled frame mustn't fling bits through the ground
    if (fx.particles.size() > size_t(std::max(set.cap, 0))) fx.particles.resize(size_t(std::max(set.cap, 0))), fx.oldest = 0;
    for (size_t i = 0; i < fx.particles.size();) {
        Particle& p = fx.particles[i];
        p.age += dt;
        if (!p.resting && dt > 0) {
            p.vel.y -= p.gravity * dt;
            glm::vec3 to = p.pos + p.vel * dt;
            glm::ivec3 cell(glm::floor(to));
            if (isOpaque(world.get(cell))) {
                bool landed = cell.y < int(std::floor(p.pos.y));  // came down onto it, not into its side
                if (landed && p.rest > 0) {
                    p.resting = true, p.age = 0, p.life = p.rest, p.vel = {};
                    p.pos = glm::vec3(to.x, float(cell.y + 1) + 0.01f, to.z);
                } else if (landed) {
                    p.age = p.life;  // a spark dies where it lands
                } else if (isOpaque(world.get(glm::ivec3(glm::floor(p.pos))))) {
                    p.age = p.life;  // born inside a wall: gone
                } else {
                    p.vel.x = p.vel.z = 0;  // against a wall: falls down it
                }
            } else {
                p.pos = to;
            }
        }
        if (p.age >= p.life) {
            p = fx.particles.back();
            fx.particles.pop_back();
        } else {
            ++i;
        }
    }
    fx.ribbon.clear();
    for (Trail& t : fx.trails) {
        if (!t.frozen)
            for (TrailSample& s : t.samples) s.age += dt;
        std::erase_if(t.samples, [&](const TrailSample& s) { return s.age >= set.trailSeconds; });
        if (!t.frozen) t.since += dt;  // hit-stop doesn't count against the blade's speed
        // Catmull-Rom between the samples, 4 steps each: a smooth arc from edges a tick apart.
        size_t n = t.samples.size();
        auto at = [&](size_t i) { return t.samples[std::min(i, n - 1)]; };
        auto alpha = [&](float age) { return 0.85f * std::max(0.0f, 1 - age / set.trailSeconds); };
        auto cr = [](glm::vec3 p0, glm::vec3 p1, glm::vec3 p2, glm::vec3 p3, float u) {
            return 0.5f * (2.0f * p1 + (p2 - p0) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u * u + (3.0f * p1 - p0 - 3.0f * p2 + p3) * u * u * u);
        };
        constexpr int kSteps = 4;
        for (size_t i = 0; i + 1 < n; ++i) {
            TrailSample s1 = at(i), s2 = at(i + 1);
            if (s2.start) continue;  // the gap between two swings
            TrailSample s0 = i == 0 || s1.start ? s1 : at(i - 1), s3 = i + 2 < n && !at(i + 2).start ? at(i + 2) : s2;
            for (int k = 0; k < kSteps; ++k) {
                float u0 = float(k) / kSteps, u1 = float(k + 1) / kSteps;
                glm::vec3 b0 = cr(s0.base, s1.base, s2.base, s3.base, u0), t0 = cr(s0.tip, s1.tip, s2.tip, s3.tip, u0);
                glm::vec3 b1 = cr(s0.base, s1.base, s2.base, s3.base, u1), t1 = cr(s0.tip, s1.tip, s2.tip, s3.tip, u1);
                float a0 = alpha(glm::mix(s1.age, s2.age, u0)), a1 = alpha(glm::mix(s1.age, s2.age, u1));
                // Fading toward the hilt as well: the tip's path reads, the inner edge melts away.
                RibbonVertex vb0{b0, a0 * 0.2f}, vt0{t0, a0}, vb1{b1, a1 * 0.2f}, vt1{t1, a1};
                fx.ribbon.insert(fx.ribbon.end(), {vb0, vt0, vt1, vb0, vt1, vb1});
            }
        }
    }
    std::erase_if(fx.trails, [](const Trail& t) { return !t.seen && t.samples.empty(); });
    for (Trail& t : fx.trails) t.seen = t.frozen = false;  // trailFx says so again each frame it's drawn
}
