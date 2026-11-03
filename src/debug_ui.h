#pragma once
#include <array>
#include <cstdint>
#include <string>

#include <SDL3/SDL.h>

#include <vector>

#include "renderer.h"
#include "session.h"

// Main-loop knobs the debug UI can change.
struct GameSettings {
    float fovDegrees = 70, cameraDistance = 9, mouseSensitivity = 0.004f;
    bool cameraCollision = true;
    bool hud = true;  // nameplates, speech balloons, health bar
    uint32_t seed = 1337;
};

// One-shot requests the main loop carries out (they need the world and renderer).
struct DebugActions {
    bool regenerate = false, respawn = false, respawnEntities = false;
    bool saveGame = false, loadGame = false, deleteSave = false;  // the player save (F5, F9)
};

// Dear ImGui debug window, toggled with '.'. Tabs: Render, Player, Items, Entities, Data, Camera, World, Save, Stats.
// Also owns the ImGui frame the in-game HUD draws into.
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
