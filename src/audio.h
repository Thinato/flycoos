#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

struct Session;
struct Sound;

// Sound (ADR-037): clips loaded from data/sounds, a mixer the audio device pulls from, and what the
// world's noises (Session::sounds) and its ambience sound like. Nothing here touches the device:
// main.cpp feeds mix() to it, so the core and its tests run silent.

// A decoded .ogg: 16-bit samples, interleaved when stereo.
struct Clip {
    std::vector<int16_t> samples;
    int channels = 1, rate = 48000;
    float seconds() const { return float(samples.size()) / float(channels * rate); }
};

// Each voice plays on a bus, with its own volume (the debug window's sliders).
enum Bus : uint8_t { BusEffects, BusAmbience, BusInterface, BusCount };
inline constexpr const char* kBusNames[] = {"effects", "ambience", "interface"};

// Where sounds are heard from (the player) and which way is right (the camera's).
struct Listener {
    glm::vec3 pos{};
    glm::vec2 right{1, 0};  // XZ unit vector
};

struct Audio {
    static constexpr int kRate = 48000;     // mix() writes this many frames a second, stereo
    static constexpr int kMaxVoices = 48;   // past this, a new sound replaces the quietest
    // A sound's takes: data/sounds/<name>_<n>.ogg (or <name>.ogg). Each play picks one, never the
    // same twice running, at a slightly different pitch and volume.
    struct Group {
        std::vector<Clip> clips;
        int last = -1;
    };
    struct Voice {
        const Clip* clip = nullptr;
        double at = 0, step = 1;  // the frame it's at in the clip (negative: still waiting), frames per output frame
        float gain = 0, target = 0, pan = 0;
        Bus bus = BusEffects;
        bool loop = false;
        int id = 0;
    };

    std::map<std::string, Group, std::less<>> groups;
    // The knobs (debug window). The device's thread reads the first three as it mixes.
    std::atomic<float> volume = 0.8f;
    std::atomic<float> bus[BusCount] = {1.0f, 0.7f, 0.8f};
    std::atomic<bool> muted = false;
    float pitchSpread = 0.06f;  // each play: pitch 1 ± this (Wwise-style randomization)
    float gainSpread = 0.12f;   // and volume 1 - up to this
    float hearing = 45;         // blocks: further off it's silent
    float near = 4;             // blocks: this close it's at full volume, further it falls off as near / distance

    // Loads every .ogg under `dir`; false (and why) if some didn't decode (the rest load). A missing folder is no sounds.
    bool load(const std::filesystem::path& dir, std::string& error);
    bool has(std::string_view group) const { return groups.contains(group); }
    // Starts one of a group's takes; its id (0: no such group, or silent). `delay`: seconds before it starts.
    int play(std::string_view group, float gain = 1, float pan = 0, float pitch = 1, Bus bus = BusEffects, bool loop = false,
             float delay = 0);
    // A playing voice's volume, eased toward over ~40 ms; 0 stops a one-shot early.
    void setGain(int id, float gain);
    // Adds `frames` stereo frames of everything playing to `out` (zeroed first). The device's thread calls it.
    void mix(float* out, int frames);
    int playing();
    std::deque<std::string> log;        // the latest plays, newest last (debug window)
    std::vector<std::string> missing;   // names played that have no takes (debug window)

private:
    std::vector<Voice> voices;
    int nextId = 1;
    std::mutex mutex;  // voices: main plays, the device's thread mixes
    std::minstd_rand rng{1};
};

// Volume and pan (-1 left .. 1 right) of a sound at `pos` for `ear`; volume 0: out of hearing.
struct Placed { float gain = 0, pan = 0; };
Placed place(const Audio& a, const Listener& ear, glm::vec3 pos);
// What one noise plays: layers of sounds at once (a hit is its impact, its weight and a voice), each
// placed where the noise was unless it's the interface's.
struct Layer {
    std::string name;
    float gain = 1, pitch = 1, delay = 0;
    Bus bus = BusEffects;
};
std::vector<Layer> layersOf(const Sound& s);
void hear(Audio& a, const Sound& s, const Listener& ear);

// The world's background: wind (more of it up high), birds by day where there are trees, insects at
// night, waves by open water (a stream by a little), fire near embers, plus single calls now and then.
struct Ambience {
    int wind = 0, high = 0, birds = 0, night = 0, sea = 0, river = 0, fire = 0;  // their looping voices
    float water = 0, leaves = 0, ember = 0;  // 0..1 how much is around, from the latest look
    float lookIn = 0, callIn = 4;            // seconds until the next look around, the next single call
    float gains[7] = {};                     // what each loop was set to (debug window)
    std::minstd_rand rng{7};
};
void updateAmbience(Audio& a, Ambience& amb, const Session& s, const Listener& ear, float dt);
