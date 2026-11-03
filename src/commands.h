#pragma once
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "prefab.h"

// Remote world-editing commands (see docs/ARCHITECTURE.md, "Command server"). One request line
// {"id", "cmd", "args"} gives one response {"id", "ok", "result" | "error"}. Edits and queries run
// here on the grid; what needs the game (remesh, save, render, camera) comes back as data, so this
// runs without a window in tests.

// What `state` reports and `screenshot` defaults to.
struct GameView {
    std::string path;  // the file Ctrl+S saves to
    uint32_t seed = 0;
    bool unsaved = false, editing = false, follow = true;
    glm::vec3 player{}, eye{};
    float yawDegrees = 0, pitchDegrees = 0, fovDegrees = 70, timeOfDay = 0;
};

struct Shot {  // render this view offscreen
    glm::vec3 eye{}, target{};
    float fovDegrees = 70, timeOfDay = 0;
    int width = 1280, height = 720;
};

struct Goto {  // fly the user's editor camera here
    glm::vec3 pos{};
    std::optional<glm::vec3> lookAt;
};

struct Command {
    nlohmann::json response;  // the game adds save and screenshot results to response["result"]
    std::string summary;      // one line for the in-game log
    Box dirty;                // what the command changed (bounding box): remesh and highlight it
    std::optional<std::string> save;  // save the world: "" = to its file, else a copy to this path
    std::optional<Shot> shot;
    std::optional<Goto> go;
};

// `prefabs` is rescanned by the prefabs command.
Command runCommand(std::string_view request, Grid& world, std::vector<Instance>& instances, std::vector<Marker>& markers,
                   EditHistory& history, PrefabLibrary& prefabs, const GameView& view);

// Writes an RGB8 image to a new PNG in the system temp folder and returns its absolute path, or ""
// on failure.
std::string writePng(const char* name, int w, int h, const uint8_t* rgb);
