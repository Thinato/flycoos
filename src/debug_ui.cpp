#include "debug_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

DebugUI::DebugUI(SDL_Window* window, const Renderer& renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleMain = 1.2f;
    // We render into an sRGB swapchain (linear-light output). ImGui's colors are authored in sRGB,
    // so convert them to linear or the UI comes out washed-out.
    for (ImVec4& c : style.Colors) c = {std::pow(c.x, 2.2f), std::pow(c.y, 2.2f), std::pow(c.z, 2.2f), c.w};

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = renderer.device();
    info.ColorTargetFormat = renderer.colorFormat();
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&info);
}

DebugUI::~DebugUI() {
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void DebugUI::processEvent(const SDL_Event& e) { ImGui_ImplSDL3_ProcessEvent(&e); }
bool DebugUI::wantsKeyboard() const { return open && ImGui::GetIO().WantCaptureKeyboard; }
bool DebugUI::wantsMouse() const { return ImGui::GetIO().WantCaptureMouse; }

static void renderTab(RenderSettings& r, const RenderStats& s) {
    static const char* modes[ViewModeCount] = {"Shaded", "Greedy quads", "Chunks", "Ambient occlusion", "Normals"};
    ImGui::Combo("View", &r.viewMode, modes, ViewModeCount);
    ImGui::SetItemTooltip("Greedy quads: one color per merged face. Chunks: one color per 32^3 chunk.");
    ImGui::Checkbox("Wireframe (quad edges)", &r.wireframe);

    ImGui::SeparatorText("Layers");
    ImGui::Checkbox("Sky", &r.sky);
    ImGui::SameLine();
    ImGui::Checkbox("Fog", &r.fog);
    ImGui::SameLine();
    ImGui::Checkbox("Water", &r.water);
    ImGui::SameLine();
    ImGui::Checkbox("Characters", &r.character);

    ImGui::SeparatorText("Culling");
    ImGui::Checkbox("Frustum culling", &r.frustumCulling);
    ImGui::Checkbox("Freeze culling", &r.freezeCulling);
    ImGui::SetItemTooltip("Keep the current visible chunk set, then move the camera to see what was culled.");

    ImGui::SeparatorText("This frame");
    ImGui::Text("Chunks drawn   %d / %d", s.chunksDrawn, s.chunksTotal);
    ImGui::Text("Quads drawn    %zu / %zu", s.quadsDrawn, s.opaqueQuads + s.waterQuads);
    ImGui::Text("Draw calls     %d", s.drawCalls);
}

// Health, the five attributes and what derives from them; shared by the Player and Entities tabs.
static void attributes(Player& b, bool fights) {
    Stats& s = b.stats;
    ImGui::SliderFloat("Health", &b.health, 0, float(maxHealth(s)), "%.0f");
    ImGui::SliderInt("Stamina", &s.stamina, 1, 100);
    ImGui::SetItemTooltip("+%d max health per point", kHealthPerStamina);
    ImGui::SliderInt("Strength", &s.strength, 0, 100);
    ImGui::SetItemTooltip("Physical damage + strength x attack speed");
    ImGui::SliderInt("Agility", &s.agility, 0, 100);
    ImGui::SetItemTooltip("+%.0f%% physical attack speed per point", kHastePerAgility * 100);
    ImGui::SliderInt("Intellect", &s.intellect, 0, 100);
    ImGui::SetItemTooltip("Unused until magic");
    ImGui::SliderInt("Spirit", &s.spirit, 0, 100);
    ImGui::SetItemTooltip("+%.1f health per second out of combat per point", kRegenPerSpirit);
    ImGui::Text("Max health %d, regen %.1f/s out of combat", maxHealth(s), healthRegen(s));
    if (fights) {
        const Attack& a = mainAttack(b.inv);
        float swing = swingTime(s, a);
        int lo = a.minDamage + strengthBonus(s, a), hi = a.maxDamage + strengthBonus(s, a);
        ItemId weapon = b.inv.equipped[SlotMainHand];
        ImGui::Text("%s %d-%d every %.2f s (%.1f damage/s)", weapon ? itemType(weapon).name : "Punch", lo, hi, swing,
                    float(lo + hi) / 2 / swing);
    }
    if (!b.alive()) ImGui::Text("Knocked down for %.1f s", b.deadTime);
    else if (b.combatTimer > 0) ImGui::Text("In combat, %.1f s left", b.combatTimer);
    else ImGui::TextDisabled("Out of combat");
}

static void playerTab(Player& p, DebugActions& actions) {
    attributes(p, true);
    ImGui::SeparatorText("Body");
    ImGui::DragFloat3("Position", &p.pos.x, 0.25f);
    ImGui::Text("Velocity  %.2f  %.2f  %.2f  (%.2f b/s)", p.vel.x, p.vel.y, p.vel.z, glm::length(p.vel));
    ImGui::Text("On ground %s   In water %s", p.onGround ? "yes" : "no", p.inWater ? "yes" : "no");
    if (ImGui::Button("Respawn")) actions.respawn = true;

    PlayerTuning& t = p.tuning;
    ImGui::SeparatorText("Movement");
    ImGui::SliderFloat("Walk speed", &t.walkSpeed, 1, 40, "%.1f b/s");
    ImGui::SliderFloat("Run speed", &t.runSpeed, 1, 60, "%.1f b/s");
    ImGui::SliderFloat("Jump speed", &t.jumpSpeed, 1, 40, "%.1f b/s");
    ImGui::SliderFloat("Gravity", &t.gravity, 0, 80, "%.1f b/s^2");

    ImGui::SeparatorText("Flight");
    ImGui::Checkbox("Fly", &t.fly);
    ImGui::SetItemTooltip("Space rises, Ctrl sinks, Shift is 2.5x faster.");
    ImGui::SameLine();
    ImGui::Checkbox("Noclip", &t.noclip);
    ImGui::SetItemTooltip("Fly through terrain.");
    ImGui::SliderFloat("Fly speed", &t.flySpeed, 1, 120, "%.0f b/s");
    if (ImGui::Button("Reset movement")) t = PlayerTuning{};
}

static void entitiesTab(std::vector<Entity>& entities, const Player& player, DebugActions& actions) {
    if (ImGui::Button("Respawn NPCs near player")) actions.respawnEntities = true;
    for (size_t i = 0; i < entities.size(); ++i) {
        Entity& e = entities[i];
        const EntityType& t = e.type();
        char title[96];
        std::snprintf(title, sizeof title, "%s  (Lv %d)###entity%zu", e.name.c_str(), e.level, i);
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(int(i));
        attributes(e.body, false);
        ImGui::Text("Attackable %s   Wanders %s   Talks %s", t.attackable ? "yes" : "no", t.wanders ? "yes" : "no",
                    t.talks ? "yes" : "no");
        ImGui::Text("At %.1f %.1f %.1f, %.1f from you", e.body.pos.x, e.body.pos.y, e.body.pos.z,
                    glm::distance(e.body.pos, player.pos));
        if (t.wanders)
            ImGui::Text("%s %.1f s more, %.1f from home", e.wanderDir == glm::vec2(0) ? "Standing" : "Walking",
                        e.wanderTimer, glm::distance(e.body.pos, e.home));
        if (t.attackable && ImGui::Button("Hit for 10")) damage(e, 10);
        if (t.talks && ImGui::Button("Talk")) talk(e);
        if (const char* s = e.speech()) ImGui::TextWrapped("Says: \"%s\"", s);
        ImGui::PopID();
    }
}

static void itemsTab(Player& p) {
    Inventory& inv = p.inv;
    ImGui::Text("%d / %d item slots free (C: character, B: bags)", freeSlots(inv), itemSlots(inv));
    ImGui::BeginDisabled(freeSlots(inv) == 0);
    for (int id = 1; id < ItemCount; ++id) {
        ImGui::PushID(id);
        if (ImGui::Button("Give")) giveItem(inv, ItemId(id));
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextUnformatted(itemType(ItemId(id)).name);
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("Equipped");
    for (int s = 0; s < EquipCount; ++s)
        ImGui::Text("%-10s %s", kSlotNames[s], inv.equipped[size_t(s)] ? itemType(inv.equipped[size_t(s)]).name : "-");
    for (int b = 0; b < kBagBarSlots; ++b)
        ImGui::Text("Bag %d      %s", b + 1, inv.bags[size_t(b)] ? itemType(inv.bags[size_t(b)]).name : "-");
    if (ImGui::Button("Empty inventory")) inv = Inventory{};
    ImGui::SetItemTooltip("Removes every item, equipped ones and bags included.");
}

static void cameraTab(GameSettings& g) {
    ImGui::SliderFloat("Field of view", &g.fovDegrees, 30, 120, "%.0f deg");
    ImGui::SliderFloat("Distance", &g.cameraDistance, 2, 80, "%.1f blocks");
    ImGui::SetItemTooltip("Also the mouse wheel. Pull far back for an overview.");
    ImGui::Checkbox("Terrain collision", &g.cameraCollision);
    ImGui::Checkbox("HUD", &g.hud);
    ImGui::SetItemTooltip("Nameplates, speech balloons and the health bar.");
    ImGui::SliderFloat("Mouse sensitivity", &g.mouseSensitivity, 0.001f, 0.02f, "%.4f");
}

static void worldTab(GameSettings& g, const RenderStats& s, DebugActions& actions) {
    int minutes = int(g.timeOfDay * 24 * 60);
    ImGui::SliderFloat("Time of day", &g.timeOfDay, 0, 0.9999f, "%.3f");
    ImGui::SameLine();
    ImGui::Text("%02d:%02d", minutes / 60, minutes % 60);
    ImGui::SliderFloat("Time speed", &g.timeScale, 0, 200, g.timeScale == 0 ? "paused" : "%.1fx",
                       ImGuiSliderFlags_Logarithmic);

    ImGui::SeparatorText("Generation");
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &g.seed);
    if (ImGui::Button("Regenerate world")) actions.regenerate = true;
    ImGui::SetItemTooltip("Takes about a second.");
    ImGui::Text("Size %d x %d x %d", kWorldSize.x, kWorldSize.y, kWorldSize.z);
    ImGui::Text("Generated in %.3f s, meshed + uploaded in %.3f s", s.genSeconds, s.meshSeconds);
    ImGui::Text("%zu opaque + %zu water quads (%.1f MB) in %d chunks", s.opaqueQuads, s.waterQuads,
                double(s.opaqueQuads + s.waterQuads) * 8 / (1 << 20), s.chunksTotal);
}

static void statsTab(const std::array<float, 240>& frameMs, size_t frameIndex) {
    float mean = 0, worst = 0;
    for (float ms : frameMs) {
        mean += ms;
        worst = std::max(worst, ms);
    }
    mean /= float(frameMs.size());
    ImGui::Text("Frame %.2f ms avg (%.0f fps), worst %.2f ms", mean, 1000 / std::max(mean, 1e-3f), worst);
    ImGui::PlotLines("##frames", frameMs.data(), int(frameMs.size()), int(frameIndex % frameMs.size()),
                     "frame time (ms), last 240 frames", 0, std::max(16.7f, worst), {-1, 90});
    ImGui::Text("World grid  %.0f MB", double(kWorldSize.x) * kWorldSize.y * kWorldSize.z / (1 << 20));
}

void DebugUI::window(GameSettings& game, RenderSettings& render, Player& player, std::vector<Entity>& entities,
                     const RenderStats& stats, DebugActions& actions) {
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({440, 470}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug   ( . to toggle )", &open) && ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Render")) { renderTab(render, stats); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Player")) { playerTab(player, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Items")) { itemsTab(player); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Entities")) { entitiesTab(entities, player, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Camera")) { cameraTab(game); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("World")) { worldTab(game, stats, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Stats")) { statsTab(frameMs_, frameIndex_); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void DebugUI::begin(float dt, bool cursor) {
    frameMs_[frameIndex_++ % frameMs_.size()] = dt * 1000;
    // Without a free cursor the game owns the (hidden, relative-mode) mouse.
    ImGuiIO& io = ImGui::GetIO();
    if (cursor) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

ImDrawData* DebugUI::end(GameSettings& game, RenderSettings& render, Player& player, std::vector<Entity>& entities,
                         const RenderStats& stats, DebugActions& actions) {
    if (open) window(game, render, player, entities, stats, actions);
    ImGui::Render();
    return ImGui::GetDrawData();
}
