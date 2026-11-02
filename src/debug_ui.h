#pragma once
#include <array>
#include <cstdint>

#include <SDL3/SDL.h>

#include "player.h"
#include "renderer.h"

// Main-loop knobs the debug UI can change.
struct GameSettings {
    float fovDegrees = 70, cameraDistance = 9, mouseSensitivity = 0.004f;
    bool cameraCollision = true;
    float timeOfDay = 0.3f, timeScale = 1;  // timeScale 0 = time paused
    uint32_t seed = 1337;
};

// One-shot requests the main loop carries out (they need the world and renderer).
struct DebugActions {
    bool regenerate = false, respawn = false;
};

// Dear ImGui debug window, toggled with '.'. Tabs: Render, Player, Camera, World, Stats.
class DebugUI {
public:
    DebugUI(SDL_Window* window, const Renderer& renderer);
    ~DebugUI();
    DebugUI(const DebugUI&) = delete;
    DebugUI& operator=(const DebugUI&) = delete;

    void processEvent(const SDL_Event& e);
    bool wantsKeyboard() const;  // a widget has keyboard focus: keys aren't game input
    bool wantsMouse() const;     // the cursor is over the window

    // Records frame time and, when open, builds this frame's window. Returns null when closed.
    ImDrawData* frame(float dt, GameSettings& game, RenderSettings& render, Player& player,
                      const RenderStats& stats, DebugActions& actions);

    bool open = false;

private:
    std::array<float, 240> frameMs_{};
    size_t frameIndex_ = 0;
};
