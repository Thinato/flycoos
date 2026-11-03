#include "editor_ui.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "hud.h"

namespace {

constexpr const char* kToolNames[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint", "Fill box"};
static_assert(std::size(kToolNames) == size_t(Tool::Count));
constexpr Block kMaterials[] = {Grass, Dirt, Stone, Sand, Snow, Wood, Leaves, Rock, Water, Air};  // solid ones first
constexpr const char* kMaterialNames[] = {"Grass", "Dirt", "Stone", "Sand", "Snow", "Wood", "Leaves", "Rock", "Water", "Air (clear)"};
constexpr int kSolidMaterials = 8;
constexpr float kFlashSeconds = 3;

}  // namespace

void editorWindow(Editor& ed, const std::optional<RayHit>& aim, const RenderStats& s, EditorActions& actions) {
    EditOp& b = ed.brush;
    if (b.tool == Tool::Paint && !isOpaque(b.block)) b.block = Grass;  // Paint needs a solid block
    if (!ed.active) return;
    // While the crosshair owns the mouse the panel is display-only, so clicks can't land in it.
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 12, 12}, ImGuiCond_FirstUseEver, {1, 0});
    ImGui::SetNextWindowBgAlpha(ed.cursor ? 0.95f : 0.6f);
    if (!ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize | (ed.cursor ? 0 : ImGuiWindowFlags_NoInputs))) {
        ImGui::End();
        return;
    }
    int tool = int(b.tool);
    for (int t = 0; t < int(Tool::Count); ++t) {
        if (t % 3) ImGui::SameLine();
        ImGui::RadioButton(kToolNames[t], &tool, t);
    }
    b.tool = Tool(tool);
    if (b.tool != Tool::Fill) {
        ImGui::SliderInt("Radius", &b.radius, 0, kMaxBrushRadius);
        ImGui::SetItemTooltip("[ and ] change it too.");
        ImGui::SliderInt("Strength", &b.strength, 1, 16);
        ImGui::SetItemTooltip("Blocks per application at the center; holding the button applies 8 times a second.");
    }
    if (b.tool == Tool::Flatten) {
        ImGui::SliderInt("Height", &b.height, 0, kWorldSize.y - 1);
        ImGui::SetItemTooltip("Ground height to flatten to. Each stroke takes it from where it starts.");
    }
    if (b.tool == Tool::Paint || b.tool == Tool::Fill) {
        int count = b.tool == Tool::Fill ? int(std::size(kMaterials)) : kSolidMaterials;
        int current = int(std::ranges::find(kMaterials, b.block) - std::begin(kMaterials));
        if (ImGui::BeginCombo("Material", current < count ? kMaterialNames[current] : "?")) {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(kMaterialNames[i], i == current)) b.block = kMaterials[i];
            ImGui::EndCombo();
        }
    }
    if (b.tool == Tool::Fill) {
        if (ed.corner) ImGui::Text("First corner %d %d %d: click the second", ed.corner->x, ed.corner->y, ed.corner->z);
        else ImGui::TextDisabled("Click two corners. Air clears the aimed voxels;\nblocks go in front of the aimed face.");
    }

    ImGui::Separator();
    if (ImGui::Button("Undo")) actions.undo = true;
    ImGui::SameLine();
    if (ImGui::Button("Redo")) actions.redo = true;
    ImGui::SameLine();
    ImGui::Text("%zu undo / %zu redo (max %zu)", ed.history.undo.size(), ed.history.redo.size(), kMaxUndo);
    if (ImGui::Button("Save")) actions.save = true;
    ImGui::SameLine();
    ImGui::TextUnformatted(ed.path.c_str());
    if (ed.unsaved) ImGui::TextColored({1, 0.6f, 0.15f, 1}, "Unsaved changes");
    else if (ed.savedAt >= 0) ImGui::Text("Saved %.0f s ago", double(SDL_GetTicksNS()) * 1e-9 - ed.savedAt);
    else ImGui::TextDisabled("No changes");
    if (!ed.status.empty()) ImGui::TextWrapped("%s", ed.status.c_str());

    ImGui::SeparatorText("Debug");
    if (aim) ImGui::Text("Aim %d %d %d, face %d %d %d", aim->cell.x, aim->cell.y, aim->cell.z, aim->normal.x, aim->normal.y, aim->normal.z);
    else ImGui::TextDisabled("Aim: no ground in range");
    if (ed.error) ImGui::TextColored({1, 0.35f, 0.3f, 1}, "Rejected: %s", ed.error);
    else if (!ed.lastDirty.empty())
        ImGui::Text("Last op %.2f ms, changed %d x %d x %d", ed.opMs, ed.lastDirty.size().x, ed.lastDirty.size().y, ed.lastDirty.size().z);
    ImGui::Text("Remesh %.2f ms (%d chunks), %d repacks", s.remeshMs, s.chunksRemeshed, s.repacks);

    ImGui::SeparatorText("MCP");
    ImGui::Checkbox("Follow Claude", &ed.follow);
    ImGui::SetItemTooltip("Let Claude's goto command fly your camera to what it means.");
    ImGui::SameLine();
    ImGui::TextDisabled("server %s", ed.server.c_str());
    if (ed.log.empty()) ImGui::TextDisabled("No commands yet");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 32 * ImGui::GetFontSize());  // long lines wrap instead of widening the panel
    for (auto it = ed.log.rbegin(); it != ed.log.rend(); ++it)
        ImGui::TextColored(it->ok ? ImVec4(0.8f, 0.85f, 0.9f, 1) : ImVec4(1, 0.35f, 0.3f, 1), "%6.1f ms  %s", it->ms, it->text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    ImGui::TextDisabled("%s", ed.cursor ? "Q: back to the crosshair" : "Q: cursor for this panel");
    ImGui::TextDisabled("LMB apply (hold repeats)   1-6 tool   [ ] radius\nCtrl+Z undo   Ctrl+Shift+Z / Ctrl+Y redo   Ctrl+S save");
    ImGui::End();
}

void drawEditorOverlay(const glm::mat4& view, float fovY, const Grid& world, const Editor& ed, const std::optional<RayHit>& aim) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (screen.x <= 0 || screen.y <= 0) return;
    if (ed.quitArmed) {
        const char* msg = "Unsaved edits: Esc again quits, Ctrl+S saves";
        ImVec2 size = ImGui::CalcTextSize(msg), at(screen.x / 2 - size.x / 2, screen.y * 0.3f);
        dl->AddText(ImVec2(at.x + 1, at.y + 1), uiColor(0, 0, 0, 0.8f), msg);
        dl->AddText(at, uiColor(1, 0.6f, 0.15f), msg);
    }
    glm::mat4 viewProj = glm::perspectiveRH_ZO(fovY, screen.x / screen.y, 0.1f, 1000.0f) * view;
    auto project = [&](glm::vec3 p, ImVec2& out) {
        glm::vec4 clip = viewProj * glm::vec4(p, 1);
        out = ImVec2((clip.x / clip.w * 0.5f + 0.5f) * screen.x, (0.5f - clip.y / clip.w * 0.5f) * screen.y);
        return clip.w > 0.1f;  // in front of the camera
    };
    auto drawBox = [&](glm::ivec3 a, glm::ivec3 b, ImU32 color) {  // outlines the voxels between two corners
        glm::vec3 lo(glm::min(a, b)), hi(glm::max(a, b) + 1);
        auto corner = [&](int i) { return glm::vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z); };
        for (int i = 0; i < 8; ++i)
            for (int bit : {1, 2, 4}) {
                ImVec2 p, q;
                if (!(i & bit) && project(corner(i), p) && project(corner(i | bit), q)) dl->AddLine(p, q, color, 2);
            }
    };
    // A remote edit flashes its box (fading out), so the user sees where Claude changed the world.
    if (float age = float(double(SDL_GetTicksNS()) * 1e-9 - ed.flashAt); ed.flashAt >= 0 && age < kFlashSeconds && !ed.flash.empty())
        drawBox(ed.flash.lo, ed.flash.hi, uiColor(0.3f, 0.9f, 1, (1 - age / kFlashSeconds) * (0.6f + 0.4f * std::cos(age * 8))));
    if (!ed.active) return;

    ImVec2 c(screen.x / 2, screen.y / 2);
    for (auto [color, w] : {std::pair{uiColor(0, 0, 0, 0.7f), 4.0f}, std::pair{uiColor(1, 1, 1, 0.9f), 2.0f}}) {  // outlined
        dl->AddLine(ImVec2(c.x - 9, c.y), ImVec2(c.x + 10, c.y), color, w);
        dl->AddLine(ImVec2(c.x, c.y - 9), ImVec2(c.x, c.y + 10), color, w);
    }
    if (!aim) return;

    ImU32 color = uiColor(1, 0.85f, 0.3f, 0.95f);
    if (ed.brush.tool == Tool::Fill) {  // the box the next click fills
        drawBox(ed.corner.value_or(fillCell(ed, *aim)), fillCell(ed, *aim), color);
        return;
    }
    // The brush ring follows the ground tops around the aimed column.
    constexpr int kPoints = 64;
    ImVec2 ring[kPoints];
    glm::vec2 center(float(aim->cell.x) + 0.5f, float(aim->cell.z) + 0.5f);
    float r = float(ed.brush.radius) + 0.5f;
    for (int i = 0; i < kPoints; ++i) {
        float angle = float(i) / kPoints * 2 * std::numbers::pi_v<float>;
        glm::vec2 p = center + r * glm::vec2(std::cos(angle), std::sin(angle));
        int top = groundTop(world, int(std::floor(p.x)), int(std::floor(p.y)));
        if (!project({p.x, float(top >= 0 ? top : aim->cell.y) + 1.05f, p.y}, ring[i])) return;
    }
    dl->AddPolyline(ring, kPoints, uiColor(0, 0, 0, 0.6f), ImDrawFlags_Closed, 4.0f);
    dl->AddPolyline(ring, kPoints, color, ImDrawFlags_Closed, 2.0f);
}
