#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numbers>
#include <print>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include "commands.h"
#include "debug_ui.h"
#include "defs.h"
#include "editor_ui.h"
#include "hud.h"
#include "inventory_ui.h"
#include "notes.h"
#include "renderer.h"
#include "save.h"
#include "server.h"
#include "session.h"

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
    bool debug = false;  // --debug: start with the debug window open; F2 writes a debug note
    bool newGame = false;  // --new-game: don't load the world's player save (quitting still writes it)
    int port = 47800;    // command server (127.0.0.1 only)
    bool serve = true;   // --no-serve turns it off; benchmarks never serve
    RenderSettings render;
};

[[noreturn]] static void usage() {
    std::println(stderr, "usage: flycoos [--world PATH] [--new-game] [--seed N] [--time 0..1] [--debug] [--view shaded|quads|chunks|ao|normals|light|wire]\n"
                         "               [--patches STRENGTH (colour patches, 0 flat palette, default 1)]\n"
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
        else if (a == "--new-game") o.newGame = true;
        else if (a == "--port" && hasValue) {
            o.port = std::atoi(argv[++i]);
            if (o.port < 1 || o.port > 65535) usage();
        } else if (a == "--no-serve") o.serve = false;
        else if (a == "--patches" && hasValue) {
            char* end = nullptr;
            o.render.patchStrength = std::strtof(argv[++i], &end);
            if (end == argv[i] || *end || !std::isfinite(o.render.patchStrength) || o.render.patchStrength < 0) usage();
        }
        else if (a == "--view" && hasValue) {
            std::string_view v = argv[++i];
            constexpr std::string_view names[ViewModeCount] = {"shaded", "quads", "chunks", "ao", "normals", "light"};
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
    RenderSettings render = opt.render;
    DebugActions actions;

    Session session{.world = Grid(kWorldSize), .seed = game.seed};
    session.timeOfDay = opt.timeOfDay;
    Grid& world = session.world;
    std::vector<Instance>& instances = session.instances;
    std::vector<Marker>& markers = session.markers;
    uint32_t& worldSeed = session.seed;
    auto buildWorld = [&] {
        uint64_t start = SDL_GetTicksNS();
        std::ranges::fill(world.cells, Air);
        worldSeed = game.seed;
        generateTerrain(world, game.seed);
        renderer.stats.genSeconds = double(SDL_GetTicksNS() - start) * 1e-9;
        renderer.uploadWorld(world);
        const RenderStats& s = renderer.stats;
        std::println("world {}x{}x{} seed {} | generated {:.3f}s + lit {:.3f}s + meshed/uploaded {:.3f}s = {:.3f}s | "
                     "{} opaque + {} water quads ({:.1f} MB) in {} chunks, light {:.1f} MiB",
                     world.size.x, world.size.y, world.size.z, game.seed, s.genSeconds, s.lightSeconds, s.meshSeconds,
                     s.genSeconds + s.lightSeconds + s.meshSeconds, s.opaqueQuads, s.waterQuads,
                     double(s.opaqueQuads + s.waterQuads) * 8 / (1 << 20), s.chunksTotal, double(s.lightBytes) / (1 << 20));
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
        if (std::string error = loadWorld(opt.world, world, worldSeed, instances, loaded, &editor.history.markerIds); !error.empty()) {
            std::println(stderr, "cannot load world {}: {}", opt.world, error);
            return 1;  // never fall back to generating: the next save would overwrite the file
        }
        markers = loaded ? std::move(*loaded) : defaultMarkers(world, worldSeed);  // older files: the NPCs they always had
        double loadSeconds = double(SDL_GetTicksNS() - start) * 1e-9;
        game.seed = worldSeed;
        renderer.uploadWorld(world);
        const RenderStats& s = renderer.stats;
        std::println("world {} ({:.1f} MB, {}x{}x{}, seed {}) loaded {:.3f}s + lit {:.3f}s + meshed/uploaded {:.3f}s | "
                     "{} opaque + {} water quads in {} chunks, light {:.1f} MiB, {} prefab instances, {} markers",
                     opt.world, double(std::filesystem::file_size(opt.world, ec)) / (1 << 20), world.size.x,
                     world.size.y, world.size.z, worldSeed, loadSeconds, s.lightSeconds, s.meshSeconds, s.opaqueQuads,
                     s.waterQuads, s.chunksTotal, double(s.lightBytes) / (1 << 20), instances.size(), markers.size());
    } else {
        buildWorld();
        markers = defaultMarkers(world, worldSeed);
    }
    renderer.uploadModels();
    const RenderStats& s = renderer.stats;
    for (const std::string& warning : checkQuestMarkers(defs(), markers)) std::println(stderr, "warning: {}", warning);

    auto save = [&] {
        uint64_t start = SDL_GetTicksNS();
        std::string error = saveWorld(editor.path.c_str(), world, worldSeed, instances, markers, editor.history.markerIds);
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
    startSession(session);
    Player& player = session.player;
    // The player save (ADR-025) next to the world file; the editor's saves never write it, F5, F9 and quitting do.
    const std::string savePath = savePathFor(editor.path).string();
    ui.savePath = savePath;
    std::string notice;  // a line at the top of the screen for a moment (F5, F9)
    uint64_t noticeAt = 0;
    // When quitting and F5 don't save: a save that exists but didn't load is kept until the player
    // overwrites or deletes it in the Save tab; after Delete, until the next save (so the next start
    // is a new game); and quitting doesn't save a session that opened the editor (F5 still does).
    bool saveLocked = false, deleted = false, usedEditor = false;
    auto autosaveOff = [&]() -> const char* {
        if (benchmark) return "benchmark: saves are off";
        if (saveLocked) return "the save didn't load: the Save tab can save over it or delete it";
        if (deleted) return "the save was deleted: the next start is a new game (F5 saves again)";
        if (usedEditor) return "the editor was opened this session: quitting doesn't save (F5 does)";
        return nullptr;
    };
    auto saveGame = [&] {
        uint64_t start = SDL_GetTicksNS();
        std::string error = savePlayer(savePath.c_str(), session, std::filesystem::path(editor.path).filename().string(),
                                       worldFileChecksum(editor.path.c_str()));
        ui.saveStatus = error.empty() ? std::format("Saved in {:.1f} ms", double(SDL_GetTicksNS() - start) * 1e-6) : "Save failed: " + error;
        notice = error.empty() ? "Game saved" : "Save failed";
        noticeAt = SDL_GetTicksNS();
        std::println("{}: {}", savePath, ui.saveStatus);
        if (error.empty()) saveLocked = deleted = false;
    };
    auto loadGame = [&] {
        SaveReport r;
        if (std::string error = loadPlayer(savePath.c_str(), session, worldFileChecksum(editor.path.c_str()), r); !error.empty()) {
            ui.saveStatus = "Load failed: " + error;
            saveLocked = true;  // don't overwrite what may still be rescued
            notice = "Load failed: saving is off until you choose in the debug window's Save tab";
            noticeAt = SDL_GetTicksNS();
            std::println(stderr, "{}: {}", savePath, ui.saveStatus);
            return;
        }
        ui.saveStatus = std::format("Loaded {}{}: {} NPCs restored, {} dropped", r.world, r.fromBackup ? " (from the backup)" : "",
                                    r.npcs, r.droppedNpcs);
        ui.saveNotes = r.notes;
        notice = r.notes.empty() ? "Game loaded" : "Game loaded, with warnings (debug window, Save tab)";
        noticeAt = SDL_GetTicksNS();
        std::println("{}: {}", savePath, ui.saveStatus);
        for (const std::string& note : ui.saveNotes) std::println("  {}", note);
    };
    if (!benchmark && !opt.newGame && (std::filesystem::exists(savePath, ec) || std::filesystem::exists(savePath + ".bak", ec))) loadGame();
    auto quit = [&] {
        if (const char* off = autosaveOff()) std::println("{}: not saved on quit ({})", savePath, off);
        else saveGame();
        return 0;
    };
    std::vector<Entity>& entities = session.entities;
    // After an edit, undo or redo: remesh what changed, bring spawns in line with the markers and
    // keep the editor's bookkeeping.
    auto edited = [&](Box dirty, uint64_t start, const char* error) {
        editor.opMs = double(SDL_GetTicksNS() - start) * 1e-6;
        editor.error = error;
        if (dirty.empty()) return;
        editor.lastDirty = dirty;
        ++editor.edits;
        renderer.updateWorld(world, dirty);
        syncSpawns(entities, world, markers, worldSeed, player.pos, session.timeOfDay);
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

    bool punchQueued = false;  // clicked before the swing was ready
    bool pressInGame = false;  // the left button went down while no window was open
    float camYaw = std::numbers::pi_v<float>, camPitch = 0.45f;  // behind the player
    if (opt.fixedLook) {  // degrees on the CLI; yaw 0 looks toward -Z, 90 toward -X
        camYaw = opt.lookYaw;
        camPitch = std::clamp(opt.lookPitch, -0.5f, 1.45f);
    }
    glm::vec3 camTarget = player.pos + glm::vec3(0, 1.4f, 0);
    float camDist = game.cameraDistance;  // actual distance after terrain collision
    bool characterOpen = false, bagsOpen = false, journalOpen = false, progressOpen = false;
    // The debug note box (notes.h): while it's open the simulation doesn't step. What it saves is
    // taken the frame it opens: the state, and a screenshot without the UI.
    bool noteOpen = false, noteFocus = false;
    char noteText[4096] = "";
    nlohmann::json noteState;
    std::vector<uint8_t> noteShot;
    int noteW = 0, noteH = 0;
    // Any open window frees the cursor and stops mouse look and attacks; the keyboard still plays.
    auto menus = [&] { return ui.open || characterOpen || bagsOpen || journalOpen || progressOpen || noteOpen || editor.cursor; };
    auto updateMouseMode = [&] { SDL_SetWindowRelativeMouseMode(window, !benchmark && !menus()); };
    updateMouseMode();
    PlayerTuning walking;  // movement settings from before entering the editor
    auto setEditing = [&](bool on) {  // the editor flies with noclip; NPCs and combat pause
        editor.active = on;
        editor.cursor = false;
        punchQueued = false;  // a click queued mid-swing must not become an editor action
        editor.corner.reset();
        if (on) {
            usedEditor = true;
            walking = player.tuning;
            player.tuning.fly = player.tuning.noclip = true;
        } else {
            player.tuning.fly = walking.fly;
            player.tuning.noclip = walking.noclip;
            player.teleport(clearOfTerrain(world, player.pos));  // not buried by an edit
        }
        updateMouseMode();
    };
    // Editor flight to `pos`, facing `lookAt` past the character.
    auto flyTo = [&](glm::vec3 pos, std::optional<glm::vec3> lookAt) {
        if (!editor.active) setEditing(true);
        player.teleport(pos);
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
    double titleTimer = 0;
    float tickTime = 0;          // frame time not yet simulated, < kTick between frames
    bool interactQueued = false;  // E pressed, not yet seen by a step
    for (int frame = 0;; ++frame) {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {  // don't spin or count frames while hidden
            for (SDL_Event e; SDL_WaitEventTimeout(&e, 100);)
                if (e.type == SDL_EVENT_QUIT) return quit();
            last = SDL_GetTicksNS();
            --frame;
            continue;
        }
        uint64_t now = SDL_GetTicksNS();
        float realDt = float(double(now - last) * 1e-9);
        last = now;
        if (frame > 0) frameMs.push_back(realDt * 1000.0);
        // Game time this frame. A benchmark frame is exactly one tick, so a run (path, camera,
        // screenshot) is the same on every machine and at any frame rate.
        float dt = benchmark ? kTick : realDt;

        SimInput sim;
        for (SDL_Event e; SDL_PollEvent(&e);) {
            ui.processEvent(e);
            if (e.type == SDL_EVENT_QUIT) return quit();
            // Editor shortcuts (Ctrl or Cmd); undo and redo repeat while held.
            // While the note box is open only Esc (cancel) reaches the game: typing isn't hotkeys.
            if (e.type == SDL_EVENT_KEY_DOWN && (e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && !ui.wantsKeyboard() && !noteOpen) {
                if (e.key.key == SDLK_Z) ((e.key.mod & SDL_KMOD_SHIFT) ? editorActions.redo : editorActions.undo) = true;
                else if (e.key.key == SDLK_Y) editorActions.redo = true;
                else if (e.key.key == SDLK_S && !e.key.repeat) editorActions.save = true;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && !ui.wantsKeyboard() && (!noteOpen || e.key.key == SDLK_ESCAPE)) {
                if (e.key.key == SDLK_PERIOD) {
                    ui.open = !ui.open;
                    updateMouseMode();
                } else if (e.key.key == SDLK_C) {
                    characterOpen = !characterOpen;
                    updateMouseMode();
                } else if (e.key.key == SDLK_B) {
                    bagsOpen = !bagsOpen;
                    updateMouseMode();
                } else if (e.key.key == SDLK_J) {
                    journalOpen = !journalOpen;
                    updateMouseMode();
                } else if (e.key.key == SDLK_K) {
                    progressOpen = !progressOpen;
                    updateMouseMode();
                } else if (e.key.key == SDLK_E) {  // loot a corpse, else talk
                    interactQueued = true;
                } else if (e.key.key == SDLK_TAB) {
                    setEditing(!editor.active);
                } else if (e.key.key == SDLK_F5 && !benchmark) {
                    if (!saveLocked) saveGame();
                    else {
                        notice = "Not saved: the save didn't load (debug window, Save tab)";
                        noticeAt = SDL_GetTicksNS();
                    }
                } else if (e.key.key == SDLK_F9 && !benchmark) {
                    actions.loadGame = true;
                } else if (e.key.key == SDLK_F2 && opt.debug && !benchmark && !noteOpen) {
                    noteOpen = noteFocus = true;
                    updateMouseMode();
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
                    if (!menus() && (!editor.unsaved || editor.quitArmed)) return quit();
                    if (!menus()) editor.quitArmed = true;
                    else if (noteOpen) noteOpen = false, noteText[0] = 0;
                    else if (ui.open) ui.open = false;
                    else characterOpen = bagsOpen = journalOpen = progressOpen = editor.cursor = false;
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
        Input& in = sim.move;
        in.move = forward * float(key(SDL_SCANCODE_W) - key(SDL_SCANCODE_S)) +
                  right * float(key(SDL_SCANCODE_D) - key(SDL_SCANCODE_A));
        if (glm::length(in.move) > 1) in.move = glm::normalize(in.move);
        in.jump = key(SDL_SCANCODE_SPACE);
        in.run = key(SDL_SCANCODE_LSHIFT);
        in.descend = key(SDL_SCANCODE_LCTRL);
        if ((SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && (key(SDL_SCANCODE_Z) || key(SDL_SCANCODE_Y) || key(SDL_SCANCODE_S)))
            in = {};  // a shortcut (Ctrl+S) isn't movement
        // Left mouse attacks toward where the camera looks; holding it keeps swinging.
        // A press that began in a window (e.g. a drag) doesn't attack once the window closes.
        if (menus()) punchQueued = pressInGame = false;
        if (noteOpen) interactQueued = false;
        bool punchHeld = pressInGame && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK);
        sim.aim = forward;
        sim.editing = editor.active;
        sim.timeRate = benchmark ? 0 : key(SDL_SCANCODE_T) ? 60.0f : 1.0f;
        // Fixed 60 Hz simulation (ADR-017): as many ticks as the frame time covers, at most 0.25 s
        // worth after a stall. A click or E press is held until one tick takes it.
        if (!noteOpen) tickTime += std::min(dt, 0.25f);
        bool respawned = false;
        ui.ticks = 0;
        for (; tickTime >= kTick; tickTime -= kTick, ++ui.ticks) {
            if (benchmark) {
                double t = session.elapsed + kTick;  // after this tick
                in.move = {std::sin(t * 0.4), std::cos(t * 0.4)};  // walk a circle
                player.health = float(maxHealth(sheet(player)));  // a passing fight mustn't change the benchmark's path
            }
            sim.attack = punchHeld || punchQueued;
            sim.interact = interactQueued;
            StepResult stepped = step(session, sim, kTick);
            if (stepped.attacked) punchQueued = false;
            interactQueued = false;
            respawned |= stepped.respawned;
        }
        if (!session.toasts.empty()) {  // quest news: started, updated, complete
            notice.clear();
            for (const auto& [quest, text] : session.toasts) notice += (notice.empty() ? "" : "   |   ") + text;
            noticeAt = SDL_GetTicksNS();
            session.toasts.clear();
        }
        float alpha = ui.tickAlpha = tickTime / kTick;  // how far into the next tick this frame is drawn
        glm::vec3 drawnAt = drawPos(player, alpha);
        if (respawned) camTarget = drawnAt + glm::vec3(0, 1.4f, 0);

        if (benchmark && !opt.fixedLook) camYaw -= dt * 0.5f;  // counter-orbit so the benchmark sees the player from all sides

        // Third-person orbit camera. A camera-sized box is marched out from the player; the camera
        // snaps in when terrain is in the way and eases back out so it doesn't pop.
        camTarget += (drawnAt + glm::vec3(0, 1.4f, 0) - camTarget) * (1 - std::exp(-15 * dt));
        glm::vec3 offset{std::sin(camYaw) * std::cos(camPitch), std::sin(camPitch), std::cos(camYaw) * std::cos(camPitch)};
        bool collide = game.cameraCollision && !player.tuning.noclip;
        float clear = collide ? cameraClearance(world, camTarget, offset, game.cameraDistance, kCameraRadius)
                              : game.cameraDistance;
        camDist = clear < camDist ? clear : camDist + (clear - camDist) * (1 - std::exp(-4 * dt));

        FrameParams fp;
        fp.eye = camTarget + offset * camDist;
        fp.view = glm::lookAtRH(fp.eye, camTarget, glm::vec3(0, 1, 0));
        fp.fovY = glm::radians(game.fovDegrees);
        fp.timeOfDay = session.timeOfDay;

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
                    EditResult r = placePrefab(world, *prefab, in, aim->cell.y, instances, editor.history, false, &markers);
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
            fp.models.push_back({partModels(model, body.inv), poseModel(body, model, &held, alpha)});
            if (held == glm::mat4(0)) return;
            ModelInstance weapon;
            weapon.parts.fill(heldModel(body.weapon()));
            weapon.pose.fill(held);
            fp.models.push_back(weapon);
        };
        pushModel(player, ModelPlayer);
        for (const Entity& npc : entities) pushModel(npc.body, npc.type().model);

        GameView view{editor.path, worldSeed, editor.unsaved, editor.active, editor.follow, player.pos, fp.eye,
                      glm::degrees(camYaw), glm::degrees(camPitch), game.fovDegrees, session.timeOfDay};
        if (noteOpen && noteFocus) {  // just opened: no tick ran this frame
            noteState = noteContext(session, view);
            noteState["frame_ms"] = std::round(realDt * 1e5) / 100;
            noteState["chunks_drawn"] = s.chunksDrawn;
            SDL_GetWindowSize(window, &noteW, &noteH);
            if (!renderer.snapshot(fp, render, uint32_t(noteW), uint32_t(noteH), noteShot)) noteShot.clear();
        }

        // One remote command per frame, run here so it shares the editor's undo history and remesh
        // path, and its log line and highlight show this frame.
        if (std::optional<std::string> request = server.take()) {
            uint64_t start = SDL_GetTicksNS();
            Command c = runCommand(*request, world, instances, markers, editor.history, editor.prefabs, view);
            nlohmann::json& response = c.response;
            if (!c.dirty.empty()) {
                edited(c.dirty, start, nullptr);
                editor.flash = c.dirty;
                editor.flashAt = double(SDL_GetTicksNS()) * 1e-9;
            }
            if (c.save) {  // to the world's file, or a copy elsewhere (which leaves the unsaved state alone)
                std::string target = c.save->empty() ? editor.path : *c.save;
                std::string error = c.save->empty() ? save() : saveWorld(target.c_str(), world, worldSeed, instances, markers, editor.history.markerIds);
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
        auto inRegion = std::ranges::find(markers, session.regionId, &Marker::id);  // still there after the command?
        const Marker* region = session.regionId && inRegion != markers.end() ? &*inRegion : nullptr;
        ui.region = region ? region->name.c_str() : nullptr;
        bool wasOpen = menus();
        ui.begin(realDt, wasOpen);
        if (game.hud) {
            const Entity* target = lootTarget(entities, player);
            drawHud(fp.view, fp.fovY, fp.eye, world, player, entities, target ? target : talkTarget(entities, player), alpha);
            drawPlaces(fp.view, fp.fovY, player, markers, region, float(session.elapsed - session.regionSince));
            drawNotice(notice.c_str(), float(double(SDL_GetTicksNS() - noticeAt) * 1e-9));
        }
        drawEditorOverlay(fp.view, fp.fovY, fp.eye, world, instances, markers, editor, aim);
        characterWindow(characterOpen, player, session.armorK);
        bagsWindow(bagsOpen, player);
        journalWindow(journalOpen, session);
        progressWindow(progressOpen, player);
        editorWindow(editor, instances, markers, aim, s, editorActions);
        if (noteOpen) {
            NoteAction note = noteWindow(noteText, sizeof noteText, noteFocus);
            noteFocus = false;
            if (note == NoteAction::Save) {
                std::string error;
                int n = saveNote("debug_notes", noteText, std::move(noteState), noteShot, noteW, noteH, error);
                notice = n ? std::format("Debug note {} saved (debug_notes/notes.jsonl)", n) : "Debug note not saved: " + error;
                noticeAt = SDL_GetTicksNS();
                std::println("{}", notice);
                if (!n) note = NoteAction::None;  // stays open with the text: try again, or copy it out
            }
            if (note != NoteAction::None) noteOpen = false, noteText[0] = 0;
        }
        ImDrawData* uiDraw = ui.end(game, render, session, s, actions);
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
            EditResult r = removePrefab(world, editorActions.remove, instances, editor.history, &markers);
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
            session.rng.seed(worldSeed);
            editor.marked = 0;
            ++editor.edits;
            editor.corner.reset();
            editor.unsaved = true;
        }
        if (actions.regenerate || actions.respawn) {
            respawnPlayer(session);
            camTarget = player.pos + glm::vec3(0, 1.4f, 0);
        }
        if (actions.regenerate || actions.respawnEntities) {
            entities.clear();
            syncSpawns(entities, world, markers, worldSeed, player.pos, session.timeOfDay);
        }
        if (benchmark) actions.saveGame = actions.loadGame = actions.deleteSave = false;
        if (actions.saveGame) saveGame();  // the Save tab's button: also over a save that didn't load
        if (actions.loadGame) {
            loadGame();
            camTarget = player.pos + glm::vec3(0, 1.4f, 0);
        }
        if (actions.deleteSave) {
            std::filesystem::remove(savePath, ec);
            std::filesystem::remove(savePath + ".bak", ec);
            ui.saveStatus = "Deleted";
            ui.saveNotes.clear();
            saveLocked = false;
            deleted = true;
        }
        const char* off = autosaveOff();
        ui.autosave = off ? off : "on: quitting saves";
        ui.saveLocked = saveLocked;
        actions = {};

        titleTimer += realDt;
        if (titleTimer > 0.5) {
            titleTimer = 0;
            int minutes = int(session.timeOfDay * 24 * 60);
            char title[256];
            std::snprintf(title, sizeof title, "flycoos | %.2f ms (%.0f fps) | chunks %d/%d | %02d:%02d | LMB = attack | E = talk/loot | C = character | B = bags | J = journal | . = debug | Tab = editor",
                          realDt * 1000.0, 1.0 / std::max(realDt, 1e-6f), s.chunksDrawn, s.chunksTotal, minutes / 60, minutes % 60);
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
    Defs loaded;
    if (std::string error; !loadDefs(dataDir(), loaded, error)) {
        std::println(stderr, "cannot load definitions: {}", error);
        return 1;
    }
    for (const std::string& warning : loaded.warnings) std::println(stderr, "warning: {}", warning);
    setDefs(std::move(loaded));
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
