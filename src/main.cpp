#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numbers>
#include <print>
#include <random>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include "commands.h"
#include "debug_ui.h"
#include "editor_ui.h"
#include "entity.h"
#include "hud.h"
#include "inventory_ui.h"
#include "renderer.h"
#include "server.h"

constexpr float kDayLength = 4320;    // seconds per full day/night cycle (72 minutes)
constexpr float kCameraRadius = 0.3f;  // camera collision box half-size; > near-plane half-diagonal
constexpr float kBrushRate = 8;        // editor brush applications per second while the button is held
constexpr float kEditReach = 400;      // blocks; about where fog hides the ground

struct Options {
    uint32_t seed = 1337;
    const char* world = nullptr;  // --world: load this file if it exists, else generate; the editor saves here
    int frames = 0;  // > 0: benchmark mode, exit after N frames and print stats
    float timeOfDay = 0.3f;
    const char* screenshot = nullptr;
    bool fixedLook = false;  // --look: fixed camera angles instead of the benchmark orbit
    float lookYaw = 0, lookPitch = 0;
    bool debug = false;  // --debug: start with the debug window open
    int port = 47800;    // command server (127.0.0.1 only)
    bool serve = true;   // --no-serve turns it off; benchmarks never serve
    RenderSettings render;
};

[[noreturn]] static void usage() {
    std::println(stderr, "usage: flycoos [--world PATH] [--seed N] [--time 0..1] [--debug] [--view shaded|quads|chunks|ao|normals|wire]\n"
                         "               [--port N | --no-serve]\n"
                         "               [--frames N [--screenshot out.bmp] [--look YAW PITCH]]");
    std::exit(2);
}

static Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        bool hasValue = i + 1 < argc;
        if (a == "--seed" && hasValue) o.seed = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--world" && hasValue) o.world = argv[++i];
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
        else if (a == "--port" && hasValue) {
            o.port = std::atoi(argv[++i]);
            if (o.port < 1 || o.port > 65535) usage();
        } else if (a == "--no-serve") o.serve = false;
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
    std::vector<Instance> instances;  // placed prefabs, saved with the world
    std::vector<Marker> markers;      // story markers (spawns, places), saved with the world
    uint32_t worldSeed = game.seed;  // what the world was generated from; saved with it
    auto buildWorld = [&] {
        uint64_t start = SDL_GetTicksNS();
        std::ranges::fill(world.cells, Air);
        worldSeed = game.seed;
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
    Editor editor;
    editor.path = opt.world ? opt.world : "worlds/world.flyw";
    editor.prefabs = loadPrefabs("prefabs");  // next to worlds/, from the working folder
    editor.place.seed = uint32_t(SDL_rand_bits());
    for (const std::string& error : editor.prefabs.errors) std::println(stderr, "prefab {}", error);
    std::error_code ec;
    if (opt.world && std::filesystem::exists(opt.world, ec)) {
        uint64_t start = SDL_GetTicksNS();
        std::optional<std::vector<Marker>> loaded;
        if (std::string error = loadWorld(opt.world, world, worldSeed, instances, loaded); !error.empty()) {
            std::println(stderr, "cannot load world {}: {}", opt.world, error);
            return 1;  // never fall back to generating: the next save would overwrite the file
        }
        markers = loaded ? std::move(*loaded) : defaultMarkers(world, worldSeed);  // older files: the NPCs they always had
        double loadSeconds = double(SDL_GetTicksNS() - start) * 1e-9;
        game.seed = worldSeed;
        renderer.uploadWorld(world);
        const RenderStats& s = renderer.stats;
        std::println("world {} ({:.1f} MB, {}x{}x{}, seed {}) loaded {:.3f}s + meshed/uploaded {:.3f}s | "
                     "{} opaque + {} water quads in {} chunks, {} prefab instances, {} markers",
                     opt.world, double(std::filesystem::file_size(opt.world, ec)) / (1 << 20), world.size.x,
                     world.size.y, world.size.z, worldSeed, loadSeconds, s.meshSeconds, s.opaqueQuads, s.waterQuads,
                     s.chunksTotal, instances.size(), markers.size());
    } else {
        buildWorld();
        markers = defaultMarkers(world, worldSeed);
    }
    renderer.uploadModels();
    const RenderStats& s = renderer.stats;

    auto save = [&] {
        uint64_t start = SDL_GetTicksNS();
        std::string error = saveWorld(editor.path.c_str(), world, worldSeed, instances, markers);
        double seconds = double(SDL_GetTicksNS() - start) * 1e-9;
        editor.status = error.empty() ? std::format("Saved {:.1f} MB in {:.2f} s",
                                                    double(std::filesystem::file_size(editor.path, ec)) / (1 << 20), seconds)
                                      : "Save failed: " + error;
        std::println("{}: {}", editor.path, editor.status);
        if (!error.empty()) return error;
        editor.unsaved = editor.quitArmed = false;
        editor.savedAt = double(SDL_GetTicksNS()) * 1e-9;
        return error;
    };
    Player player;
    player.pos = playerSpawn(world, markers);
    giveItem(player.inv, Shortsword);
    giveItem(player.inv, Satchel);
    std::vector<Entity> entities;
    syncSpawns(entities, world, markers, worldSeed, player.pos);
    // After an edit, undo or redo: remesh what changed, bring spawns in line with the markers and
    // keep the editor's bookkeeping.
    auto edited = [&](Box dirty, uint64_t start, const char* error) {
        editor.opMs = double(SDL_GetTicksNS() - start) * 1e-6;
        editor.error = error;
        if (dirty.empty()) return;
        editor.lastDirty = dirty;
        ++editor.edits;
        renderer.updateWorld(world, dirty);
        syncSpawns(entities, world, markers, worldSeed, player.pos);
        editor.unsaved = true;
        editor.quitArmed = false;
    };
    EditorActions editorActions;
    auto selectMarker = [&](const Marker& m) {  // its settings load into the panel, for Update selected
        editor.marked = m.id;
        editor.mark = editor.markedWas = m;
        std::snprintf(editor.markName, sizeof editor.markName, "%s", m.name.c_str());
        std::string lines;
        for (const std::string& line : m.lines) lines += (lines.empty() ? "" : "\n") + line;
        std::snprintf(editor.markLines, sizeof editor.markLines, "%s", lines.c_str());
    };
    float brushTimer = 0;
    CommandServer server;  // remote commands (the MCP bridge); runs once per frame below
    if (!benchmark && opt.serve && server.start(opt.port)) editor.server = std::format("127.0.0.1:{}", opt.port);

    std::mt19937 rng(std::random_device{}());  // damage rolls
    bool punchQueued = false;  // clicked before the swing was ready
    bool pressInGame = false;  // the left button went down while no window was open
    uint32_t regionId = 0;  // the region the player is in (0: none), since `regionSince` seconds of `elapsed`
    double regionSince = 0;
    float camYaw = std::numbers::pi_v<float>, camPitch = 0.45f;  // behind the player
    if (opt.fixedLook) {  // degrees on the CLI; yaw 0 looks toward -Z, 90 toward -X
        camYaw = opt.lookYaw;
        camPitch = std::clamp(opt.lookPitch, -0.5f, 1.45f);
    }
    glm::vec3 camTarget = player.pos + glm::vec3(0, 1.4f, 0);
    float camDist = game.cameraDistance;  // actual distance after terrain collision
    bool characterOpen = false, bagsOpen = false;
    // Any open window frees the cursor and stops mouse look and attacks; the keyboard still plays.
    auto menus = [&] { return ui.open || characterOpen || bagsOpen || editor.cursor; };
    auto updateMouseMode = [&] { SDL_SetWindowRelativeMouseMode(window, !benchmark && !menus()); };
    updateMouseMode();
    PlayerTuning walking;  // movement settings from before entering the editor
    auto setEditing = [&](bool on) {  // the editor flies with noclip; NPCs and combat pause
        editor.active = on;
        editor.cursor = false;
        editor.corner.reset();
        if (on) {
            walking = player.tuning;
            player.tuning.fly = player.tuning.noclip = true;
        } else {
            player.tuning.fly = walking.fly;
            player.tuning.noclip = walking.noclip;
            player.pos = clearOfTerrain(world, player.pos);  // not buried by an edit
        }
        updateMouseMode();
    };
    // Editor flight to `pos`, facing `lookAt` past the character.
    auto flyTo = [&](glm::vec3 pos, std::optional<glm::vec3> lookAt) {
        if (!editor.active) setEditing(true);
        player.pos = pos;
        player.vel = {};
        camTarget = player.pos + glm::vec3(0, 1.4f, 0);
        if (lookAt) {
            glm::vec3 d = *lookAt - camTarget;
            camYaw = std::atan2(-d.x, -d.z);
            camPitch = std::clamp(std::atan2(-d.y, glm::length(glm::vec2(d.x, d.z))), -0.5f, 1.45f);
        }
    };

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
            // Editor shortcuts (Ctrl or Cmd); undo and redo repeat while held.
            if (e.type == SDL_EVENT_KEY_DOWN && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && !ui.wantsKeyboard()) {
                if (e.key.key == SDLK_Z) ((e.key.mod & SDL_KMOD_SHIFT) ? editorActions.redo : editorActions.undo) = true;
                else if (e.key.key == SDLK_Y) editorActions.redo = true;
                else if (e.key.key == SDLK_S && !e.key.repeat) editorActions.save = true;
            }
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
                } else if (e.key.key == SDLK_E && player.alive()) {  // loot a corpse, else talk
                    if (Entity* corpse = lootTarget(entities, player)) takeLoot(*corpse, player.inv);
                    else if (Entity* npc = talkTarget(entities, player)) talk(*npc);
                } else if (e.key.key == SDLK_TAB) {
                    setEditing(!editor.active);
                } else if (editor.active && e.key.key == SDLK_Q) {
                    editor.cursor = !editor.cursor;
                    updateMouseMode();
                } else if (editor.active && e.key.key >= SDLK_1 && e.key.key <= SDLK_1 + int(Tool::Count) + 1) {
                    int tool = int(e.key.key - SDLK_1);  // the brushes, then Prefab, then Marker
                    editor.prefabTool = tool == int(Tool::Count);
                    editor.markerTool = tool == int(Tool::Count) + 1;
                    if (tool < int(Tool::Count)) editor.brush.tool = Tool(tool);
                    editor.corner.reset();
                } else if (editor.active && editor.prefabTool && e.key.key == SDLK_R) {
                    editor.place.rotation = (editor.place.rotation + 1) % 4;
                } else if (editor.active && editor.prefabTool && editor.selected && (e.key.key == SDLK_DELETE || e.key.key == SDLK_BACKSPACE)) {
                    editorActions.remove = editor.selected;
                } else if (editor.active && editor.markerTool && editor.marked && (e.key.key == SDLK_DELETE || e.key.key == SDLK_BACKSPACE)) {
                    editorActions.unmark = editor.marked;
                } else if (editor.active && (e.key.key == SDLK_LEFTBRACKET || e.key.key == SDLK_RIGHTBRACKET)) {
                    editor.brush.radius = std::clamp(editor.brush.radius + (e.key.key == SDLK_RIGHTBRACKET ? 1 : -1), 0, kMaxBrushRadius);
                } else if (e.key.key == SDLK_ESCAPE) {
                    // Esc closes the debug window, then the others, then quits (twice with unsaved edits).
                    if (!menus() && (!editor.unsaved || editor.quitArmed)) return 0;
                    if (!menus()) editor.quitArmed = true;
                    else if (ui.open) ui.open = false;
                    else characterOpen = bagsOpen = editor.cursor = false;
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
        if (!player.alive() && !editor.active) in = {};  // knocked out
        if ((SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && (key(SDL_SCANCODE_Z) || key(SDL_SCANCODE_Y) || key(SDL_SCANCODE_S)))
            in = {};  // a shortcut (Ctrl+S) isn't movement
        if (benchmark) player.health = float(maxHealth(player.stats));  // a passing fight mustn't change the benchmark's path
        // Left mouse attacks toward where the camera looks; holding it keeps swinging.
        // A press that began in a window (e.g. a drag) doesn't attack once the window closes.
        if (menus()) punchQueued = pressInGame = false;
        bool punchHeld = pressInGame && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
        if (!editor.active && (punchHeld || punchQueued) && (startPunch(player, forward) || !player.alive())) punchQueued = false;
        updatePlayer(player, world, in, dt);
        if (!editor.active) {
            updateEntities(entities, world, player, dt);
            separate(player, world, entities);
            updateCombat(player, entities, dt, rng, game.armorK);
        }

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

        // Editor: LMB applies the brush where the crosshair (the screen center) meets the ground.
        std::optional<RayHit> aim;
        if (editor.active) {
            aim = raycast(world, fp.eye, glm::normalize(camTarget - fp.eye), kEditReach);
            brushTimer -= dt;
            bool fire = punchQueued || (punchHeld && brushTimer <= 0 && editor.brush.tool != Tool::Fill);
            if (editor.prefabTool && aim && (aim->cell != editor.aimedCell || editor.edits != editor.aimedEdits)) {
                const Instance* hit = instanceAt(instances, world, aim->cell);  // scans every instance cell: cached
                editor.aimed = hit ? hit->id : 0;
                editor.aimedCell = aim->cell;
                editor.aimedEdits = editor.edits;
            }
            if (!aim) editor.aimed = 0, editor.aimedCell = glm::ivec3(-1);
            if (editor.markerTool) {  // clicks only: select the marker standing on the aimed voxel, else place one there
                if (punchQueued && aim) {  // on the ground of the aimed column, under trees and not inside a cliff face
                    glm::vec3 spot(float(aim->cell.x) + 0.5f, float(groundUnderTrees(world, aim->cell.x, aim->cell.z) + 1), float(aim->cell.z) + 0.5f);
                    auto near = std::ranges::find_if(markers, [&](const Marker& m) {
                        return glm::distance(glm::vec2(m.pos.x, m.pos.z), glm::vec2(spot.x, spot.z)) < 1.5f && std::abs(m.pos.y - spot.y) < 3;
                    });
                    if (near != markers.end()) {
                        selectMarker(*near);
                    } else {
                        Marker m = editor.mark;
                        m.id = 0;
                        m.name = editor.markName;
                        m.pos = spot;
                        uint64_t start = SDL_GetTicksNS();
                        EditResult r = putMarker(world, markers, editor.history, m);
                        edited(r.dirty, start, r.error);
                        if (!r.error) selectMarker(*std::ranges::find(markers, r.id, &Marker::id));
                    }
                }
            } else if (editor.prefabTool) {  // clicks only: select the aimed instance, else place one
                const Prefab* prefab = editor.prefabs.find(editor.place.prefab);
                if (punchQueued && editor.aimed) {
                    editor.selected = editor.aimed;
                } else if (punchQueued && aim && prefab) {
                    Instance in = editor.place;
                    in.pos = aim->cell;
                    uint64_t start = SDL_GetTicksNS();
                    EditResult r = placePrefab(world, *prefab, in, aim->cell.y, instances, editor.history);
                    edited(r.dirty, start, r.error);
                    if (!r.error) editor.selected = r.id, editor.place.seed = uint32_t(SDL_rand_bits());  // the next one differs
                }
            } else if (fire && aim && editor.brush.tool == Tool::Fill && !editor.corner) {
                editor.corner = fillCell(editor, *aim);
            } else if (fire && aim) {
                brushTimer = 1 / kBrushRate;
                EditOp op = editor.brush;
                if (op.tool == Tool::Fill) {
                    op.min = *editor.corner;
                    op.max = fillCell(editor, *aim);
                    editor.corner.reset();
                }
                if (op.tool == Tool::Flatten && punchQueued) editor.brush.height = op.height = aim->cell.y;  // a stroke flattens to where it starts
                op.center = {aim->cell.x, aim->cell.z};
                uint64_t start = SDL_GetTicksNS();
                EditResult r = applyEdit(world, op, editor.history);
                edited(r.dirty, start, r.error);
            }
            punchQueued = false;
        }
        auto pushModel = [&](const Player& body, Model model) {  // with whatever it holds
            glm::mat4 held(0);
            fp.models.push_back({partModels(model, body.inv), poseModel(body, model, &held)});
            if (held == glm::mat4(0)) return;
            ModelInstance weapon;
            weapon.parts.fill(heldModel(body.weapon()));
            weapon.pose.fill(held);
            fp.models.push_back(weapon);
        };
        pushModel(player, ModelPlayer);
        for (const Entity& npc : entities) pushModel(npc.body, npc.type().model);

        // One remote command per frame, run here so it shares the editor's undo history and remesh
        // path, and its log line and highlight show this frame.
        if (std::optional<std::string> request = server.take()) {
            uint64_t start = SDL_GetTicksNS();
            GameView view{editor.path, worldSeed, editor.unsaved, editor.active, editor.follow, player.pos, fp.eye,
                          glm::degrees(camYaw), glm::degrees(camPitch), game.fovDegrees, game.timeOfDay};
            Command c = runCommand(*request, world, instances, markers, editor.history, editor.prefabs, view);
            nlohmann::json& response = c.response;
            if (!c.dirty.empty()) {
                edited(c.dirty, start, nullptr);
                editor.flash = c.dirty;
                editor.flashAt = double(SDL_GetTicksNS()) * 1e-9;
            }
            if (c.save) {  // to the world's file, or a copy elsewhere (which leaves the unsaved state alone)
                std::string target = c.save->empty() ? editor.path : *c.save;
                std::string error = c.save->empty() ? save() : saveWorld(target.c_str(), world, worldSeed, instances, markers);
                if (error.empty()) response["result"]["path"] = std::filesystem::absolute(target, ec).string();
                else response = {{"id", response["id"]}, {"ok", false}, {"error", "save failed: " + error}};
            }
            if (c.shot) {  // offscreen, from the same frame's models; the window's view is untouched
                FrameParams shotFrame = fp;
                shotFrame.eye = c.shot->eye;
                shotFrame.view = glm::lookAtRH(c.shot->eye, c.shot->target, glm::vec3(0, 1, 0));
                shotFrame.fovY = glm::radians(c.shot->fovDegrees);
                shotFrame.timeOfDay = c.shot->timeOfDay;
                uint64_t shotStart = SDL_GetTicksNS();
                std::vector<uint8_t> rgb;
                std::string path;
                bool rendered = renderer.snapshot(shotFrame, RenderSettings{}, uint32_t(c.shot->width), uint32_t(c.shot->height), rgb);
                double renderMs = double(SDL_GetTicksNS() - shotStart) * 1e-6;  // render + readback, without the PNG
                if (rendered) path = writePng("screenshot", c.shot->width, c.shot->height, rgb.data());
                if (path.empty()) response = {{"id", response["id"]}, {"ok", false}, {"error", "screenshot failed"}};
                else response["result"] = {{"path", path}, {"width", c.shot->width}, {"height", c.shot->height}, {"render_ms", renderMs}};
            }
            if (c.go && editor.follow) flyTo(c.go->pos, c.go->lookAt);
            if (!c.dirty.empty()) c.summary += std::format("  -> changed {}x{}x{}", c.dirty.size().x, c.dirty.size().y, c.dirty.size().z);
            editor.log.push_back({c.summary, double(SDL_GetTicksNS() - start) * 1e-6, response["ok"].get<bool>()});
            if (editor.log.size() > Editor::kLogLines) editor.log.pop_front();
            server.reply(response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));  // never throws on bad UTF-8
        }

        // After the command: it may have changed the markers. The selected one's settings in the panel
        // follow it (an undo, Claude); a removed one is deselected.
        if (auto it = std::ranges::find(markers, editor.marked, &Marker::id); it == markers.end()) editor.marked = 0;
        else if (*it != editor.markedWas) selectMarker(*it);
        const Marker* region = regionAt(markers, player.pos);  // its name shows for a few seconds on entering
        if (uint32_t id = region ? region->id : 0; id != regionId) regionId = id, regionSince = elapsed;
        ui.region = region ? region->name.c_str() : nullptr;
        bool wasOpen = menus();
        ui.begin(dt, wasOpen);
        if (game.hud) {
            const Entity* target = lootTarget(entities, player);
            drawHud(fp.view, fp.fovY, fp.eye, world, player, entities, target ? target : talkTarget(entities, player));
            drawPlaces(fp.view, fp.fovY, player, markers, region, float(elapsed - regionSince));
        }
        drawEditorOverlay(fp.view, fp.fovY, fp.eye, world, instances, markers, editor, aim);
        characterWindow(characterOpen, player, game.armorK);
        bagsWindow(bagsOpen, player);
        editorWindow(editor, instances, markers, aim, s, editorActions);
        ImDrawData* uiDraw = ui.end(game, render, player, entities, s, actions);
        if (menus() != wasOpen) updateMouseMode();  // closed with a window's X button
        bool lastFrame = benchmark && frame + 1 >= opt.frames;
        if (!renderer.render(fp, render, uiDraw, lastFrame ? opt.screenshot : nullptr)) return 1;

        if (editorActions.undo || editorActions.redo) {
            uint64_t start = SDL_GetTicksNS();
            edited(editorActions.undo ? undoEdit(world, editor.history, instances, markers) : redoEdit(world, editor.history, instances, markers),
                   start, nullptr);
        }
        if (editorActions.remove) {
            uint64_t start = SDL_GetTicksNS();
            EditResult r = removePrefab(world, editorActions.remove, instances, editor.history);
            edited(r.dirty, start, r.error);
        }
        if (auto it = std::ranges::find(instances, editorActions.goTo, &Instance::id); it != instances.end()) {
            Box b = instanceBox(*it);  // look at it from the south, above
            glm::vec3 center = glm::vec3(b.lo + b.hi + 1) * 0.5f;
            float size = float(std::max({b.size().x, b.size().y, b.size().z}));
            flyTo(center + glm::vec3(0, size * 0.8f + 4, size * 1.2f + 8), center);
            editor.selected = it->id;
        }
        if (editorActions.unmark) {
            uint64_t start = SDL_GetTicksNS();
            EditResult r = removeMarker(world, markers, editor.history, editorActions.unmark);
            edited(r.dirty, start, r.error);
        }
        if (auto it = std::ranges::find(markers, editor.marked, &Marker::id); editorActions.updateMark && it != markers.end()) {
            Marker m = editor.mark;  // the panel's settings, at the same place, of the same kind
            m.id = it->id;
            m.kind = it->kind;
            m.pos = it->pos;
            m.name = editor.markName;
            if (!takesLines(m)) m.lines = it->lines;  // the panel hides the lines then: keep them (checkMarker decides)
            uint64_t start = SDL_GetTicksNS();
            EditResult r = putMarker(world, markers, editor.history, m);
            edited(r.dirty, start, r.error);
        }
        if (auto it = std::ranges::find(markers, editorActions.markGoTo, &Marker::id); it != markers.end()) {
            flyTo(it->pos + glm::vec3(0, 10, 16), it->pos);  // from the south, above
            selectMarker(*it);
        }
        if (editorActions.save) save();
        editorActions = {};
        if (actions.regenerate) {
            buildWorld();
            editor.history = {};  // its records belong to the old world
            instances.clear();
            markers = defaultMarkers(world, worldSeed);
            editor.marked = 0;
            ++editor.edits;
            editor.corner.reset();
            editor.unsaved = true;
        }
        if (actions.regenerate || actions.respawn || player.deadTime >= kPlayerRespawnSeconds) {
            player.pos = playerSpawn(world, markers);
            player.vel = {};
            player.health = float(maxHealth(player.stats));
            player.deadTime = 0;
            player.combatTimer = 0;
            camTarget = player.pos + glm::vec3(0, 1.4f, 0);
        }
        if (actions.regenerate || actions.respawnEntities) {
            entities.clear();
            syncSpawns(entities, world, markers, worldSeed, player.pos);
        }
        actions = {};

        titleTimer += dt;
        if (titleTimer > 0.5) {
            titleTimer = 0;
            int minutes = int(game.timeOfDay * 24 * 60);
            char title[224];
            std::snprintf(title, sizeof title, "flycoos | %.2f ms (%.0f fps) | chunks %d/%d | %02d:%02d | LMB = attack | E = talk/loot | C = character | B = bags | . = debug | Tab = editor",
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
