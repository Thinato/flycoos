#include "editor_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include "defs.h"
#include "hud.h"

namespace {

constexpr const char* kToolNames[] = {"Raise", "Lower", "Smooth", "Flatten", "Paint", "Fill box", "Prefab", "Marker"};
static_assert(std::size(kToolNames) == size_t(Tool::Count) + 2);
constexpr float kFlashSeconds = 3;
constexpr float kMarkerRange = 300;  // blocks from the camera
constexpr size_t kMaxRings = 32;     // marker circles drawn per frame (the nearest)
constexpr int kRingPoints = Editor::kRingPoints;
// A ring's unit circle, point kRingPoints closing it.
const auto [kRingCos, kRingSin] = [] {
    std::array<float, kRingPoints + 1> c{}, s{};
    for (int i = 0; i <= kRingPoints; ++i) {
        float angle = float(i) / kRingPoints * 2 * std::numbers::pi_v<float>;
        c[size_t(i)] = std::cos(angle), s[size_t(i)] = std::sin(angle);
    }
    return std::pair{c, s};
}();
constexpr float kMarkerColors[][3] = {{1, 1, 1}, {0.95f, 0.3f, 0.25f}, {1, 0.85f, 0.2f}, {0.3f, 0.9f, 1}};  // by kind

}  // namespace

void editorWindow(Editor& ed, const std::vector<Instance>& instances, const std::vector<Marker>& markers,
                  const std::optional<RayHit>& aim, const RenderStats& s, EditorActions& actions) {
    EditOp& b = ed.brush;
    if (b.tool == Tool::Paint && !isOpaque(b.block)) b.block = Grass;  // Paint needs a solid block
    const Prefab* prefab = ed.prefabs.find(ed.place.prefab);
    if (!prefab && !ed.prefabs.prefabs.empty()) prefab = &ed.prefabs.prefabs[0];  // e.g. its file was removed
    if (prefab && (ed.place.prefab != prefab->name || ed.place.params.size() != prefab->params.size())) {
        ed.place.prefab = prefab->name;
        ed.place.params.clear();
        for (const PrefabParam& q : prefab->params) ed.place.params.push_back(q.fallback);
    }
    if (!ed.active) return;
    // While the crosshair owns the mouse the panel is display-only, so clicks can't land in it.
    ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 12, 12}, ImGuiCond_FirstUseEver, {1, 0});
    ImGui::SetNextWindowBgAlpha(ed.cursor ? 0.95f : 0.6f);
    if (!ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize | (ed.cursor ? 0 : ImGuiWindowFlags_NoInputs))) {
        ImGui::End();
        return;
    }
    int tool = ed.markerTool ? int(Tool::Count) + 1 : ed.prefabTool ? int(Tool::Count) : int(b.tool);
    for (int t = 0; t <= int(Tool::Count) + 1; ++t) {
        if (t % 4) ImGui::SameLine();
        ImGui::RadioButton(kToolNames[t], &tool, t);
    }
    ed.prefabTool = tool == int(Tool::Count);
    ed.markerTool = tool == int(Tool::Count) + 1;
    bool brushTool = tool < int(Tool::Count);
    if (brushTool) b.tool = Tool(tool);
    if (ed.markerTool) {
        int kind = int(ed.mark.kind), entity = int(ed.mark.entity);
        if (ImGui::Combo("Kind", &kind, kMarkerKindNames, int(MarkerKind::Count))) ed.marked = 0;  // a new kind is for placing
        ImGui::SetItemTooltip("Changing it deselects: a marker's kind can't change (remove it and place another).");
        ed.mark.kind = MarkerKind(kind);
        ImGui::InputText("Name", ed.markName, sizeof ed.markName);
        ImGui::SliderFloat("Radius##marker", &ed.mark.radius, 0, kMaxMarkerRadius, "%.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Spawn: how far its NPCs wander. Poi: its label shows within it. Region: the circle.");
        if (ed.mark.kind == MarkerKind::Spawn) {
            ImGui::Combo("Entity", &entity, [](void*, int i) { return defs().creatureIds[size_t(i)].c_str(); }, nullptr,
                         int(defs().creatures.size()));
            ed.mark.entity = EntityKind(entity);
            ImGui::SliderInt("Count", &ed.mark.count, 1, kMaxSpawnCount);
            bool timed = ed.mark.activeFrom != ed.mark.activeTo;
            if (ImGui::Checkbox("Only at certain times", &timed) && timed) ed.mark.activeFrom = 0.8f, ed.mark.activeTo = 0.2f;
            if (!timed) ed.mark.activeFrom = ed.mark.activeTo = 0;  // always: the only equal pair checkMarker takes
            ImGui::SetItemTooltip("Its NPCs are out from From to To (times of day; past midnight when From > To). Outside\n"
                                  "it they leave once out of a fight, corpses once looted, and none come back until it opens.");
            for (auto [label, t] : {std::pair{"From", &ed.mark.activeFrom}, std::pair{"To", &ed.mark.activeTo}}) {
                if (!timed) break;
                int minutes = int(*t * 24 * 60);
                char format[24];
                std::snprintf(format, sizeof format, "%%.3f (%02d:%02d)", minutes / 60, minutes % 60);
                ImGui::SliderFloat(label, t, 0, 0.999f, format, ImGuiSliderFlags_AlwaysClamp);
            }
        } else {
            ed.mark.activeFrom = ed.mark.activeTo = 0;  // spawns only
        }
        // Lines for spawns (not the dummy); blank lines are dropped, checkMarker rejects the rest.
        bool lines = takesLines(ed.mark);
        if (lines) {
            ImGui::InputTextMultiline("Lines", ed.markLines, sizeof ed.markLines, {0, 5 * ImGui::GetTextLineHeightWithSpacing()});
            ImGui::SetItemTooltip("What its NPCs say, one line each (up to 8 of 160 bytes), in order: talkers on E, fighters\n"
                                  "when they attack. With lines and count 1 the NPC is called by the marker's name.");
        }
        ed.mark.lines.clear();
        for (char *line = ed.markLines, *end = line + std::strlen(line); lines && line < end;) {
            char* next = std::find(line, end, '\n');
            if (next > line) ed.mark.lines.emplace_back(line, next);
            line = next + (next < end);
        }
        ImGui::BeginDisabled(std::ranges::find(markers, ed.marked, &Marker::id) == markers.end());
        if (ImGui::Button("Update selected")) actions.updateMark = true;
        ImGui::SetItemTooltip("The selected marker takes these settings (but keeps its kind) and stays where it is.");
        ImGui::EndDisabled();
        ImGui::TextDisabled("LMB places on the aimed voxel, or selects the marker\nthere (its settings load above); Delete removes it.");
    }
    if (ed.prefabTool) {
        if (ImGui::BeginCombo("Prefab", prefab ? prefab->name.c_str() : "none loaded")) {
            for (const Prefab& p : ed.prefabs.prefabs)
                if (ImGui::Selectable(p.name.c_str(), &p == prefab)) ed.place.prefab = p.name;  // params reset next frame
            ImGui::EndCombo();
        }
        if (prefab) ImGui::TextDisabled("%s", prefab->description.c_str());
        if (prefab && ed.place.params.size() == prefab->params.size()) {
            for (size_t i = 0; i < prefab->params.size(); ++i) {
                const PrefabParam& q = prefab->params[i];
                float& v = ed.place.params[i];
                if (q.whole) {
                    int n = int(v);
                    ImGui::SliderInt(q.name.c_str(), &n, int(q.lo), int(q.hi));
                    v = float(n);
                } else {
                    ImGui::SliderFloat(q.name.c_str(), &v, q.lo, q.hi, "%.1f");
                }
            }
            if (!prefab->generated())
                ImGui::Text("%d x %d x %d, rotation %d (R)", prefab->size.x, prefab->size.z, prefab->size.y, ed.place.rotation * 90);
        }
        ImGui::Text("Seed %u", ed.place.seed);
        ImGui::SameLine();
        if (ImGui::Button("Reroll")) ed.place.seed = uint32_t(SDL_rand_bits());
        ImGui::SameLine();
        if (ImGui::Button("Rescan prefabs/")) ed.prefabs = loadPrefabs(ed.prefabs.dir);
        ImGui::TextDisabled("LMB places on the aimed voxel, or selects the\nprefab you aim at; Delete removes the selection.");
    }
    if (brushTool && b.tool != Tool::Fill) {
        ImGui::SliderInt("Radius", &b.radius, 0, kMaxBrushRadius);
        ImGui::SetItemTooltip("[ and ] change it too.");
        ImGui::SliderInt("Strength", &b.strength, 1, 16);
        ImGui::SetItemTooltip("Blocks per application at the center; holding the button applies 8 times a second.");
    }
    if (brushTool && b.tool == Tool::Flatten) {
        ImGui::SliderInt("Height", &b.height, 0, kWorldSize.y - 1);
        ImGui::SetItemTooltip("Ground height to flatten to. Each stroke takes it from where it starts.");
    }
    if (brushTool && (b.tool == Tool::Paint || b.tool == Tool::Fill)) {
        if (ImGui::BeginCombo("Material", b.block == Air ? "Air (clear)" : kBlockNames[b.block])) {
            for (int i = 0; i < BlockCount; ++i)  // every block; Paint only solid ones
                if ((b.tool == Tool::Fill || isOpaque(Block(i))) && ImGui::Selectable(i == Air ? "Air (clear)" : kBlockNames[i], i == b.block))
                    b.block = Block(i);
            ImGui::EndCombo();
        }
    }
    if (brushTool && b.tool == Tool::Fill) {
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

    ImGui::SeparatorText("Prefabs");
    ImGui::Text("%zu instances, %zu prefabs", instances.size(), ed.prefabs.prefabs.size());
    for (const std::string& error : ed.prefabs.errors) ImGui::TextColored({1, 0.35f, 0.3f, 1}, "%s", error.c_str());
    if (!instances.empty() && ImGui::BeginChild("instances", {0, float(std::min<size_t>(instances.size(), 6)) * ImGui::GetFrameHeightWithSpacing()})) {
        ImGuiListClipper clipper;  // only the visible rows
        clipper.Begin(int(instances.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Instance& in = instances[size_t(i)];
                ImGui::PushID(int(in.id));
                if (ImGui::SmallButton("Go to")) actions.goTo = in.id;
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) actions.remove = in.id;
                ImGui::SameLine();
                ImGui::TextColored(in.id == ed.selected ? ImVec4(0.3f, 1, 0.5f, 1) : ImVec4(0.8f, 0.85f, 0.9f, 1), "#%u %s at %d %d %d",
                                   in.id, in.prefab.c_str(), in.pos.x, in.pos.y, in.pos.z);
                ImGui::PopID();
            }
    }
    if (!instances.empty()) ImGui::EndChild();

    ImGui::SeparatorText("Markers");
    ImGui::Text("%zu markers (max %zu)", markers.size(), kMaxMarkers);
    if (!markers.empty() && ImGui::BeginChild("markers", {0, float(std::min<size_t>(markers.size(), 6)) * ImGui::GetFrameHeightWithSpacing()})) {
        ImGuiListClipper clipper;
        clipper.Begin(int(markers.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Marker& m = markers[size_t(i)];
                ImGui::PushID(int(m.id));
                if (ImGui::SmallButton("Go to")) actions.markGoTo = m.id;
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) actions.unmark = m.id;
                ImGui::SameLine();
                ImGui::TextColored(m.id == ed.marked ? ImVec4(0.3f, 1, 0.5f, 1) : ImVec4(0.8f, 0.85f, 0.9f, 1), "#%u %s %s%s%s at %.0f %.0f r %.0f",
                                   m.id, kMarkerKindNames[int(m.kind)], m.name.c_str(), m.kind == MarkerKind::Spawn ? " / " : "",
                                   m.kind == MarkerKind::Spawn ? defs().creatureIds[size_t(m.entity)].c_str() : "", m.pos.x, m.pos.z, m.radius);
                if (m.owner) {  // made by a prefab instance: it moves and goes with it
                    auto in = std::ranges::lower_bound(instances, m.owner, {}, &Instance::id);
                    ImGui::SameLine();
                    ImGui::TextDisabled("(of %s #%u)", in != instances.end() && in->id == m.owner ? in->prefab.c_str() : "instance", m.owner);
                    ImGui::SetItemTooltip("Placed with this prefab instance: moving it moves the marker, removing it removes the marker.");
                }
                if (m.activeFrom != m.activeTo) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(out %.3f-%.3f)", m.activeFrom, m.activeTo);
                }
                ImGui::PopID();
            }
    }
    if (!markers.empty()) ImGui::EndChild();

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
    ImGui::TextDisabled("LMB apply (hold repeats)   1-8 tool   [ ] radius   R rotate prefab\nCtrl+Z undo   Ctrl+Shift+Z / Ctrl+Y redo   Ctrl+S save");
    ImGui::End();
}

void drawEditorOverlay(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const std::vector<Instance>& instances,
                       const std::vector<Marker>& markers, const Editor& ed, const std::optional<RayHit>& aim) {
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
    if (auto it = std::ranges::find(instances, ed.selected, &Instance::id); it != instances.end()) {
        Box b = instanceBox(*it);
        drawBox(b.lo, b.hi, uiColor(0.3f, 1, 0.5f, 0.9f));
    }
    // A circle at heights[i] above its points, outlined; segments behind the camera are skipped.
    auto ring = [&](glm::vec2 center, float r, const Editor::Ring& heights, ImU32 color) {
        ImVec2 points[kRingPoints + 1];
        bool front[kRingPoints + 1];
        for (int i = 0; i <= kRingPoints; ++i)
            front[i] = project({center.x + r * kRingCos[i], heights[size_t(i)], center.y + r * kRingSin[i]}, points[i]);
        for (auto [c, w] : {std::pair{uiColor(0, 0, 0, 0.6f), 4.0f}, std::pair{color, 2.0f}})
            for (int i = 0; i < kRingPoints; ++i)
                if (front[i] && front[i + 1]) dl->AddLine(points[i], points[i + 1], c, w);
    };
    // Markers: a pin with "#id name" (green when selected), regions and the selected one's radius as
    // circles on the ground under the trees. Ring heights are cached until the world changes, and
    // only the nearest kMaxRings are drawn.
    if (ed.ringEdits != ed.edits) ed.rings.clear(), ed.ringEdits = ed.edits;
    std::vector<std::pair<float, const Marker*>> rings;  // distance from the eye to the circle
    for (const Marker& m : markers) {
        const float* rgb = kMarkerColors[int(m.kind)];
        float far = glm::distance(glm::vec2(eye.x, eye.z), glm::vec2(m.pos.x, m.pos.z));
        if ((m.kind == MarkerKind::Region || m.id == ed.marked) && m.radius > 0 && far < kMarkerRange + m.radius)
            rings.push_back({std::abs(far - m.radius), &m});
        ImVec2 foot, head;
        if (far > kMarkerRange || !project(m.pos, foot) || !project(m.pos + glm::vec3(0, 2.5f, 0), head)) continue;
        dl->AddLine(foot, head, uiColor(0, 0, 0, 0.8f), 2);
        dl->AddCircleFilled(head, 6, uiColor(0, 0, 0, 0.8f));
        dl->AddCircleFilled(head, 4.5f, uiColor(rgb[0], rgb[1], rgb[2]));
        char label[128], owner[24] = "";
        if (m.owner) std::snprintf(owner, sizeof owner, " (of #%u)", m.owner);  // a prefab instance's
        std::snprintf(label, sizeof label, "#%u %s%s%s%s", m.id, m.name.c_str(), m.kind == MarkerKind::Spawn ? " - " : "",
                      m.kind == MarkerKind::Spawn ? defs().creatureIds[size_t(m.entity)].c_str() : "", owner);
        ImVec2 at(head.x + 9, head.y - ImGui::GetFontSize() / 2);
        dl->AddText(ImVec2(at.x + 1, at.y + 1), uiColor(0, 0, 0, 0.8f), label);
        dl->AddText(at, m.id == ed.marked ? uiColor(0.3f, 1, 0.5f) : uiColor(1, 1, 1), label);
    }
    if (rings.size() > kMaxRings) std::ranges::nth_element(rings, rings.begin() + kMaxRings), rings.resize(kMaxRings);
    for (auto [distance, m] : rings) {
        auto [it, fresh] = ed.rings.try_emplace(m->id);
        for (int i = 0; fresh && i <= kRingPoints; ++i) {
            int top = groundUnderTrees(world, int(std::floor(m->pos.x + m->radius * kRingCos[i])), int(std::floor(m->pos.z + m->radius * kRingSin[i])));
            it->second[size_t(i)] = top >= 0 ? float(top) + 1.05f : m->pos.y + 0.05f;
        }
        const float* rgb = kMarkerColors[int(m->kind)];
        ring({m->pos.x, m->pos.z}, m->radius, it->second, uiColor(rgb[0], rgb[1], rgb[2], m->id == ed.marked ? 0.95f : 0.6f));
    }
    if (!aim) return;

    ImU32 color = uiColor(1, 0.85f, 0.3f, 0.95f);
    if (ed.markerTool) return;
    if (ed.prefabTool) {  // the box the prefab may fill (without the foundation's reach down)
        const Prefab* p = ed.prefabs.find(ed.place.prefab);
        Instance at = ed.place;
        at.pos = aim->cell;
        if (!p || checkPlacement(world, *p, at) || ed.aimed) return;
        Box b = prefabBox(world, *p, at);
        if (!p->generated()) b.lo.y = std::max(b.lo.y, aim->cell.y + 1 + p->y);
        drawBox(b.lo, b.hi, color);
        return;
    }
    if (ed.brush.tool == Tool::Fill) {  // the box the next click fills
        drawBox(ed.corner.value_or(fillCell(ed, *aim)), fillCell(ed, *aim), color);
        return;
    }
    // The brush ring follows the ground tops around the aimed column.
    glm::vec2 center(float(aim->cell.x) + 0.5f, float(aim->cell.z) + 0.5f);
    float r = float(ed.brush.radius) + 0.5f;
    Editor::Ring heights;
    for (int i = 0; i <= kRingPoints; ++i) {
        int top = groundTop(world, int(std::floor(center.x + r * kRingCos[i])), int(std::floor(center.y + r * kRingSin[i])));
        heights[size_t(i)] = float(top >= 0 ? top : aim->cell.y) + 1.05f;
    }
    ring(center, r, heights, color);
}
