#pragma once
#include <deque>
#include <optional>
#include <string>

#include "edit.h"
#include "renderer.h"

// In-game world editor (Tab): brush settings, undo history and save state. main.cpp applies the
// brushes; this draws the Editor panel and the crosshair/brush preview.
struct Editor {
    bool active = false;
    bool cursor = false;  // Q: the cursor is free for the panel (brushes and mouse look pause)
    EditOp brush;         // tool and settings; center (and Fill's corners) come from the crosshair
    std::optional<glm::ivec3> corner;  // Fill: first corner, waiting for the second click
    EditHistory history;
    std::string path;     // save target
    bool unsaved = false, quitArmed = false;  // quitArmed: Esc pressed once with unsaved edits
    double savedAt = -1;  // SDL seconds of the last save; -1 = not saved this session
    std::string status;   // last save result
    const char* error = nullptr;  // last rejected op
    double opMs = 0;      // last applyEdit/undo/redo
    Box lastDirty;

    // Remote commands (Claude through the MCP bridge).
    struct LogLine {
        std::string text;
        double ms;
        bool ok;
    };
    std::deque<LogLine> log;  // newest last, at most kLogLines
    static constexpr size_t kLogLines = 20;
    std::string server = "off";  // where the command server listens
    bool follow = true;          // `goto` may move the camera
    Box flash;                   // what the last remote edit changed, outlined for a few seconds
    double flashAt = -1;         // SDL seconds
};

// The cell a Fill click picks: the aimed voxel when clearing, else the empty cell in front of its face.
inline glm::ivec3 fillCell(const Editor& editor, const RayHit& aim) {
    return editor.brush.block == Air ? aim.cell : aim.cell + aim.normal;
}

struct EditorActions {
    bool undo = false, redo = false, save = false;
};

// The Editor panel (only takes input while editor.cursor is set). Call between DebugUI::begin and end.
void editorWindow(Editor& editor, const std::optional<RayHit>& aim, const RenderStats& stats, EditorActions& actions);
// Crosshair, the brush ring on the terrain (or Fill's box) at `aim`, and the unsaved-quit warning.
void drawEditorOverlay(const glm::mat4& view, float fovY, const Grid& world, const Editor& editor,
                       const std::optional<RayHit>& aim);
