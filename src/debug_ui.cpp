#include "debug_ui.h"

#include <algorithm>
#include <cmath>

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
bool DebugUI::wantsMouse() const { return open && ImGui::GetIO().WantCaptureMouse; }

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
    ImGui::Checkbox("Character", &r.character);

    ImGui::SeparatorText("Culling");
    ImGui::Checkbox("Frustum culling", &r.frustumCulling);
    ImGui::Checkbox("Freeze culling", &r.freezeCulling);
    ImGui::SetItemTooltip("Keep the current visible chunk set, then move the camera to see what was culled.");

    ImGui::SeparatorText("This frame");
    ImGui::Text("Chunks drawn   %d / %d", s.chunksDrawn, s.chunksTotal);
    ImGui::Text("Quads drawn    %zu / %zu", s.quadsDrawn, s.opaqueQuads + s.waterQuads);
    ImGui::Text("Draw calls     %d", s.drawCalls);
}

static void playerTab(Player& p, DebugActions& actions) {
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

static void cameraTab(GameSettings& g) {
    ImGui::SliderFloat("Field of view", &g.fovDegrees, 30, 120, "%.0f deg");
    ImGui::SliderFloat("Distance", &g.cameraDistance, 2, 80, "%.1f blocks");
    ImGui::SetItemTooltip("Also the mouse wheel. Pull far back for an overview.");
    ImGui::Checkbox("Terrain collision", &g.cameraCollision);
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

ImDrawData* DebugUI::frame(float dt, GameSettings& game, RenderSettings& render, Player& player,
                           const RenderStats& stats, DebugActions& actions) {
    frameMs_[frameIndex_++ % frameMs_.size()] = dt * 1000;
    if (!open) return nullptr;

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({440, 470}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug   ( . to toggle )", &open) && ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Render")) { renderTab(render, stats); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Player")) { playerTab(player, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Camera")) { cameraTab(game); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("World")) { worldTab(game, stats, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Stats")) { statsTab(frameMs_, frameIndex_); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::Render();
    return ImGui::GetDrawData();
}
