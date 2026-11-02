#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <print>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include "player.h"
#include "renderer.h"

constexpr float kDayLength = 600;  // seconds per full day/night cycle

struct Options {
    uint32_t seed = 1337;
    int frames = 0;  // > 0: benchmark mode, exit after N frames and print stats
    float timeOfDay = 0.3f;
    const char* screenshot = nullptr;
    bool fixedLook = false;  // --look: fixed camera angles instead of the benchmark orbit
    float lookYaw = 0, lookPitch = 0;
};

[[noreturn]] static void usage() {
    std::println(stderr, "usage: flycoos [--seed N] [--time 0..1] [--frames N [--screenshot out.bmp] [--look YAW PITCH]]");
    std::exit(2);
}

static Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == "--seed" && hasValue) o.seed = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--frames" && hasValue) o.frames = std::max(0, std::atoi(argv[++i]));
        else if (a == "--time" && hasValue) {
            float t = std::strtof(argv[++i], nullptr);
            if (!std::isfinite(t)) usage();
            o.timeOfDay = t - std::floor(t);  // wrap into [0,1): -0.3 means 0.7
        } else if (a == "--screenshot" && hasValue) o.screenshot = argv[++i];
        else if (a == "--look" && i + 2 < argc) {
            o.fixedLook = true;
            o.lookYaw = glm::radians(std::strtof(argv[++i], nullptr));
            o.lookPitch = glm::radians(std::strtof(argv[++i], nullptr));
        }
        else usage();
    }
    if ((o.screenshot || o.fixedLook) && !o.frames) usage();  // benchmark-only options
    return o;
}

static int run(SDL_Window* window, const Options& opt) {
    const bool benchmark = opt.frames > 0;
    Renderer renderer;
    if (!renderer.init(window, !benchmark)) return 1;

    using Clock = std::chrono::steady_clock;
    auto secondsSince = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
    auto start = Clock::now();
    Grid world(kWorldSize);
    generateTerrain(world, opt.seed);
    double genSeconds = secondsSince(start);
    auto meshStart = Clock::now();
    renderer.uploadWorld(world);
    renderer.uploadCharacter();
    double meshSeconds = secondsSince(meshStart);
    const RenderStats& s = renderer.stats;
    std::println("world {}x{}x{} seed {} | generated {:.3f}s + meshed/uploaded {:.3f}s = {:.3f}s | "
                 "{} opaque + {} water quads ({:.1f} MB) in {} chunks",
                 world.size.x, world.size.y, world.size.z, opt.seed, genSeconds, meshSeconds,
                 genSeconds + meshSeconds, s.opaqueQuads, s.waterQuads,
                 double(s.opaqueQuads + s.waterQuads) * 8 / (1 << 20), s.chunksTotal);

    Player player;
    player.pos = findSpawn(world);
    float camYaw = std::numbers::pi_v<float>, camPitch = 0.45f, camDist = 9;  // behind the player
    if (opt.fixedLook) {  // degrees on the CLI; yaw 0 looks toward -Z, 90 toward -X
        camYaw = opt.lookYaw;
        camPitch = std::clamp(opt.lookPitch, -0.5f, 1.45f);
    }
    glm::vec3 camTarget = player.pos + glm::vec3(0, 1.4f, 0);
    float timeOfDay = opt.timeOfDay;
    if (!benchmark) SDL_SetWindowRelativeMouseMode(window, true);

    std::vector<double> frameMs;
    frameMs.reserve(size_t(opt.frames));
    uint64_t last = SDL_GetTicksNS();
    double elapsed = 0, titleTimer = 0;
    for (int frame = 0;; ++frame) {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {  // don't spin or count frames while hidden
            for (SDL_Event e; SDL_WaitEventTimeout(&e, 100);)
                if (e.type == SDL_EVENT_QUIT) return 0;
            last = SDL_GetTicksNS();
            --frame;
            continue;
        }
        uint64_t now = SDL_GetTicksNS();
        float dt = float(double(now - last) * 1e-9);
        last = now;
        elapsed += dt;
        if (frame > 0) frameMs.push_back(dt * 1000.0);

        for (SDL_Event e; SDL_PollEvent(&e);) {
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)) return 0;
            if (e.type == SDL_EVENT_MOUSE_MOTION && !benchmark) {
                camYaw -= e.motion.xrel * 0.004f;
                camPitch = std::clamp(camPitch + e.motion.yrel * 0.004f, -0.5f, 1.45f);
            }
            if (e.type == SDL_EVENT_MOUSE_WHEEL) camDist = std::clamp(camDist * std::pow(0.9f, e.wheel.y), 3.0f, 25.0f);
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        glm::vec2 forward{-std::sin(camYaw), -std::cos(camYaw)}, right{std::cos(camYaw), -std::sin(camYaw)};
        Input in;
        in.move = forward * float(keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]) +
                  right * float(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]);
        if (glm::length(in.move) > 1) in.move = glm::normalize(in.move);
        in.jump = keys[SDL_SCANCODE_SPACE];
        in.run = keys[SDL_SCANCODE_LSHIFT];
        if (benchmark) in.move = {std::sin(elapsed * 0.4), std::cos(elapsed * 0.4)};  // walk a circle
        updatePlayer(player, world, in, dt);

        if (benchmark && !opt.fixedLook) camYaw -= dt * 0.5f;  // counter-orbit so the benchmark sees the player from all sides
        else timeOfDay = std::fmod(timeOfDay + dt / kDayLength * (keys[SDL_SCANCODE_T] ? 60.0f : 1.0f), 1.0f);

        // Third-person orbit camera; pulled in when terrain blocks the view.
        camTarget += (player.pos + glm::vec3(0, 1.4f, 0) - camTarget) * (1 - std::exp(-15 * dt));
        glm::vec3 offset{std::sin(camYaw) * std::cos(camPitch), std::sin(camPitch), std::cos(camYaw) * std::cos(camPitch)};
        float dist = camDist;
        for (float t = 0.5f; t < camDist; t += 0.1f)
            if (isOpaque(world.get(glm::ivec3(glm::floor(camTarget + offset * t))))) {
                dist = std::max(0.5f, t - 0.3f);
                break;
            }

        FrameParams fp;
        fp.eye = camTarget + offset * dist;
        fp.view = glm::lookAtRH(fp.eye, camTarget, glm::vec3(0, 1, 0));
        fp.fovY = glm::radians(70.0f);
        fp.timeOfDay = timeOfDay;
        fp.pose = posePlayer(player);

        bool lastFrame = benchmark && frame + 1 >= opt.frames;
        if (!renderer.render(fp, lastFrame ? opt.screenshot : nullptr)) return 1;

        titleTimer += dt;
        if (titleTimer > 0.5) {
            titleTimer = 0;
            int minutes = int(timeOfDay * 24 * 60);
            char title[128];
            std::snprintf(title, sizeof title, "flycoos | %.2f ms (%.0f fps) | chunks %d/%d | %02d:%02d",
                          dt * 1000.0, 1.0 / std::max(dt, 1e-6f), s.chunksDrawn, s.chunksTotal, minutes / 60, minutes % 60);
            SDL_SetWindowTitle(window, title);
        }
        if (lastFrame) break;
    }

    // Benchmark summary, skipping warm-up frames (pipeline/drawable warm-up).
    size_t warmup = std::min<size_t>(60, frameMs.size() / 4);
    std::vector<double> ms(frameMs.begin() + long(warmup), frameMs.end());
    if (ms.empty()) return 0;
    double mean = 0;
    for (double v : ms) mean += v;
    mean /= double(ms.size());
    std::ranges::sort(ms);
    std::println("frames {} (after {} warm-up) | mean {:.3f} ms ({:.0f} fps) | p99 {:.3f} ms | max {:.3f} ms | "
                 "chunks drawn {}/{}",
                 ms.size(), warmup, mean, 1000.0 / mean, ms[ms.size() * 99 / 100], ms.back(), s.chunksDrawn,
                 s.chunksTotal);
    return 0;
}

int main(int argc, char** argv) {
    Options opt = parseArgs(argc, argv);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("flycoos", 1600, 900, SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::println(stderr, "window creation failed: {}", SDL_GetError());
        return 1;
    }
    int result = run(window, opt);  // Renderer is destroyed before the window
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
