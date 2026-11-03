#pragma once
#include <array>
#include <deque>
#include <unordered_map>
#include <optional>
#include <string>

#include "prefab.h"
#include "render_stats.h"

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

    // Prefab tool (7): LMB places `place` (its prefab, rotation, seed and params) on the aimed voxel,
    // or selects the instance that voxel belongs to.
    bool prefabTool = false;
    Instance place{.prefab = "tree"};
    PrefabLibrary prefabs;
    uint32_t selected = 0;  // instance id, 0 = none
    // The instance owning the aimed voxel (0 = none), recomputed when the aim or the world changes.
    uint32_t aimed = 0;
    glm::ivec3 aimedCell{-1};
    uint64_t edits = 0, aimedEdits = 0;  // edits counts every change to the world (see main.cpp)

    // Marker tool (8): LMB places `mark` (kind, name, radius, entity, count, lines) on the aimed voxel, or
    // selects the marker standing there and loads its settings.
    bool markerTool = false;
    Marker mark{.kind = MarkerKind::Poi, .radius = 16};
    char markName[kMaxMarkerName + 1] = "";
    char markLines[kMaxMarkerLines * (kMaxMarkerLine + 1)] = "";  // spawn: its lines, one per text line
    uint32_t marked = 0;  // selected marker id, 0 = none
    Marker markedWas;     // the selected marker as last loaded into the panel (main reloads it when it changes)
    // Marker circle heights per marker id, valid while ringEdits == edits (the overlay draws them each frame).
    static constexpr int kRingPoints = 64;
    using Ring = std::array<float, kRingPoints + 1>;
    mutable std::unordered_map<uint32_t, Ring> rings;
    mutable uint64_t ringEdits = 0;

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
    uint32_t remove = 0, goTo = 0;  // instance ids
    uint32_t unmark = 0, markGoTo = 0;  // marker ids
    bool updateMark = false;            // the selected marker takes the panel's settings
};

// The Editor panel (only takes input while editor.cursor is set). Call between DebugUI::begin and end.
void editorWindow(Editor& editor, const std::vector<Instance>& instances, const std::vector<Marker>& markers,
                  const std::optional<RayHit>& aim, const RenderStats& stats, EditorActions& actions);
// Crosshair, the brush ring on the terrain (or Fill's box, or the prefab's box) at `aim`, the
// selected instance's box, the markers near the camera (pins with labels, region circles) and the
// unsaved-quit warning.
void drawEditorOverlay(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const std::vector<Instance>& instances,
                       const std::vector<Marker>& markers, const Editor& editor, const std::optional<RayHit>& aim);
