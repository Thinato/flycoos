#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "defs.h"
#include "melee.h"
#include "session.h"

#include "stb_vorbis.c"

bool Audio::load(const std::filesystem::path& dir, std::string& error) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file() && it->path().extension() == ".ogg") files.push_back(it->path());
    std::ranges::sort(files);  // takes in name order, whatever the folder lists first
    for (const auto& path : files) {
        int channels = 0, rate = 0;
        short* data = nullptr;
        int frames = stb_vorbis_decode_filename(path.string().c_str(), &channels, &rate, &data);
        if (frames <= 0 || channels < 1 || channels > 2) {
            std::free(data);
            error += (error.empty() ? "" : "; ") + path.string() + ": not a mono or stereo Ogg Vorbis file";
            continue;
        }
        Clip clip;
        clip.channels = channels, clip.rate = rate;
        clip.samples.assign(data, data + size_t(frames) * size_t(channels));
        std::free(data);
        std::string name = path.stem().string();  // step_grass_3 -> step_grass
        if (size_t cut = name.find_last_not_of("0123456789"); cut != std::string::npos && cut + 1 < name.size() && name[cut] == '_')
            name.resize(cut);
        groups[name].clips.push_back(std::move(clip));
    }
    return error.empty();
}

int Audio::play(std::string_view name, float gain, float pan, float pitch, Bus onBus, bool loop, float delay) {
    std::lock_guard hold(mutex);
    auto it = groups.find(name);
    if (it == groups.end()) {
        if (std::ranges::find(missing, name) == missing.end()) missing.emplace_back(name);
        return 0;
    }
    Group& g = it->second;
    std::uniform_real_distribution<float> unit(0, 1);
    int pick = int(rng() % g.clips.size());
    if (g.clips.size() > 1 && pick == g.last) pick = (pick + 1 + int(rng() % (g.clips.size() - 1))) % int(g.clips.size());
    g.last = pick;
    if (!loop) {  // no two the same (loops keep their pitch: a bed shouldn't wobble)
        pitch *= 1 + (2 * unit(rng) - 1) * pitchSpread;
        gain *= 1 - unit(rng) * gainSpread;
    }
    const Clip& clip = g.clips[size_t(pick)];
    Voice v{&clip, 0, double(pitch) * clip.rate / kRate, gain, gain, std::clamp(pan, -1.0f, 1.0f), onBus, loop, nextId++};
    v.at = -double(delay) * kRate * v.step;
    if (voices.size() >= size_t(kMaxVoices)) {  // the quietest one-shot makes room, if it's quieter than this one
        auto loudness = [&](const Voice& o) { return o.loop ? 1e9f : o.gain * bus[o.bus]; };
        auto quiet = std::ranges::min_element(voices, {}, loudness);
        if (loudness(*quiet) > loudness(v)) return 0;
        *quiet = v;
    } else {
        voices.push_back(v);
    }
    log.emplace_back(name);
    if (log.size() > 24) log.pop_front();
    return v.id;
}

void Audio::setGain(int id, float gain) {
    std::lock_guard hold(mutex);
    for (Voice& v : voices)
        if (v.id == id) v.target = gain;
}

int Audio::playing() {
    std::lock_guard hold(mutex);
    return int(voices.size());
}

void Audio::mix(float* out, int frames) {
    std::fill(out, out + 2 * size_t(frames), 0.0f);
    std::lock_guard hold(mutex);
    const float master = muted ? 0 : volume.load(), ease = 1 - std::exp(-1.0f / (0.04f * kRate));
    for (Voice& v : voices) {
        const Clip& c = *v.clip;
        const size_t length = c.samples.size() / size_t(c.channels);
        const float level = bus[v.bus] * master, left = std::min(1.0f, 1 - v.pan) * level, right = std::min(1.0f, 1 + v.pan) * level;
        constexpr float kScale = 1 / 32768.0f;
        for (int f = 0; f < frames; ++f) {
            v.gain += (v.target - v.gain) * ease;
            if (v.at < 0) {
                v.at += v.step;
                continue;
            }
            size_t i = size_t(v.at);
            if (i >= length) {
                if (!v.loop) break;
                v.at = std::fmod(v.at, double(length)), i = size_t(v.at);
            }
            size_t j = i + 1 < length ? i + 1 : v.loop ? 0 : i;
            float t = float(v.at - double(i));
            if (c.channels == 1) {
                float s = std::lerp(float(c.samples[i]), float(c.samples[j]), t) * kScale * v.gain;
                out[2 * f] += s * left, out[2 * f + 1] += s * right;
            } else {
                out[2 * f] += std::lerp(float(c.samples[2 * i]), float(c.samples[2 * j]), t) * kScale * v.gain * left;
                out[2 * f + 1] += std::lerp(float(c.samples[2 * i + 1]), float(c.samples[2 * j + 1]), t) * kScale * v.gain * right;
            }
            v.at += v.step;
        }
    }
    std::erase_if(voices, [](const Voice& v) {
        return !v.loop && (v.at >= double(v.clip->samples.size() / size_t(v.clip->channels)) || (v.target <= 0 && v.gain < 1e-4f));
    });
    for (int k = 0; k < 2 * frames; ++k) out[k] = std::tanh(out[k]);  // a soft limiter: loud moments squash, never clip
}

Placed place(const Audio& a, const Listener& ear, glm::vec3 pos) {
    glm::vec3 d = pos - ear.pos;
    float dist = glm::length(d);
    if (dist >= a.hearing) return {};
    float gain = dist <= a.near ? 1 : a.near / dist;
    gain *= std::clamp((a.hearing - dist) / (0.2f * a.hearing), 0.0f, 1.0f);  // fades out toward the edge, no pop
    glm::vec2 flat(d.x, d.z);
    float across = glm::length(flat);
    float pan = across > 1e-3f ? glm::dot(flat / across, ear.right) * 0.75f * std::min(1.0f, across / 3) : 0;  // close by: centered
    return {gain, pan};
}

static const char* surface(Block b) {
    switch (b) {
    case Grass: case Leaves: return "grass";
    case Stone: case Rock: case Cobble: return "stone";
    case Sand: return "sand";
    case Snow: return "snow";
    case Wood: case Planks: case Thatch: case Straw: return "wood";
    case Water: return "water";
    default: return "dirt";
    }
}

std::vector<Layer> layersOf(const Sound& s) {
    std::vector<Layer> out;
    auto add = [&](std::string name, float gain = 1, float pitch = 1, float delay = 0, Bus bus = BusEffects) {
        out.push_back({std::move(name), gain, pitch, delay, bus});
    };
    const EntityType* type = s.kind >= 0 ? &typeOf(EntityKind(s.kind)) : nullptr;
    const std::string voice = type ? type->voice : "player";
    const float pitch = type ? type->voicePitch : 1;
    auto say = [&](const char* what, float gain = 1, float delay = 0) {
        if (!voice.empty()) add(voice + "_" + what, gain, pitch, delay);
    };
    const float others = type ? 0.7f : 1;  // the player's own steps and breath sit over everyone else's
    const bool beast = type && (type->model == ModelBoar || type->model == ModelWolf);
    const std::string step = std::string("step_") + surface(s.ground);
    switch (s.what) {
    case Noise::Step: add(step, (0.3f + 0.3f * std::min(s.amount, 1.4f)) * others, beast ? 1.15f : pitch); break;
    case Noise::Swim: add("swim", 0.6f * others); break;
    case Noise::Jump: add("jump", 0.6f * others); break;
    case Noise::Land: {
        float g = std::clamp((s.amount - 3) / 12, 0.25f, 1.0f);  // harder from higher
        add("land", g * others), add(step, 0.7f * g * others);
        break;
    }
    case Noise::Roll:  // a heavy load's roll thuds down (ADR-039)
        add("roll", 0.9f * others, s.amount >= float(Load::Heavy) ? 0.85f : 1), add(step, 0.5f * others);
        if (s.amount >= float(Load::Heavy)) add("land", 0.5f * others, 0.9f, 0.12f);
        break;
    case Noise::Backstep: add(step, 0.6f * others), add(step, 0.55f * others, 1.05f, 0.14f), add("jump", 0.3f * others); break;  // a hop: off, and down
    case Noise::Splash: add("splash", 0.9f); break;
    case Noise::Swing: {
        if (s.move == Move::Drink) {  // the flask at the lips (ADR-047): the cork comes out
            add("flask_open", 0.6f);
            break;
        }
        if (s.move == Move::Shot) break;  // the string lets go: Loose has it (ADR-050)
        if (s.move == Move::Parry && s.with && defs().items[s.with].shield.size != ShieldSize::None) {
            add("swing_shield", 0.8f);
            break;
        }
        if (beast) {  // a lunge: teeth and tusks, no blade
            say("attack");
            break;
        }
        bool heavy = spec(classOf(s.with), s.move).heavy || s.move == Move::Riposte, small = classOf(s.with) <= WeaponClass::Dagger;  // fists, daggers
        bool great = weaponClass(classOf(s.with)).twoHanded;  // a great weapon's every swing whooshes heavy, deeper (ADR-049)
        add(heavy || great ? "swing_heavy" : small ? "swing_fast" : "swing_light", heavy ? 1 : great ? 0.9f : 0.8f,
            small && !s.with ? 1.15f : great ? 0.85f : 1);
        if (heavy && !type) say("attack", 0.6f);  // the player's effort
        break;
    }
    case Noise::Windup:  // the tell: it's coming; a bow being drawn creaks (ADR-050)
        if (s.move == Move::Shot) add("bow_draw", 0.7f);
        else say("windup", 0.9f);
        break;
    case Noise::Charged:  // a heavy charged; a bow at full draw, a last creak
        if (s.move == Move::Shot) add("bow_draw", 0.45f, 1.2f);
        else add("charge", 0.8f);
        break;
    case Noise::Loose: add("bow_loose", 0.9f); break;
    case Noise::Thunk: {  // an arrow into terrain: brighter off stone, duller in earth
        std::string_view on = surface(s.ground);
        add("arrow_hit", 0.8f, on == "stone" ? 1.2f : on == "wood" ? 1 : 0.85f);
        break;
    }
    case Noise::GuardUp: add("guard_up", 0.5f); break;
    case Noise::Hurt:
    case Noise::Death:
        if (s.with && weaponClass(classOf(s.with)).ranged) add(type && type->wooden ? "arrow_hit" : "arrow_flesh");  // an arrow (ADR-050)
        else add(type && type->wooden ? "hit_wood" : s.with && defs().items[s.with].damage != DamageType::Strike ? "hit_blade" : "hit_blunt");
        if (s.amount >= 20) add("hit_heavy", std::min(1.0f, s.amount / 40));
        if (s.what == Noise::Hurt) say("hurt", 0.9f);
        else say("death", 1, 0.05f), add("body_fall", 0.8f, 1, 0.45f);
        break;
    case Noise::Crit: add("hit_crit"); break;
    case Noise::Bleed: add("status_bleed"), add("hit_heavy", 0.5f); break;  // a status took effect (ADR-046)
    case Noise::Poison: add("status_poison"); break;
    case Noise::Frost: add("status_frost"); break;
    case Noise::StanceBreak: add("stance_break"); break;
    case Noise::Parry: add("parry"); break;
    case Noise::Block: {
        const Shield& shield = s.with ? defs().items[s.with].shield : Shield{};
        if (shield.size == ShieldSize::Great) add("block_shield", 1, 0.8f);  // a deeper thud off a tower of wood
        else if (shield.size != ShieldSize::None) add("block_shield");
        else add(s.with ? "block_metal" : "block_arms");
        break;
    }
    case Noise::PerfectGuard: add("block_perfect", 0.9f); break;  // a bright clink over the block: timed right
    case Noise::GuardBreak: add("guard_break"); break;
    case Noise::Bounce: add("bounce"); break;
    case Noise::Loot: add("pickup", 0.8f, 1, 0, BusInterface); break;
    case Noise::Talk: add("talk", 0.6f, 1, 0, BusInterface); break;
    case Noise::Drink: add("flask_drink"), add("flask_drink", 0.8f, 0.95f, 0.3f); break;  // two gulps (ADR-047)
    case Noise::Empty: add("flask_empty", 0.8f), add("flask_empty", 0.7f, 1.05f, 0.18f); break;  // shaken: nothing in it
    case Noise::Rest: add("rest", 0.9f); break;  // the fire flares up
    case Noise::Grip: add("equip", 0.6f, 1.1f); break;  // the hilt taken in both hands, or let go (ADR-048)
    case Noise::Count: break;
    }
    return out;
}

void hear(Audio& a, const Sound& s, const Listener& ear) {
    Placed at = place(a, ear, s.pos);
    for (const Layer& l : layersOf(s))
        if (l.bus == BusInterface) a.play(l.name, l.gain, 0, l.pitch, l.bus, false, l.delay);
        else if (at.gain > 0) a.play(l.name, l.gain * at.gain, at.pan, l.pitch, l.bus, false, l.delay);
}

static constexpr const char* kLoops[] = {"amb_wind", "amb_high", "amb_birds", "amb_night", "amb_sea", "amb_river", "amb_fire"};

// How much water, tree cover and fire is around `at` (0..1 each): a sparse look at the voxels.
static void lookAround(Ambience& amb, const Grid& world, glm::vec3 at) {
    glm::ivec3 c = glm::ivec3(glm::floor(at));
    float water = 0, weight = 0;
    for (int dx = -24; dx <= 24; dx += 4)
        for (int dz = -24; dz <= 24; dz += 4) {
            float w = 1 / (1 + std::hypot(float(dx), float(dz)) / 6);
            weight += w;
            for (int dy = -8; dy <= 2; dy += 2)
                if (world.get(c + glm::ivec3(dx, dy, dz)) == Water) {
                    water += w;
                    break;
                }
        }
    amb.water = std::clamp(water / weight * 2.5f, 0.0f, 1.0f);
    int columns = 0, leafy = 0;
    for (int dx = -16; dx <= 16; dx += 4)
        for (int dz = -16; dz <= 16; dz += 4, ++columns)
            for (int dy = 0; dy <= 14; dy += 2)
                if (world.get(c + glm::ivec3(dx, dy, dz)) == Leaves) {
                    ++leafy;
                    break;
                }
    amb.leaves = std::clamp(float(leafy) / float(columns) * 2.5f, 0.0f, 1.0f);
    float nearest = 99;
    for (int dx = -10; dx <= 10; ++dx)
        for (int dz = -10; dz <= 10; ++dz)
            for (int dy = -3; dy <= 3; ++dy)
                if (world.get(c + glm::ivec3(dx, dy, dz)) == Ember) nearest = std::min(nearest, glm::length(glm::vec3(dx, dy, dz)));
    amb.ember = std::pow(std::clamp(1 - nearest / 10, 0.0f, 1.0f), 1.5f);
}

void updateAmbience(Audio& a, Ambience& amb, const Session& s, const Listener& ear, float dt) {
    if ((amb.lookIn -= dt) <= 0) amb.lookIn = 0.5f, lookAround(amb, s.world, ear.pos);
    float sun = std::sin((s.timeOfDay - 0.25f) * 2 * std::numbers::pi_v<float>);  // as the sky's (renderer.cpp)
    float day = glm::smoothstep(-0.1f, 0.2f, sun), night = 1 - glm::smoothstep(-0.2f, 0.05f, sun);
    float up = std::clamp((ear.pos.y - float(kSeaLevel + 25)) / 45, 0.0f, 1.0f);
    float sea = glm::smoothstep(0.35f, 0.8f, amb.water);  // open water: waves; a little of it: a stream
    const float want[] = {0.45f + 0.25f * up, up, day * (0.25f + 0.75f * amb.leaves) * (1 - up), night * (0.6f + 0.4f * amb.leaves) * (1 - up),
                          sea, amb.water * (1 - sea), amb.ember};
    int* ids[] = {&amb.wind, &amb.high, &amb.birds, &amb.night, &amb.sea, &amb.river, &amb.fire};
    for (size_t i = 0; i < std::size(kLoops); ++i) {
        if (!*ids[i] && a.has(kLoops[i])) *ids[i] = a.play(kLoops[i], 0, 0, 1, BusAmbience, true);
        a.setGain(*ids[i], want[i]);
        amb.gains[i] = want[i];
    }
    if ((amb.callIn -= dt) > 0) return;
    std::uniform_real_distribution<float> unit(0, 1);
    amb.callIn = 5 + 12 * unit(amb.rng);
    float pan = 2 * unit(amb.rng) - 1;
    if (float birds = want[2]; birds > 0.2f) a.play("bird", 0.6f * birds, pan, 1, BusAmbience);
    else if (want[3] > 0.4f && unit(amb.rng) < 0.4f) a.play("owl", 0.5f * want[3], pan, 1, BusAmbience);
}
