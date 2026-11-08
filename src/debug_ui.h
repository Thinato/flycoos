#pragma once
#include <array>
#include <cstdint>
#include <string>

#include <SDL3/SDL.h>

#include <vector>

#include "renderer.h"
#include "lockon.h"
#include "session.h"

// Main-loop knobs the debug UI can change.
struct GameSettings {
    float fovDegrees = 70, cameraDistance = 9, mouseSensitivity = 0.004f;
    float stickLookSpeed = 3, stickDeadzone = 0.15f;
    LockSettings lock;            // R3 / Tab lock-on
    float lockFlickPixels = 60;   // mouse travel (quickly) that switches the lock-on target  // a controller's right stick: radians a second at full tilt
    bool cameraCollision = true;
    bool hud = true;  // nameplates, speech balloons, health bar
    uint32_t seed = 1337;
    float streamLightMs = 2;  // the endless world: time a frame spends lighting chunk columns (then they're meshed)
};

// One-shot requests the main loop carries out (they need the world and renderer).
struct DebugActions {
    bool regenerate = false, respawn = false, respawnEntities = false;
    bool saveGame = false, loadGame = false, deleteSave = false;  // the player save (F5, F9)
};

// The debug note box (F2 in a --debug session, see notes.h), centered on the screen: Enter (or Save)
// returns Save with `text` filled in, Esc (or Cancel) returns Cancel. `focus`: put the cursor in the
// text field (the frame it opens).
enum class NoteAction { None, Save, Cancel };
NoteAction noteWindow(char* text, size_t size, bool focus);

// Dear ImGui debug window, toggled with '.'. Tabs: Render, Player, Progression, Items, Entities, Quests, Data, Camera, World, Save,
// Stats.
// Also owns the ImGui frame the in-game HUD draws into.
class Stream;
class DebugUI {
public:
    DebugUI(SDL_Window* window, const Renderer& renderer);
    ~DebugUI();
    DebugUI(const DebugUI&) = delete;
    DebugUI& operator=(const DebugUI&) = delete;

    void processEvent(const SDL_Event& e);
    bool wantsKeyboard() const;  // a widget has keyboard focus: keys aren't game input
    bool wantsMouse() const;     // the cursor is over an ImGui window

    // Records frame time and starts the ImGui frame (the HUD and game windows draw between begin
    // and end). `cursor`: the mouse is free (some window is open), so ImGui may change its shape.
    void begin(float dt, bool cursor);
    // Builds the debug window when open and finishes the frame.
    ImDrawData* end(GameSettings& game, RenderSettings& render, Session& session,
                    const RenderStats& stats, DebugActions& actions);

    bool open = false;
    const char* region = nullptr;  // the name of the region the player is in (main sets it each frame)
    const Stream* stream = nullptr;  // the endless world's, else null (main sets it)
    SDL_Gamepad* gamepad = nullptr;  // the controller playing, else null (main sets it)
    EntityKind spawnKind = EntityKind::Bandit;
    const LockOn* lock = nullptr;  // the player's lock-on (main sets it)  // what F6 and the Entities tab spawn in front of the player
    int ticks = 0;                 // sim ticks this frame, and how far into the next one it's drawn (main sets them)
    float tickAlpha = 0;
    // The Save tab (main sets them): the player save's path, the last save or load's result, and the
    // last load's warnings (world changed, NPCs dropped, items moved).
    std::string savePath, saveStatus;
    std::vector<std::string> saveNotes;
    const char* autosave = "";  // whether quitting saves, else why not
    bool saveLocked = false;    // the save didn't load: F5 and quitting won't write over it

private:
    void window(GameSettings& game, RenderSettings& render, Session& session,
                const RenderStats& stats, DebugActions& actions);

    std::array<float, 240> frameMs_{};
    size_t frameIndex_ = 0;
    std::string defsStatus_;  // the Data tab's last reload result
};
