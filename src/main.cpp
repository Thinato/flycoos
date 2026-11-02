#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <print>
#include <random>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include "debug_ui.h"
#include "entity.h"
#include "hud.h"
#include "inventory_ui.h"
#include "renderer.h"

constexpr float kDayLength = 600;     // seconds per full day/night cycle
constexpr float kCameraRadius = 0.3f;  // camera collision box half-size; > near-plane half-diagonal

struct Options {
    uint32_t seed = 1337;
    int frames = 0;  // > 0: benchmark mode, exit after N frames and print stats
    float timeOfDay = 0.3f;
    const char* screenshot = nullptr;
    bool fixedLook = false;  // --look: fixed camera angles instead of the benchmark orbit
    float lookYaw = 0, lookPitch = 0;
    bool debug = false;  // --debug: start with the debug window open
    RenderSettings render;
};

[[noreturn]] static void usage() {
    std::println(stderr, "usage: flycoos [--seed N] [--time 0..1] [--debug] [--view shaded|quads|chunks|ao|normals|wire]\n"
                         "               [--frames N [--screenshot out.bmp] [--look YAW PITCH]]");
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
        } else if (a == "--debug") o.debug = true;
        else if (a == "--view" && hasValue) {
            std::string_view v = argv[++i];
            constexpr std::string_view names[ViewModeCount] = {"shaded", "quads", "chunks", "ao", "normals"};
            auto it = std::ranges::find(names, v);
            if (v == "wire") o.render.wireframe = true;
            else if (it != std::end(names)) o.render.viewMode = int(it - std::begin(names));
            else usage();
        } else usage();
    }
    if ((o.screenshot || o.fixedLook) && !o.frames) usage();  // benchmark-only options
    return o;
}

static int run(SDL_Window* window, const Options& opt) {
    const bool benchmark = opt.frames > 0;
    Renderer renderer;
    if (!renderer.init(window, !benchmark)) return 1;
    DebugUI ui(window, renderer);  // destroyed before the renderer (needs its GPU device)
    ui.open = opt.debug;
    GameSettings game;
    game.seed = opt.seed;
    game.timeOfDay = opt.timeOfDay;
    RenderSettings render = opt.render;
    DebugActions actions;

    Grid world(kWorldSize);
    auto buildWorld = [&] {
        uint64_t start = SDL_GetTicksNS();
        std::ranges::fill(world.cells, Air);
        generateTerrain(world, game.seed);
        renderer.stats.genSeconds = double(SDL_GetTicksNS() - start) * 1e-9;
        renderer.uploadWorld(world);
        const RenderStats& s = renderer.stats;
        std::println("world {}x{}x{} seed {} | generated {:.3f}s + meshed/uploaded {:.3f}s = {:.3f}s | "
                     "{} opaque + {} water quads ({:.1f} MB) in {} chunks",
                     world.size.x, world.size.y, world.size.z, game.seed, s.genSeconds, s.meshSeconds,
                     s.genSeconds + s.meshSeconds, s.opaqueQuads, s.waterQuads,
                     double(s.opaqueQuads + s.waterQuads) * 8 / (1 << 20), s.chunksTotal);
    };
    buildWorld();
    renderer.uploadModels();
    const RenderStats& s = renderer.stats;

    Player player;
    player.pos = findSpawn(world);
    giveItem(player.inv, Shortsword);
    giveItem(player.inv, Satchel);
    std::vector<Entity> entities = spawnEntities(world, player.pos, game.seed);
    std::mt19937 rng(std::random_device{}());  // damage rolls
    bool punchQueued = false;  // clicked before the swing was ready
    bool pressInGame = false;  // the left button went down while no window was open
    float camYaw = std::numbers::pi_v<float>, camPitch = 0.45f;  // behind the player
    if (opt.fixedLook) {  // degrees on the CLI; yaw 0 looks toward -Z, 90 toward -X
        camYaw = opt.lookYaw;
        camPitch = std::clamp(opt.lookPitch, -0.5f, 1.45f);
    }
    glm::vec3 camTarget = player.pos + glm::vec3(0, 1.4f, 0);
    float camDist = game.cameraDistance;  // actual distance after terrain collision
    bool characterOpen = false, bagsOpen = false;
    // Any open window frees the cursor and stops mouse look and attacks; the keyboard still plays.
    auto menus = [&] { return ui.open || characterOpen || bagsOpen; };
    auto updateMouseMode = [&] { SDL_SetWindowRelativeMouseMode(window, !benchmark && !menus()); };
    updateMouseMode();

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
            ui.processEvent(e);
            if (e.type == SDL_EVENT_QUIT) return 0;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !ui.wantsKeyboard()) {
                if (e.key.key == SDLK_PERIOD) {
                    ui.open = !ui.open;
                    updateMouseMode();
                } else if (e.key.key == SDLK_C) {
                    characterOpen = !characterOpen;
                    updateMouseMode();
                } else if (e.key.key == SDLK_B) {
                    bagsOpen = !bagsOpen;
                    updateMouseMode();
                } else if (e.key.key == SDLK_E) {
                    if (Entity* npc = talkTarget(entities, player)) talk(*npc);
                } else if (e.key.key == SDLK_ESCAPE) {
                    if (!menus()) return 0;  // Esc closes the debug window, then the others, then quits
                    if (ui.open) ui.open = false;
                    else characterOpen = bagsOpen = false;
                    updateMouseMode();
                }
            }
            if (e.type == SDL_EVENT_MOUSE_MOTION && !benchmark && !menus()) {
                camYaw -= e.motion.xrel * game.mouseSensitivity;
                camPitch = std::clamp(camPitch + e.motion.yrel * game.mouseSensitivity, -0.5f, 1.45f);
            }
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && !benchmark)
                punchQueued = pressInGame = !menus();
            if (e.type == SDL_EVENT_MOUSE_WHEEL && !ui.wantsMouse())
                game.cameraDistance = std::clamp(game.cameraDistance * std::pow(0.9f, e.wheel.y), 2.0f, 80.0f);
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        bool gameKeys = !ui.wantsKeyboard();
        auto key = [&](SDL_Scancode k) { return gameKeys && keys[k]; };
        glm::vec2 forward{-std::sin(camYaw), -std::cos(camYaw)}, right{std::cos(camYaw), -std::sin(camYaw)};
        Input in;
        in.move = forward * float(key(SDL_SCANCODE_W) - key(SDL_SCANCODE_S)) +
                  right * float(key(SDL_SCANCODE_D) - key(SDL_SCANCODE_A));
        if (glm::length(in.move) > 1) in.move = glm::normalize(in.move);
        in.jump = key(SDL_SCANCODE_SPACE);
        in.run = key(SDL_SCANCODE_LSHIFT);
        in.descend = key(SDL_SCANCODE_LCTRL);
        if (benchmark) in.move = {std::sin(elapsed * 0.4), std::cos(elapsed * 0.4)};  // walk a circle
        // Left mouse attacks toward where the camera looks; holding it keeps swinging.
        // A press that began in a window (e.g. a drag) doesn't attack once the window closes.
        if (menus()) punchQueued = pressInGame = false;
        bool punchHeld = pressInGame && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
        if ((punchHeld || punchQueued) && (startPunch(player, forward) || !player.alive())) punchQueued = false;
        updatePlayer(player, world, in, dt);
        updateEntities(entities, world, player, dt);
        separate(player, world, entities);
        updateCombat(player, entities, dt, rng);

        if (benchmark && !opt.fixedLook) camYaw -= dt * 0.5f;  // counter-orbit so the benchmark sees the player from all sides
        else if (!benchmark)
            game.timeOfDay = std::fmod(game.timeOfDay + dt / kDayLength * game.timeScale * (key(SDL_SCANCODE_T) ? 60.0f : 1.0f), 1.0f);

        // Third-person orbit camera. A camera-sized box is marched out from the player; the camera
        // snaps in when terrain is in the way and eases back out so it doesn't pop.
        camTarget += (player.pos + glm::vec3(0, 1.4f, 0) - camTarget) * (1 - std::exp(-15 * dt));
        glm::vec3 offset{std::sin(camYaw) * std::cos(camPitch), std::sin(camPitch), std::cos(camYaw) * std::cos(camPitch)};
        bool collide = game.cameraCollision && !player.tuning.noclip;
        float clear = collide ? cameraClearance(world, camTarget, offset, game.cameraDistance, kCameraRadius)
                              : game.cameraDistance;
        camDist = clear < camDist ? clear : camDist + (clear - camDist) * (1 - std::exp(-4 * dt));

        FrameParams fp;
        fp.eye = camTarget + offset * camDist;
        fp.view = glm::lookAtRH(fp.eye, camTarget, glm::vec3(0, 1, 0));
        fp.fovY = glm::radians(game.fovDegrees);
        fp.timeOfDay = game.timeOfDay;
        glm::mat4 held(0);
        fp.models.push_back({ModelPlayer, poseModel(player, ModelPlayer, &held)});
        if (held != glm::mat4(0)) {  // ponytail: the shortsword is the only weapon, so it's the only held model
            ModelInstance sword{ModelSword, {}};
            sword.pose.fill(held);
            fp.models.push_back(sword);
        }
        for (const Entity& npc : entities) fp.models.push_back({npc.type().model, poseModel(npc.body, npc.type().model)});

        bool wasOpen = menus();
        ui.begin(dt, wasOpen);
        if (game.hud) drawHud(fp.view, fp.fovY, fp.eye, world, player, entities, talkTarget(entities, player));
        characterWindow(characterOpen, player);
        bagsWindow(bagsOpen, player);
        ImDrawData* uiDraw = ui.end(game, render, player, entities, s, actions);
        if (menus() != wasOpen) updateMouseMode();  // closed with a window's X button
        bool lastFrame = benchmark && frame + 1 >= opt.frames;
        if (!renderer.render(fp, render, uiDraw, lastFrame ? opt.screenshot : nullptr)) return 1;

        if (actions.regenerate) buildWorld();
        if (actions.regenerate || actions.respawn) {
            player.pos = findSpawn(world);
            player.vel = {};
            player.health = float(maxHealth(player.stats));
            player.deadTime = 0;
            camTarget = player.pos + glm::vec3(0, 1.4f, 0);
        }
        if (actions.regenerate || actions.respawnEntities) entities = spawnEntities(world, player.pos, game.seed);
        actions = {};

        titleTimer += dt;
        if (titleTimer > 0.5) {
            titleTimer = 0;
            int minutes = int(game.timeOfDay * 24 * 60);
            char title[224];
            std::snprintf(title, sizeof title, "flycoos | %.2f ms (%.0f fps) | chunks %d/%d | %02d:%02d | LMB = attack | E = talk | C = character | B = bags | . = debug",
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
