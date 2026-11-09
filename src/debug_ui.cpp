#include "debug_ui.h"

#include <algorithm>
#include <format>
#include <print>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
#include <string>

#include <sys/stat.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include "defs.h"
#include "hud.h"
#include "melee.h"
#include "stream.h"
#include "text.h"

// Sound (ADR-037): the volumes, what's playing, the ambience's look around, and every sound to try.
static void audioTab(Audio& a, const Ambience* amb) {
    auto knob = [](const char* label, std::atomic<float>& v, float most) {  // the mixer reads them on its thread
        float f = v;
        if (ImGui::SliderFloat(label, &f, 0, most)) v = f;
    };
    if (bool m = a.muted; ImGui::Checkbox("Mute", &m)) a.muted = m;
    knob("Volume", a.volume, 1);
    for (int b = 0; b < BusCount; ++b) knob(std::format("{}##bus", kBusNames[b]).c_str(), a.bus[b], 1.5f);
    ImGui::SliderFloat("Pitch spread", &a.pitchSpread, 0, 0.2f);
    ImGui::SliderFloat("Volume spread", &a.gainSpread, 0, 0.5f);
    ImGui::SliderFloat("Hearing (blocks)", &a.hearing, 10, 100);
    ImGui::SliderFloat("Full volume within", &a.near, 1, 10);
    ImGui::Text("Voices %d / %d", a.playing(), Audio::kMaxVoices);
    if (amb) {
        ImGui::SeparatorText("Ambience");
        ImGui::Text("Around: water %.2f, trees %.2f, fire %.2f", amb->water, amb->leaves, amb->ember);
        ImGui::Text("Wind %.2f  high %.2f  birds %.2f  night %.2f  sea %.2f  stream %.2f  fire %.2f", amb->gains[0], amb->gains[1],
                    amb->gains[2], amb->gains[3], amb->gains[4], amb->gains[5], amb->gains[6]);
    }
    ImGui::SeparatorText("Latest");
    std::string latest;
    for (const std::string& n : a.log) latest += n + "  ";
    ImGui::TextWrapped("%s", latest.c_str());
    if (!a.missing.empty()) {
        std::string missing;
        for (const std::string& n : a.missing) missing += n + "  ";
        ImGui::TextColored({1, 0.6f, 0.3f, 1}, "No takes for:");
        ImGui::TextWrapped("%s", missing.c_str());
    }
    ImGui::SeparatorText("Sounds (click to play)");
    ImGui::BeginChild("sounds", {0, 260}, ImGuiChildFlags_Borders);
    for (const auto& [name, g] : a.groups) {
        if (ImGui::SmallButton(std::format("{} ({})", name, g.clips.size()).c_str()))
            a.play(name, 1, 0, 1, name.starts_with("amb_") ? BusAmbience : BusEffects);
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f s", g.clips.front().seconds());
    }
    ImGui::EndChild();
}

DebugUI::DebugUI(SDL_Window* window, const Renderer& renderer) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleMain = 1.2f;
    // We render into an sRGB swapchain (linear-light output). ImGui's colors are authored in sRGB,
    // so convert them to linear or the UI comes out washed-out.
    for (ImVec4& c : style.Colors) c = {std::pow(c.x, 2.2f), std::pow(c.y, 2.2f), std::pow(c.z, 2.2f), c.w};

    // The default font stays first (everything else uses it); Alagard is a pixel font on a 16 px grid,
    // crisp at multiples of 16 without oversampling.
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    ImFontConfig pixel;
    pixel.PixelSnapH = true;
    pixel.OversampleH = pixel.OversampleV = 1;
    pixel.Flags |= ImFontFlags_NoLoadError;
    std::string font = (dataDir() / "fonts" / "alagard.ttf").string();
    titleFont = io.Fonts->AddFontFromFileTTF(font.c_str(), 16, &pixel);
    if (!titleFont) std::println(stderr, "warning: {} didn't load; region titles use the default font", font);

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
    static const char* modes[ViewModeCount] = {"Shaded", "Greedy quads", "Chunks", "Ambient occlusion", "Normals", "Light levels"};
    ImGui::Combo("View", &r.viewMode, modes, ViewModeCount);
    ImGui::SetItemTooltip("Greedy quads: one color per merged face. Chunks: one color per 32^3 chunk.\n"
                          "Light levels: sky light blue-white, block (ember) light orange.");
    ImGui::Checkbox("Wireframe (quad edges)", &r.wireframe);

    ImGui::SeparatorText("Layers");
    ImGui::Checkbox("Sky", &r.sky);
    ImGui::SameLine();
    ImGui::Checkbox("Fog", &r.fog);
    ImGui::SameLine();
    ImGui::Checkbox("Water", &r.water);
    ImGui::SameLine();
    ImGui::Checkbox("Characters", &r.character);

    ImGui::Checkbox("Plants", &r.plants);
    ImGui::SetItemTooltip("Grass and flowers (plants.h). Off: their cells look empty (A/B their cost).");
    ImGui::SliderFloat("Plant radius", &r.plantRadius, 16, 160, "%.0f blocks");
    ImGui::SetItemTooltip("Plants are drawn this far from the eye, growing in over the last 12 blocks; the detailed model within %.0f.", kPlantNear);
    ImGui::SliderFloat("Wind", &r.sway, 0, 3, "%.1f");
    ImGui::SetItemTooltip("How fast the grass sways: 0 holds it still, leaning.");

    ImGui::SeparatorText("Culling");
    ImGui::Checkbox("Frustum culling", &r.frustumCulling);
    ImGui::Checkbox("Freeze culling", &r.freezeCulling);
    ImGui::SetItemTooltip("Keep the current visible chunk set, then move the camera to see what was culled.");

    ImGui::SeparatorText("Lighting");
    ImGui::Checkbox("Occlusion color ramp", &r.occlusionRamp);
    ImGui::SetItemTooltip("Off: the old linear AO without the cool corner tint.");
    ImGui::BeginDisabled(!r.occlusionRamp);
    if (ImGui::SliderFloat4("AO curve", &r.aoCurve.x, 0, 1, "%.2f")) {
        // Keep it monotonic (each value between its neighbours) and level 0 below 1 (the shader divides by 1 - it).
        for (int i = 0; i < 4; ++i)
            r.aoCurve[i] = std::clamp(r.aoCurve[i], i > 0 ? r.aoCurve[i - 1] : 0.0f, i < 3 ? r.aoCurve[i + 1] : 1.0f);
        r.aoCurve.x = std::min(r.aoCurve.x, 0.95f);
    }
    ImGui::SetItemTooltip("Ambient light kept at AO level 0 (corner) .. 3 (open).");
    ImGui::ColorEdit3("Shadow tint", &r.tintColor.x);
    ImGui::SliderFloat("Tint strength", &r.tintStrength, 0, 1, "%.2f");
    ImGui::EndDisabled();
    ImGui::Checkbox("Voxel light", &r.voxelLight);
    ImGui::SetItemTooltip("Sky and ember light per face. Off: everything under open sky, embers dark.");
    ImGui::BeginDisabled(!r.voxelLight);
    ImGui::SliderFloat("Sun gate", &r.sunGate, 0, 0.99f, "%.2f");
    ImGui::SetItemTooltip("Sun light fades in as sky light goes from this fraction to full (no shadow map).");
    ImGui::SliderFloat("Falloff", &r.lightFalloff, 1, 6, "%.1f");
    ImGui::SetItemTooltip("Light level curve: 1 linear, higher drops faster away from the light.");
    ImGui::ColorEdit3("Ember color", &r.emberColor.x);
    ImGui::SliderFloat("Ember strength", &r.emberStrength, 0, 4, "%.2f");
    ImGui::SliderFloat("Dark ambient", &r.darkAmbient, 0, 0.2f, "%.3f");
    ImGui::SetItemTooltip("Light left where neither sky nor embers reach.");
    ImGui::EndDisabled();
    ImGui::Text("Light %.1f MiB in %d chunks, filled in %.2f s; last relight %.2f ms", double(s.lightBytes) / (1 << 20),
                s.lightChunks, s.lightSeconds, s.relightMs);

    ImGui::SeparatorText("Colour patches (docs/ART.md)");
    ImGui::SliderFloat("Patch strength", &r.patchStrength, 0, 2, "%.2f");
    ImGui::SetItemTooltip("Times every block's strength. 0: each block exactly its palette colour.");
    ImGui::SliderFloat("Patch size", &r.patchSize, 0.25f, 4, "%.2f");
    static int block = Grass;
    ImGui::Combo("Block", &block, kBlockNames, BlockCount);
    BlockTone& t = r.tones[size_t(block)];
    ImGui::ColorButton("Main", {kPalette[block][0], kPalette[block][1], kPalette[block][2], 1});
    ImGui::SameLine();
    ImGui::Text("main (kPalette)");
    ImGui::ColorEdit3("Variant a", t.a);
    ImGui::ColorEdit3("Variant b", t.b);
    ImGui::SliderFloat("Size (voxels)", &t.size, 1, 64, "%.0f");
    ImGui::SliderFloat("Strength", &t.strength, 0, 1, "%.2f");
    ImGui::SetItemTooltip("Runtime only: copy tuned values into kTones (src/world.h).");
    if (ImGui::Button("Reset tones")) r.tones = kTones;
    ImGui::SliderFloat("Leaf shade", &r.leafShade, 0, 1.5f, "%.2f");
    ImGui::SetItemTooltip("Tree crowns, dark at the bottom to light at the top. 0: one colour per leaf kind.");

    ImGui::SeparatorText("This frame");
    ImGui::Text("Chunks drawn   %d / %d", s.chunksDrawn, s.chunksTotal);
    ImGui::Text("Quads drawn    %zu / %zu", s.quadsDrawn, s.opaqueQuads + s.waterQuads);
    ImGui::Text("Draw calls     %d", s.drawCalls);
    ImGui::Text("Plants drawn   %zu (%zu quads)", s.plantsDrawn, s.plantQuadsDrawn);
}

// Health, the five attributes and what derives from them; shared by the Player and Entities tabs.
static void attributes(Player& b, bool fights) {
    Stats& s = b.stats;
    ImGui::SliderFloat("Health", &b.health, 0, float(maxHealth(sheet(b))), "%.0f");
    ImGui::SliderInt("Vitality", &s.stamina, 1, 100);
    ImGui::SetItemTooltip("+%d max health per point", kHealthPerStamina);
    ImGui::SliderInt("Strength", &s.strength, 0, 100);
    ImGui::SetItemTooltip("Adds to hits by each weapon's scaling with it, x its speed (ADR-048)");
    ImGui::SliderInt("Agility", &s.agility, 0, 100);
    ImGui::SetItemTooltip("+%.0f%% physical attack speed per point; adds to hits by each weapon's scaling", kHastePerAgility * 100);
    ImGui::SliderInt("Intellect", &s.intellect, 0, 100);
    ImGui::SetItemTooltip("Adds to hits by each weapon's scaling (none scales with it yet)");
    ImGui::SliderInt("Spirit", &s.spirit, 0, 100);
    ImGui::SetItemTooltip("+%.1f health per second out of combat per point; adds to hits by each weapon's scaling", kRegenPerSpirit);
    StatSheet total = sheet(b);
    ImGui::Text("Max health %d, regen %.1f/s out of combat", maxHealth(total), healthRegen(total));
    if (fights) {
        const Attack& a = mainAttack(b.inv);
        ItemId weapon = b.inv.equipped[SlotMainHand];
        StatSheet with = sheet(b, skillOf(weapon));  // the weapon skill's conditional modifiers too
        float swing = swingTime(with, a);
        int lo = hitBeforeArmor(with, a, a.minDamage), hi = hitBeforeArmor(with, a, a.maxDamage);
        ImGui::Text("%s %d-%d every %.2f s (%.1f damage/s)", weapon ? itemType(weapon).name : "Punch", lo, hi, swing,
                    float(lo + hi) / 2 / swing);
    }
    if (b.hitTime < 5) ImGui::Text("Took %d damage %.1f s ago", b.lastDamage, b.hitTime);
    if (!b.alive()) ImGui::Text("Knocked down for %.1f s", b.deadTime);
    else if (b.combatTimer > 0) ImGui::Text("In combat, %.1f s left", b.combatTimer);
    else ImGui::TextDisabled("Out of combat");
}

// The stat pipeline (ADR-022): each stat's base and final value, and every modifier with its source.
static void statSheet(const Combatant& b) {
    StatSheet base = compute(b.stats), total = sheet(b);
    if (ImGui::BeginTable("sheet", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const char* h : {"stat", "base", "final"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < size_t(Stat::Count); ++i) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(), ImGui::TextUnformatted(kStatIds[i]);
            ImGui::TableNextColumn(), ImGui::Text("%.4g", double(base.v[i]));
            ImGui::TableNextColumn(), ImGui::Text("%.4g", double(total.v[i]));
        }
        ImGui::EndTable();
    }
    static const char* const kKinds[] = {"flat", "increased", "more"};
    std::vector<Modifier> mods = collect(b);
    if (mods.empty()) ImGui::TextDisabled("No modifiers");
    for (const Modifier& m : mods)
        ImGui::BulletText("%s: %s %s %+g%s%s", m.source, kStatIds[size_t(m.stat)], kKinds[size_t(m.kind)], double(m.value),
                          m.with != Skill::Count ? " with " : "", m.with != Skill::Count ? kSkillIds[size_t(m.with)] : "");
    ImGui::TextDisabled("final = (base + flat) x (1 + increased) x (1 + more)...");
}

// A body's status build-ups and timed effects (ADR-046), with buttons that put each status on it now
// (`apply` deals the burst).
static void statusPanel(const Player& b, const std::function<void(Status)>& apply) {
    StatSheet s = sheet(b);
    for (size_t i = 0; i < size_t(Status::Count); ++i) {
        float resist = s[resistStat(Status(i))];
        ImGui::ProgressBar(resist > 0 ? std::min(b.buildup[i] / resist, 1.0f) : 0, {160, 0},
                           std::format("{} {:.0f} / {:.0f}", kStatusIds[i], b.buildup[i], resist).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(std::format("Apply##{}", kStatusIds[i]).c_str())) apply(Status(i));
        ImGui::SameLine();
        ImGui::TextDisabled("hits add %.0f", double(s[buildupStat(Status(i))]));
    }
    for (const TimedEffect& e : b.effects)
        ImGui::BulletText("%s from %s: %.1f / %.0f s, %d stack%s, %zu modifier%s", kEffectNames[size_t(e.kind)], e.source.c_str(),
                          double(e.seconds), double(e.length), e.stacks, e.stacks == 1 ? "" : "s", e.mods.size(), e.mods.size() == 1 ? "" : "s");
    if (b.effects.empty()) ImGui::TextDisabled("No timed effects");
}

static void playerTab(Session& session, float& armorK, DebugActions& actions, const char* region) {
    Player& p = session.player;
    attributes(p, true);
    ImGui::SeparatorText("Stat sheet");
    statSheet(p);
    ImGui::SeparatorText("Armor");
    ImGui::SliderFloat("Armor K", &armorK, 1, 200, "%.0f");
    ImGui::SetItemTooltip("Damage reduction = armor / (armor + K x attacker level).\n"
                          "K is the armor that halves a level 1 attacker's damage.");
    float a = sheet(p)[Stat::Armor];
    ImGui::Text("Armor %.0f: -%.1f%% damage vs level 1, -%.1f%% vs level 2", double(a),
                armorReduction(a, 1, armorK) * 100, armorReduction(a, 2, armorK) * 100);
    ImGui::SeparatorText("Body");
    if (glm::vec3 at = p.pos; ImGui::DragFloat3("Position", &at.x, 0.25f)) p.teleport(at);
    ImGui::Text("Velocity  %.2f  %.2f  %.2f  (%.2f b/s)", p.vel.x, p.vel.y, p.vel.z, glm::length(p.vel));
    ImGui::Text("On ground %s   In water %s", p.onGround ? "yes" : "no", p.inWater ? "yes" : "no");
    ImGui::Text("Region %s", region ? region : "-");
    if (ImGui::Button("Respawn")) actions.respawn = true;

    PlayerTuning& t = p.tuning;
    ImGui::SeparatorText("Movement");
    ImGui::SliderFloat("Walk speed", &t.walkSpeed, 1, 40, "%.1f b/s");
    ImGui::SliderFloat("Run speed", &t.runSpeed, 1, 60, "%.1f b/s");
    ImGui::SliderFloat("Jump speed", &t.jumpSpeed, 1, 40, "%.1f b/s");
    ImGui::SliderFloat("Gravity", &t.gravity, 0, 80, "%.1f b/s^2");
    ImGui::SliderFloat("Roll speed", &t.rollSpeed, 1, 40, "%.1f b/s");
    ImGui::SetItemTooltip("At the roll's start; it slows to half by the end. Alt (Option) or Circle rolls; just after a jump it rolls through the air; jumping cuts it short.");
    ImGui::SliderFloat("Roll length", &t.rollSeconds, 0.1f, 1.5f, "%.2f s");
    ImGui::Text("Rolling %s   (%.1f blocks a roll)", p.rolling() ? p.backstep ? "a backstep" : "yes" : "no", double(t.rollSpeed * t.rollSeconds * 0.75f));
    if (ImGui::TreeNode("Equip load and the backstep (ADR-038, ADR-039)")) {
        float w = equipWeight(p.inv), most = maxEquipLoad(p, t);
        ImGui::Text("Load %.1f / %.1f (%.0f%%): %s   rolled %s", double(w), double(most), double(w / most * 100), kLoadNames[size_t(p.load)],
                    kLoadNames[size_t(p.rollLoad)]);
        ImGui::SliderFloat("Load base", &t.loadBase, 0, 200, "%.0f");
        ImGui::SliderFloat("  + per vitality", &t.loadPerVitality, 0, 10, "%.2f");
        ImGui::SliderFloat("Light under", &t.lightLoad, 0, 1, "%.2f");
        ImGui::SliderFloat("Heavy over", &t.heavyLoad, 0, 1, "%.2f");
        ImGui::SliderFloat("Light roll speed", &t.lightRollSpeed, 1, 40, "%.1f b/s");
        ImGui::SliderFloat("Heavy roll speed", &t.heavyRollSpeed, 1, 40, "%.1f b/s");
        ImGui::SliderFloat("Heavy roll length", &t.heavyRollSeconds, 0.1f, 1.5f, "%.2f s");
        ImGui::SliderFloat("Heavy roll recovery", &t.heavyRollRecovery, 0, 1, "%.2f s");
        ImGui::SliderFloat("Heavy roll i-frames to", &gMelee.heavyIframeTo, 0, 1, "%.2f s");
        ImGui::SliderFloat("Heavy stamina regen", &gMelee.heavyRegen, 0, 1, "x %.2f");
        ImGui::SliderFloat("Overloaded walk", &t.overloadedSpeed, 0.1f, 1, "x %.2f");
        ImGui::SliderFloat("Backstep speed", &t.backstepSpeed, 1, 30, "%.1f b/s");
        ImGui::SetItemTooltip("The roll button standing still hops back, still facing ahead; attacking out of it is a lunging sweep.");
        ImGui::SliderFloat("Backstep length", &t.backstepSeconds, 0.1f, 1, "%.2f s");
        ImGui::SliderFloat("Backstep cost", &gMelee.backstepStamina, 0, 60, "%.0f");
        ImGui::SliderFloat("Backstep i-frames from", &gMelee.backstepIframeFrom, 0, 1, "%.2f s");
        ImGui::SliderFloat("Backstep i-frames to", &gMelee.backstepIframeTo, 0, 1, "%.2f s");
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Melee (ADR-035)");
    MeleeTuning& m = gMelee;
    ImGui::ProgressBar(p.stamina / m.staminaMax, ImVec2(-FLT_MIN, 0), std::format("stamina {:.0f} / {:.0f}", p.stamina, m.staminaMax).c_str());
    if (p.punching())
        ImGui::Text("%s  %.2f / %.2f s%s%s", kMoveNames[size_t(p.move)], double(p.moveTime), double(moveLength(p)),
                    p.holding ? std::format("  charging {:.2f} s", p.charge).c_str() : "", p.charged ? "  charged" : "");
    else ImGui::TextDisabled("not attacking (last: %s, %.1f s ago)", kMoveNames[size_t(p.lastMove)], double(p.sinceMove));
    ImGui::Text("Guard %s   blocked %.1f s ago   %s %.2f s", p.guarding ? "up" : "down", double(std::min(p.blockTime, 99.0f)),
                p.broken ? "guard broken" : "reeling", double(p.stagger));
    ImGui::Text("i-frames %s   hyperarmor %s", invulnerable(p) ? "yes" : "no", hyperarmor(p) ? "yes" : "no");
    if (ImGui::TreeNode("Weapon class moveset (ADR-044)")) {  // the held weapon's; tune in data/weapon_classes.json, then Data > Reload defs
        ItemId w = p.inv.equipped[SlotMainHand];
        const WeaponClassDef& c = weaponClass(classOf(w));
        ImGui::Text("%s: %s, speed x %.2f (weapon) x %.2f (class), hit-stop x %.2f", w ? itemType(w).name : "Bare hands", c.name.c_str(),
                    double(weaponAttack(w).speed), double(c.speed), double(c.hitStop));
        if (c.ranged) ImGui::TextUnformatted("Ranged: shoots arrows, always two-handed (ADR-050; Bows and projectiles below)");
        else ImGui::Text("Reach %.2f blocks, %.0f degrees either side%s", double(c.reach), double(c.arc),
                         c.twoHanded ? ", always two-handed (ADR-049)" : "");
        ImGui::TextDisabled("Edit data/weapon_classes.json, then Data > Reload defs.");
        if (ImGui::BeginTable("moves", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (const char* h : {"move", "s", "hold", "hit", "chain", "cancel", "dmg", "stance", "stam", "lunge", "kind"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (size_t i = 1; i < size_t(Move::Count); ++i) {
                const MoveSpec& ms = c.moves[i];
                if (ms.seconds <= 0) continue;  // not this class's (a bow has only the shot; melee classes no shot)
                ImGui::TableNextRow();
                ImGui::TableNextColumn(), ImGui::TextUnformatted(kMoveNames[i]);
                for (float v : {ms.seconds, ms.holdAt, ms.impact, ms.chain, ms.cancel, ms.damage, ms.stance, ms.stamina, ms.lunge})
                    ImGui::TableNextColumn(), ImGui::Text("%.3g", double(v));
                ImGui::TableNextColumn(), ImGui::Text("%s%s", ms.heavy ? "heavy " : "", ms.thrust ? "thrust" : "");
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    {  // ADR-036
        const Shield* s = shieldOf(p);
        Guard g = guardOf(p), strike = guardOf(p, DamageType::Strike), pierce = guardOf(p, DamageType::Pierce);
        float parry = parryOf(p);
        ImGui::Text("Guard with %s: stops %.0f/%.0f/%.0f%% (slash/strike/pierce), boost %.0f%%%s%s", s ? itemType(p.inv.equipped[SlotOffHand]).name
                    : p.inv.equipped[SlotMainHand] ? (twoHanding(p) ? "the weapon in both hands" : "the weapon") : "bare arms", double(g.negation * 100), double(strike.negation * 100),
                    double(pierce.negation * 100), double(g.boost * 100),
                    parry > 0 ? std::format(", parries (x {:.2f})", parry).c_str() : "", g.boost >= m.bounceBoost ? ", bounces" : "");
        ImGui::Text("Perfect guard %s   (guard up %.2f s, %s)", perfectGuard(p) ? "NOW" : "no", double(std::min(p.guardTime, 99.0f)),
                    p.perfectReady ? "rested" : "raised too soon");
        if (p.move == Move::Parry)
            ImGui::Text("Parry %.2f s: %s", double(p.moveTime / p.moveScale), parries(p, p.pos) ? "ACTIVE" : "weak guard");
    }
    if (ImGui::TreeNode("Stamina, timing and the guard")) {
        ImGui::SliderFloat("Max stamina", &m.staminaMax, 10, 300, "%.0f");
        ImGui::SliderFloat("Regen", &m.staminaRegen, 0, 200, "%.0f / s");
        ImGui::SliderFloat("Regen delay", &m.staminaDelay, 0, 3, "%.2f s");
        ImGui::SliderFloat("Regen guarding", &m.guardRegen, 0, 1, "x %.2f");
        ImGui::SliderFloat("Run drain (in combat)", &m.runDrain, 0, 50, "%.0f / s");
        ImGui::SliderFloat("Roll cost", &m.rollStamina, 0, 60, "%.0f");
        ImGui::SliderFloat("Tap or hold", &m.tapSeconds, 0.05f, 0.5f, "%.2f s");
        ImGui::SetItemTooltip("The attack button held this long makes a heavy, not a light.");
        ImGui::SliderFloat("Charge time", &m.chargeSeconds, 0.1f, 2, "%.2f s");
        ImGui::SliderFloat("Charged damage", &m.chargeDamage, 1, 3, "x %.2f");
        ImGui::SliderFloat("Charged stance", &m.chargeStance, 1, 3, "x %.2f");
        ImGui::SliderFloat("Input buffer", &m.bufferSeconds, 0, 1.5f, "%.2f s");
        ImGui::SliderFloat("Chain grace", &m.chainGrace, 0, 1, "%.2f s");
        ImGui::SetItemTooltip("After a light ends, the next one still goes on with the chain this long.");
        ImGui::SliderFloat("Sword guard stops", &m.guardNegation, 0, 1, "%.2f of the damage");
        ImGui::SliderFloat("Arms guard stops", &m.fistNegation, 0, 1, "%.2f of the damage");
        ImGui::SliderFloat("Guard boost", &m.guardBoost, 0, 1, "%.2f of the stamina");
        ImGui::SliderFloat("Block stamina", &m.blockStamina, 0, 60, "%.0f");
        ImGui::SliderFloat("  + per damage", &m.blockStaminaPerDamage, 0, 10, "%.1f");
        ImGui::SliderFloat("Guard counter window", &m.counterWindow, 0.1f, 2, "%.2f s");
        ImGui::SliderFloat("Flinch", &m.flinchSeconds, 0, 1, "%.2f s");
        ImGui::SliderFloat("Stance break", &m.breakSeconds, 0.2f, 6, "%.2f s");
        ImGui::SliderFloat("Guard break", &m.guardBreakSeconds, 0.2f, 4, "%.2f s");
        ImGui::SliderFloat("Roll i-frames from", &m.iframeFrom, 0, 1, "%.2f s");
        ImGui::SliderFloat("Roll i-frames to", &m.iframeTo, 0, 1, "%.2f s");
        ImGui::SliderFloat("NPC tracking stops", &m.trackSeconds, 0, 1, "%.2f s before the strike");
        if (ImGui::Button("Reset melee")) m = MeleeTuning{};
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Shields and the parry (ADR-036)")) {
        ImGui::SliderFloat("Parry from", &m.parryFrom, 0, 0.8f, "%.2f s");
        ImGui::SliderFloat("Parry to", &m.parryTo, 0, 0.8f, "%.2f s");
        ImGui::SetItemTooltip("Seconds into a parry (at the shield's speed 1) an attack is deflected; before and after, it's a weak guard.");
        ImGui::SliderFloat("Parried reels", &m.parrySeconds, 0.2f, 4, "%.2f s");
        ImGui::SliderFloat("Parry's weak guard", &m.parryGuard, 0, 1, "x %.2f");
        ImGui::SliderFloat("Recoil", &m.recoilSeconds, 0, 2, "%.2f s");
        ImGui::SetItemTooltip("An attack that bounces off a greatshield, or a parry short of the ones it takes, rocks the attacker back this long.");
        ImGui::SliderFloat("Bounce: boost from", &m.bounceBoost, 0, 1, "%.2f");
        ImGui::SliderFloat("Bounce: stance under", &m.bounceStance, 0, 60, "%.0f");
        ImGui::SliderFloat("Fists parry", &m.fistParry, 0, 4, "x %.2f");
        ImGui::SetItemTooltip("The parry's length with bare fists (0: they can't). A knife's is its item's; a shield's comes first.");
        ImGui::SliderFloat("Perfect guard", &m.perfectGuardSeconds, 0, 0.5f, "%.2f s");
        ImGui::SetItemTooltip("A hit this soon after the guard goes up is a perfect guard (ADR-041): no damage, less stamina.");
        ImGui::SliderFloat("  stamina", &m.perfectStamina, 0, 1, "x %.2f");
        ImGui::SliderFloat("  rearm", &m.perfectRearm, 0, 2, "%.2f s");
        ImGui::SetItemTooltip("The guard must have been down this long before (tapping it doesn't keep the window open).");
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Criticals (ADR-045)")) {
        StatSheet w = sheet(p, skillOf(p.inv.equipped[SlotMainHand]));
        ImGui::Text("Crit chance %.1f%%, crit damage +%.0f%% (base %.0f%%, +%.0f%%)", double(w[Stat::CritChance] * 100),
                    double(w[Stat::CritDamage] * 100), double(kBaseCritChance * 100), double(kBaseCritDamage * 100));
        ImGui::Text("Last hit taken: %d%s", p.lastDamage, p.lastCrit ? " (critical)" : "");
        float angle = glm::degrees(std::acos(std::clamp(m.backstabAngle, -1.0f, 1.0f)));
        if (ImGui::SliderFloat("Backstab angle", &angle, 5, 90, "%.0f deg off its back")) m.backstabAngle = std::cos(glm::radians(angle));
        ImGui::SliderFloat("Backstab wait", &m.backstabWait, 0, 6, "%.1f s after it was hurt");
        ImGui::SetItemTooltip("An NPC hurt this recently can't be backstabbed: it's turning to face its attacker.");
        ImGui::SliderFloat("Hit shake", &m.hitShake, 0, 1, "%.2f trauma");
        ImGui::SetItemTooltip("Camera trauma from a riposte or backstab; random crits add 60%%, heavy hits by their stance damage.");
        ImGui::ProgressBar(p.trauma, {-FLT_MIN, 0}, std::format("trauma {:.2f}", p.trauma).c_str());
        if (ImGui::Button("Test shake")) p.trauma = 1;
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Scaling and two-handing (ADR-048)")) {
        bool armed = p.inv.equipped[SlotMainHand] != NoItem;
        ImGui::BeginDisabled(!armed);
        ImGui::Checkbox("Two-handed (F / Triangle+R1)", &p.twoHanded);
        ImGui::EndDisabled();
        if (!armed) ImGui::SameLine(), ImGui::TextDisabled("(nothing in the main hand)");
        else if (weaponClass(classOf(p.inv.equipped[SlotMainHand])).twoHanded) ImGui::SameLine(), ImGui::TextDisabled("(a great weapon: always, ADR-049)");
        ImGui::ProgressBar(p.gripBlend, {160, 0}, std::format("left hand on the hilt {:.2f}", p.gripBlend).c_str());
        ImGui::SliderFloat("Two-handed strength", &m.twoHandStrength, 1, 3, "x %.2f");
        ImGui::SliderFloat("Two-handed guard stops", &m.twoHandNegation, 0, 1, "%.2f of the damage");
        ImGui::SliderFloat("Two-handed guard boost", &m.twoHandBoost, 0, 1, "%.2f of the stamina");
        ImGui::SetItemTooltip("One-handed, the weapon's guard: %.2f and %.2f (Stamina, timing and the guard).", double(m.guardNegation), double(m.guardBoost));
        StatSheet s = sheet(p);
        ImGui::Text("Attribute scaling x %.2f (a stat: talents may raise it)", double(s[Stat::Scaling]));
        // Every weapon in the data: its scaling per attribute (grade, number) and what it adds now with these attributes.
        if (ImGui::BeginTable("scaling", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (const char* h : {"weapon", "speed", "str", "agi", "int", "spi", "adds now (this grip)"}) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (int id = 0; id < int(defs().items.size()); ++id) {
                if (id != NoItem && itemType(ItemId(id)).slot != SlotMainHand) continue;
                const Attack& a = weaponAttack(ItemId(id));
                ImGui::TableNextRow();
                ImGui::TableNextColumn(), ImGui::TextUnformatted(id ? itemType(ItemId(id)).name : "Fists");
                ImGui::TableNextColumn(), ImGui::Text("%.2f", double(a.speed));
                for (size_t i = 0; i < std::size(kScalingStats); ++i)
                    ImGui::TableNextColumn(), ImGui::Text("%c %.2f", scalingGrade(a.scaling[i]), double(a.scaling[i]));
                StatSheet with = sheet(p, skillOf(ItemId(id)));
                ImGui::TableNextColumn(), ImGui::Text("+%d", attributeBonus(with, a));
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Status effects (ADR-046)")) {
        statusPanel(p, [&](Status s) { hurt(p, afflict(p, s, "the debug window"), false, s); });
        StatusTuning& st = gStatus;
        ImGui::SliderFloat("Build-up decay", &st.decay, 0, 50, "%.1f / s");
        ImGui::SliderFloat("Bleed share", &st.bleedShare, 0, 1, "%.2f of max health");
        ImGui::SliderFloat("Bleed flat", &st.bleedFlat, 0, 50, "%.0f");
        ImGui::SliderFloat("Poison seconds", &st.poisonSeconds, 1, 60, "%.0f s");
        ImGui::SliderFloat("Poison share", &st.poisonShare, 0, 0.1f, "%.3f of max health / s");
        ImGui::SliderFloat("Poison flat", &st.poisonFlat, 0, 10, "%.1f / s");
        ImGui::SliderFloat("Frost share", &st.frostShare, 0, 1, "%.2f of max health");
        ImGui::SliderFloat("Frost flat", &st.frostFlat, 0, 50, "%.0f");
        ImGui::SliderFloat("Frost seconds", &st.frostSeconds, 1, 60, "%.0f s");
        ImGui::SliderFloat("Frost damage taken", &st.frostTaken, 0, 1, "+%.2f");
        ImGui::SetItemTooltip("Frostbite's increased damage taken (a modifier in the stat pipeline while it lasts).");
        if (ImGui::Button("Reset statuses")) st = StatusTuning{};
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Flasks and rest (ADR-047)")) {
        StatSheet s = sheet(p);
        int most = maxFlasks(s);
        ImGui::SliderInt("Flask charges", &p.flasks, 0, std::max(most, 1), std::format("%d of {}", most).c_str());
        ImGui::Text("Each heals %.0f%% of max health (%.0f)", double(s[Stat::FlaskHealing] * 100), double(s[Stat::FlaskHealing]) * maxHealth(s));
        if (p.move == Move::Drink)
            ImGui::ProgressBar(moveProgress(p), {-FLT_MIN, 0}, p.hitPending ? "drinking: before the drink frame" : "drinking: drunk");
        ImGui::SliderFloat("Walk while drinking", &m.drinkSpeed, 0.1f, 5, "%.1f b/s");
        ImGui::Text("Can rest here: %s", canRest(session) ? "yes" : "no (alive, not hit for 5 s, nothing angry near, by an Ember)");
        if (session.restPoint) ImGui::Text("Rest point: %.1f %.1f %.1f", double(session.restPoint->x), double(session.restPoint->y), double(session.restPoint->z));
        else ImGui::TextDisabled("Rest point: none yet (the player spawn)");
        if (ImGui::Button("Rest now")) rest(p), session.restPoint = p.pos;
        ImGui::SameLine();
        if (ImGui::Button("Forget rest point")) session.restPoint.reset();
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Bows and projectiles (ADR-050)")) {
        RangedTuning& r = gRanged;
        ImGui::SliderInt("Arrows", &p.arrows, 0, kQuiver, std::format("%d of {}", kQuiver).c_str());
        ImGui::Text("%s, aiming %s (%.2f)", ranged(p) ? "A bow in hand" : "No bow in hand (Items: give a Short Bow)", p.aiming ? "yes" : "no", double(p.aimBlend));
        if (p.move == Move::Shot)
            ImGui::ProgressBar(drawOf(p), {-FLT_MIN, 0}, std::format("{} {:.0f}%", p.hitPending ? "drawn" : "loosed", drawOf(p) * 100).c_str());
        int flying = int(std::ranges::count(session.projectiles, false, &Projectile::stuck));
        ImGui::Text("Projectiles: %d flying, %d stuck (at most %zu)", flying, int(session.projectiles.size()) - flying, kMaxProjectiles);
        for (size_t i = 0; i < std::min<size_t>(session.projectiles.size(), 8); ++i) {
            const Projectile& a = session.projectiles[i];
            ImGui::Text("  %s at %.1f %.1f %.1f, %.1f b/s, %.1f s, power %.2f, stance %.1f", a.stuck ? "stuck " : "flying", double(a.pos.x),
                        double(a.pos.y), double(a.pos.z), double(a.stuck ? 0 : glm::length(a.vel)), double(a.age), double(a.power), double(a.stance));
        }
        ImGui::SliderFloat("Gravity", &r.gravity, 0, 40, "%.1f b/s^2");
        ImGui::SliderFloat("Speed, no draw", &r.minSpeed, 1, 100, "%.0f b/s");
        ImGui::SliderFloat("Speed, full draw", &r.maxSpeed, 1, 150, "%.0f b/s");
        ImGui::Text("Farthest at full draw: %.0f blocks (45 degrees, level ground)", double(r.maxSpeed * r.maxSpeed / std::max(r.gravity, 0.01f)));
        ImGui::SliderFloat("Draw to full", &r.drawSeconds, 0.05f, 3, "%.2f s at the hold pose");
        ImGui::SliderFloat("Power, no draw", &r.minPower, 0, 1, "x %.2f damage and stance");
        ImGui::SliderFloat("Hip shot pitch", &r.hipPitch, -0.3f, 0.5f, "%.2f rad");
        ImGui::SliderFloat("Walk while aiming", &r.aimWalk, 0.1f, 6, "%.1f b/s");
        ImGui::SliderFloat("Flight time", &r.flightSeconds, 0.5f, 20, "%.1f s");
        ImGui::SliderFloat("Stuck for", &r.stuckSeconds, 0, 300, "%.0f s");
        ImGui::SliderFloat("Pick up within", &r.pickupReach, 0, 4, "%.1f blocks");
        ImGui::SliderFloat("Body box half-width", &r.hitRadius, 0.1f, 1, "%.2f blocks");
        if (ImGui::Button("Fill the quiver")) p.arrows = kQuiver;
        ImGui::SameLine();
        if (ImGui::Button("Clear projectiles")) session.projectiles.clear();
        ImGui::SameLine();
        if (ImGui::Button("Reset bows")) r = RangedTuning{};
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Flight");
    ImGui::Checkbox("Fly", &t.fly);
    ImGui::SetItemTooltip("Space rises, Ctrl sinks, Shift is 2.5x faster.");
    ImGui::SameLine();
    ImGui::Checkbox("Noclip", &t.noclip);
    ImGui::SetItemTooltip("Fly through terrain.");
    ImGui::SliderFloat("Fly speed", &t.flySpeed, 1, 120, "%.0f b/s");
    if (ImGui::Button("Reset movement")) t = PlayerTuning{};
}

static void entitiesTab(Session& session, DebugActions& actions, EntityKind& spawnKind) {
    std::vector<Entity>& entities = session.entities;
    const Player& player = session.player;
    // Hits from here count for quests as the player's would.
    auto hit = [&](Entity& e, int amount, Status kind = Status::Count) {
        if (damage(e, amount, false, kind) && !e.body.alive()) session.events.push_back(Killed{e.kind, e.spawn.id});
    };
    if (ImGui::Button("Respawn NPCs at their markers")) actions.respawnEntities = true;
    const std::vector<std::string>& kinds = defs().creatureIds;
    if (size_t(spawnKind) >= kinds.size()) spawnKind = EntityKind(0);
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##spawnKind", kinds.empty() ? "-" : kinds[size_t(spawnKind)].c_str())) {
        for (size_t k = 0; k < kinds.size(); ++k)
            if (ImGui::Selectable(kinds[k].c_str(), size_t(spawnKind) == k)) spawnKind = EntityKind(k);
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(kinds.empty());
    if (ImGui::Button("Spawn 5 blocks ahead (F6)")) spawnAhead(entities, session.world, player, spawnKind, SDL_rand_bits());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Remove spawned")) std::erase_if(entities, [](const Entity& e) { return !e.spawn.id; });
    ImGui::SetItemTooltip("The NPCs spawned here or with F6 (not from a marker). They don't come back with the game either.");
    if (ImGui::TreeNode("Fighting AI: tokens, strings, answers (ADR-042, ADR-043)")) {
        AiTuning& a = gAi;
        ImGui::SliderInt("Attackers at once", &a.attackers, 1, 6);
        ImGui::SetItemTooltip("Attack tokens: this many fighters on you may swing; the rest circle on the ring and wait.");
        ImGui::SliderFloat("Waiting ring", &a.waitRing, 2.5f, 8, "%.1f blocks");
        ImGui::SliderFloat("Circling speed", &a.strafeSpeed, 0, 1.5f, "x%.2f walk");
        ImGui::SliderFloat("Delayed hit", &a.delaySeconds, 0, 1.5f, "+%.2f s");
        ImGui::SetItemTooltip("A delayed hit in a string holds its windup this much longer (x 0.8 .. 1.2): it catches an early roll.");
        ImGui::SliderFloat("Reaction from", &a.reactFrom, 0, 0.5f, "%.2f s");
        ImGui::SliderFloat("Reaction to", &a.reactTo, 0, 0.5f, "%.2f s");
        ImGui::SliderFloat("Timing error", &a.aimError, 0, 0.3f, "+-%.2f s");
        ImGui::SetItemTooltip("How far off the right moment its parries and dodges are, at most.");
        ImGui::SliderFloat("Dodge lead", &a.dodgeLead, 0, 0.4f, "%.2f s");
        ImGui::SliderFloat("Guard held after", &a.guardHold, 0, 1.5f, "%.2f s");
        ImGui::SliderFloat("Parried, you reel", &a.parriedSeconds, 0.2f, 2.5f, "%.2f s");
        ImGui::SliderFloat("Counter windup", &a.counterWindup, 0.05f, 1, "%.2f s");
        ImGui::SetItemTooltip("Its swing right after it blocked or parried you winds up this fast...");
        ImGui::SliderFloat("Counter window", &a.counterWindow, 0, 3, "%.2f s");
        ImGui::SetItemTooltip("...if it starts within this long.");
        ImGui::SliderFloat("Backing off", &a.backOffSeconds, 0, 3, "%.2f s");
        if (ImGui::Button("Reset AI")) a = AiTuning{};
        ImGui::TreePop();
    }
    for (size_t i = 0; i < entities.size(); ++i) {
        Entity& e = entities[i];
        const EntityType& t = e.type();
        char title[96];
        std::snprintf(title, sizeof title, "%s  (Lv %d)###entity%zu", e.displayName().c_str(), e.body.level, i);
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(int(i));
        attributes(e.body, false);
        ImGui::Text("Attackable %s   Wanders %s   Talks %s   Temper %s", t.attackable ? "yes" : "no",
                    t.wanders ? "yes" : "no", t.talks ? "yes" : "no", kTemperNames[int(t.temper)]);
        if (int st = int(e.state); ImGui::Combo("State", &st, kAiStateNames, int(AiState::Count)) && st != int(e.state))
            setState(e, AiState(st), "forced");
        ImGui::SetItemTooltip("Forces a state; the AI leaves it again at once if it doesn't fit (e.g. dead while alive).");
        ImGui::Text("for %.1f s   target you: %.1f away, %.1f from home%s", e.stateTime, glm::distance(e.body.pos, player.pos),
                    glm::distance(player.pos, e.home), e.calmTimer > 0 ? "   calming" : "");
        if (e.pack > 1) {
            float at = std::atan2(e.body.pos.x - player.pos.x, e.body.pos.z - player.pos.z);
            ImGui::Text("One of %d on you: its side %.0f deg, standing at %.0f deg", e.pack, double(glm::degrees(e.side)), double(glm::degrees(at)));
            ImGui::SetItemTooltip("Fighters on the player together spread evenly around it and circle to their side before closing in (ADR-034).");
        }
        if (t.temper != Temper::Harmless) {
            ImGui::Text("aggro %.0f   leash %.0f   re-aggro %.0f (after %.0f s)   chase %.1f b/s", t.aggroRange, t.leashRadius,
                        t.reaggroRadius, t.reaggroCooldown, t.chaseSpeed);
            ImGui::Text("windup %.2f s   recover %.2f s   cooldown %.2f s", t.windup, t.recover, t.cooldown);
            ImGui::Text("stance %.0f / %.0f   poise %.0f   %s %.2f s", double(e.body.stance), double(t.stance), double(t.poise),
                        e.body.broken ? "BROKEN" : "reeling", double(e.body.stagger));
            ImGui::SetItemTooltip("Hits wear the stance down; at 0 it breaks (open to a riposte). A hit of at least its poise makes it flinch (ADR-035).");
            if (t.parryable) ImGui::Text("parried %d / %d (to open it to a riposte)", e.parried, t.parries);
            else ImGui::TextDisabled("can't be parried");
            ImGui::Text("token %s   waited %.1f s   string %d left (of up to %d, %.0f%% delayed)%s", e.token ? "YES" : "no",
                        double(std::min(e.sinceSwing, 99.0f)), e.string, t.combo, double(t.delay * 100), e.backOff > 0 ? "   backing off" : "");
            ImGui::SetItemTooltip("Attack tokens and strings of hits (ADR-042): without a token it waits on the ring.");
            ImGui::Text("answers: guard %.0f%%  parry %.0f%%  dodge %.0f%%  backs off %.0f%%", double(t.guard * 100), double(t.parry * 100),
                        double(t.dodge * 100), double(t.backOff * 100));
            ImGui::Text("%s   answer %s   guard %s   stamina %.0f%s", e.threatened ? "THREATENED" : "safe", kAnswerNames[int(e.answer)],
                        e.body.guarding ? "UP" : "down", double(e.body.stamina), e.counterTimer > 0 ? "   counter ready" : "");
            ImGui::SetItemTooltip("Your attack can hit it: it answers with a guard, a parry or a dodge, by its kind's chances (ADR-043).");
            ImGui::Text("gives up after %.0f s stuck   flees below %.0f%% health", t.giveUpSeconds, t.fleeHealth * 100);
        }
        if (e.transitions && ImGui::TreeNode("transitions", "Last %u transitions", std::min(e.transitions, uint32_t(e.history.size())))) {
            for (uint32_t k = 0; k < std::min(e.transitions, uint32_t(e.history.size())); ++k) {  // newest first
                const AiTransition& h = e.history[(e.transitions - 1 - k) % e.history.size()];
                ImGui::Text("%s -> %s: %s (after %.1f s)", kAiStateNames[int(h.from)], kAiStateNames[int(h.to)], h.reason, h.after);
            }
            ImGui::TreePop();
        }
        if (t.lootChance > 0 || t.rareChance > 0)
            ImGui::Text("Drops %s %.0f%%%s%s%s", t.loot ? itemType(t.loot).name : "-", t.lootChance * 100,
                        t.rareLoot ? std::format(", else {} {:.0f}%", itemType(t.rareLoot).name, t.rareChance * 100).c_str() : "",
                        e.loot ? ", corpse holds " : "", e.loot ? itemType(e.loot).name : "");
        if (t.weapon) ImGui::Text("Wields %s", itemType(t.weapon).name);
        if (t.shield) ImGui::Text("Holds %s", itemType(t.shield).name);
        ImGui::Text("At %.1f %.1f %.1f, %.1f from you", e.body.pos.x, e.body.pos.y, e.body.pos.z,
                    glm::distance(e.body.pos, player.pos));
        if (t.wanders)
            ImGui::Text("%s %.1f s more, %.1f from home (wanders %.0f)", e.wanderDir == glm::vec2(0) ? "Standing" : "Walking",
                        e.wanderTimer, glm::distance(e.body.pos, e.home), e.wanderRadius);
        if (e.spawn.id) ImGui::Text("From spawn marker #%u %s (%zu lines)", e.spawn.id, e.spawn.name.c_str(), e.spawn.lines.size());
        if (t.attackable && ImGui::TreeNode("status", "Status effects (ADR-046)")) {
            statusPanel(e.body, [&](Status s) { hit(e, afflict(e.body, s, "the debug window"), s); });
            ImGui::TreePop();
        }
        if (t.attackable) {
            if (ImGui::Button("Hit for 10")) hit(e, 10);
            ImGui::SameLine();
            if (ImGui::Button("Kill")) hit(e, int(std::ceil(e.body.health)));
        }
        int line = pickLine(session, e);
        if ((t.talks || !e.spawn.lines.empty()) && ImGui::Button("Talk")) talk(e, line);  // no TalkedTo: quests don't move
        if (const char* s = e.speech()) ImGui::TextWrapped("Says: \"%s\"", s);
        if (line >= 0) ImGui::TextDisabled("Quest line %d of %s passes now", line, defs().questIds[defs().questLines[size_t(line)].quest].c_str());
        ImGui::PopID();
    }
}

static void itemsTab(Player& p) {
    Inventory& inv = p.inv;
    ImGui::Text("%d / %d item slots free (C: character, B: bags)", freeSlots(inv), itemSlots(inv));
    ImGui::BeginDisabled(freeSlots(inv) == 0);
    for (int id = 1; id < int(defs().items.size()); ++id) {
        ImGui::PushID(id);
        if (ImGui::Button("Give")) giveItem(inv, ItemId(id));
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextUnformatted(itemType(ItemId(id)).name);
    }
    if (ImGui::Button("Give Iron set"))
        for (ItemId id : {IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons}) giveItem(inv, id);
    ImGui::EndDisabled();
    ImGui::SeparatorText("Equipped");
    for (int s = 0; s < EquipCount; ++s)
        ImGui::Text("%-10s %s", kSlotNames[s], inv.equipped[size_t(s)] ? itemType(inv.equipped[size_t(s)]).name : "-");
    for (int b = 0; b < kBagBarSlots; ++b)
        ImGui::Text("Bag %d      %s", b + 1, inv.bags[size_t(b)] ? itemType(inv.bags[size_t(b)]).name : "-");
    if (ImGui::Button("Empty inventory")) inv = Inventory{};
    ImGui::SetItemTooltip("Removes every item, equipped ones and bags included.");
}

// Progression (ADR-027): grant XP, set the level and skills, respec, and the formulas' numbers now.
static void progressionTab(Player& p) {
    Progress& g = p.progress;
    ImGui::Text("Level %d, %d / %d XP, %d talent points free (%d spent)", p.level, g.xp, levelXpToNext(p.level), talentPoints(g, p.level),
                pointsSpent(g));
    if (ImGui::SliderInt("Level", &p.level, 1, kMaxLevel)) g.xp = 0;
    ImGui::SetItemTooltip("Also the attacker level of the player's hits in armor reduction. Lowering it below the talents spent leaves negative points.");
    for (int xp : {50, 250, 1000}) {
        ImGui::PushID(xp);
        if (ImGui::Button(std::format("+{} XP", xp).c_str())) addCharacterXp(g, p.level, xp);
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (ImGui::Button("Respec")) respec(g);
    ImGui::SetItemTooltip("Clears every talent: their points are free again.");
    ImGui::SeparatorText("Skills");
    for (size_t k = 0; k < size_t(Skill::Count); ++k) {
        const SkillDef& d = skillDef(Skill(k));
        SkillLevel& sk = g.skills[k];
        ImGui::PushID(int(k));
        if (ImGui::SliderInt(d.name.c_str(), &sk.level, kMinSkill, kMaxSkill)) sk.xp = 0;
        ImGui::SameLine();
        if (ImGui::Button("+100 XP")) addSkillXp(g, p.level, Skill(k), 100);
        ImGui::SetItemTooltip("As if used: levels it, and each level adds character XP.");
        ImGui::TextDisabled("%.0f / %.0f to %d; per use %.4g x base XP + %.4g; per level gained:", double(sk.xp),
                            double(skillXpToNext(d.improveMult, d.improveOffset, sk.level)), sk.level + 1, double(d.useMult), double(d.useOffset));
        for (const Modifier& m : d.perLevel)
            ImGui::SameLine(), ImGui::TextDisabled("%s %+g%s", kStatIds[size_t(m.stat)], double(m.value), m.kind == ModKind::Flat ? "" : " (fraction)");
        ImGui::PopID();
    }
    ImGui::SeparatorText("Formulas");
    const SkillDef &blade = skillDef(Skill::OneHanded), &fist = skillDef(Skill::Unarmed);
    ImGui::Text("XP per hit before wear: sword %.1f, punch %.1f (training dummy x %.2f)",
                double(skillUseXp(blade.useMult, blade.useOffset, (weaponAttack(Shortsword).minDamage + weaponAttack(Shortsword).maxDamage) / 2.0f)),
                double(skillUseXp(fist.useMult, fist.useOffset, (kPunch.minDamage + kPunch.maxDamage) / 2.0f)), double(kTrainingRate));
    ImGui::Text("Skill XP halves every %.0f hits on one NPC; one hit wears off per %.0f s", double(kXpWearHalving), double(kXpWearSeconds));
    ImGui::Text("Level %d -> %d: %d XP; level %d in all: %d XP", p.level, p.level + 1, levelXpToNext(p.level), p.level, levelXpTotal(p.level));
    ImGui::Text("Talent tiers: %d points spent in the tree per tier", kPointsPerTier);
}

// The UI runs between ticks, so the registry can be swapped right here.
static void dataTab(std::string& status, Player& player) {
    if (ImGui::Button("Reload defs")) {
        std::string error;
        status = reloadDefs(dataDir(), error) ? "Reloaded " + dataDir().string() : "Kept the old defs: " + error;
        if (error.empty() && !replayTalents(player.progress, player.level)) status += "; the talents no longer fit the rules: reset";
    }
    ImGui::SetItemTooltip("Reloads data/*.json. On an error, or if a loaded save id would change meaning, the old defs stay.");
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    const Defs& d = defs();
    ImGui::SeparatorText("Items (data/items.json)");
    if (ImGui::BeginTable("items", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const char* h : {"save", "id", "slot", "damage / speed", "bag / armor", "mods"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (size_t i = 1; i < d.items.size(); ++i) {
            const ItemType& t = d.items[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(), ImGui::Text("%zu", i);
            ImGui::TableNextColumn(), ImGui::TextUnformatted(d.itemIds[i].c_str());
            ImGui::SetItemTooltip("%s", t.name);
            ImGui::TableNextColumn(), ImGui::TextUnformatted(t.slot < EquipCount ? kSlotNames[t.slot] : t.slot == SlotBag ? "Bag" : "-");
            ImGui::TableNextColumn();
            if (t.attack.speed > 0) ImGui::Text("%d-%d / %.2f %s", t.attack.minDamage, t.attack.maxDamage, t.attack.speed,
                                                t.slot == SlotMainHand ? kWeaponClassIds[size_t(t.weaponClass)] : "");
            ImGui::TableNextColumn();
            if (t.bagSlots || t.armor) ImGui::Text("%d / %d", t.bagSlots, t.armor);
            ImGui::TableNextColumn();
            for (const Modifier& m : t.mods)
                ImGui::Text("%s %+g%s", kStatIds[size_t(m.stat)], double(m.kind == ModKind::Flat ? m.value : m.value * 100),
                            m.kind == ModKind::Flat ? "" : m.kind == ModKind::Increased ? "% increased" : "% more");
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Creatures (data/creatures.json)");
    if (ImGui::BeginTable("creatures", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const char* h : {"save", "id", "lvl / hp", "temper", "weapon", "drops", "xp"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (size_t k = 0; k < d.creatures.size(); ++k) {
            const EntityType& t = d.creatures[k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(), ImGui::Text("%zu", k);
            ImGui::TableNextColumn(), ImGui::TextUnformatted(d.creatureIds[k].c_str());
            ImGui::SetItemTooltip("%s", t.name ? t.name : "(a random villager name)");
            ImGui::TableNextColumn(), ImGui::Text("%d / %d", t.level, maxHealth(compute(t.stats, itemType(t.weapon).mods)));
            ImGui::TableNextColumn(), ImGui::TextUnformatted(kTemperNames[int(t.temper)]);
            ImGui::TableNextColumn(), ImGui::TextUnformatted(t.weapon ? itemType(t.weapon).name : "-");
            ImGui::TableNextColumn();
            if (t.loot) ImGui::Text("%s %.0f%%", itemType(t.loot).name, t.lootChance * 100);
            if (t.rareLoot) ImGui::Text("else %s %.0f%%", itemType(t.rareLoot).name, t.rareChance * 100);  // ADR-049
            ImGui::TableNextColumn(), ImGui::Text("%d", t.xp);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Talents (data/talents.json)");
    for (const TalentDef& t : d.talents)
        ImGui::BulletText("%s (%s tier %d, %d ranks%s%s)", t.id.c_str(), kSkillIds[size_t(t.tree)], t.tier, t.maxRank,
                          t.prerequisite >= 0 ? ", needs " : "", t.prerequisite >= 0 ? d.talents[size_t(t.prerequisite)].id.c_str() : "");
}

// Quests (ADR-026): every quest's stage and objectives (set or reset them), the flags, warnings
// about the data and the world's markers, and the latest events the quests saw. Runs between ticks.
static void questsTab(Session& s) {
    const Defs& d = defs();
    std::vector<std::string> warnings = d.warnings, markers = checkQuestMarkers(d, s.markers);
    warnings.insert(warnings.end(), markers.begin(), markers.end());
    for (const std::string& w : warnings) ImGui::TextColored({1, 0.7f, 0.3f, 1}, "%s", w.c_str());
    if (d.quests.empty()) ImGui::TextDisabled("No quests in %s", (dataDir() / "quests").string().c_str());
    for (size_t q = 0; q < std::min(d.quests.size(), s.quests.size()); ++q) {
        const QuestDef& def = d.quests[q];
        QuestState& st = s.quests[q];
        char title[128];
        std::snprintf(title, sizeof title, "%s (%s): stage %d%s###quest%zu", def.title.c_str(), def.id.c_str(), st.stage,
                      st.done ? ", done" : st.failed ? ", failed" : st.stage ? "" : ", not started", q);
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(int(q));
        char stage[16];
        std::snprintf(stage, sizeof stage, "%d", st.stage);
        if (ImGui::BeginCombo("Set stage", stage)) {
            for (const Stage& option : def.stages)
                if (std::snprintf(stage, sizeof stage, "%d", option.id); ImGui::Selectable(stage, option.id == st.stage))
                    setQuestStage(s, q, option.id);
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Enters the stage: runs its actions (objectives, items, flags), as the game would.\nNot on a done or failed quest: clear those first.");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) resetQuest(s, q);
        ImGui::Checkbox("Done", &st.done);
        ImGui::SameLine();
        ImGui::Checkbox("Failed", &st.failed);
        if (const Stage* now = findStage(def, st.stage); now && !now->journal.empty()) ImGui::TextWrapped("%s", now->journal.c_str());
        for (size_t o = 0; o < std::min(def.objectives.size(), st.objectives.size()); ++o) {
            ImGui::PushID(int(o));
            int state = int(st.objectives[o]);
            ImGui::SetNextItemWidth(110);
            if (ImGui::Combo("##state", &state, kObjectiveStateNames, int(ObjectiveState::Count))) st.objectives[o] = ObjectiveState(state);
            ImGui::SameLine();
            ImGui::Text("%s: %s", def.objectives[o].id.c_str(), def.objectives[o].text.c_str());
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::SeparatorText("Flags");
    for (auto it = s.flags.begin(); it != s.flags.end();) {
        ImGui::PushID(it->first.c_str());
        bool remove = ImGui::SmallButton("x");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt(it->first.c_str(), &it->second);
        ImGui::PopID();
        it = remove ? s.flags.erase(it) : std::next(it);
    }
    static char name[49] = "";
    static int value = 1;
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##flag", name, sizeof name);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::InputInt("##value", &value);
    ImGui::SameLine();
    ImGui::BeginDisabled(!*name || !validName(name) || s.flags.size() >= kMaxFlags);  // the save's bounds
    if (ImGui::Button("Set flag")) s.flags[name] = value;
    ImGui::EndDisabled();
    ImGui::SeparatorText("Events (newest first)");
    if (s.eventLog.empty()) ImGui::TextDisabled("None yet");
    for (auto it = s.eventLog.rbegin(); it != s.eventLog.rend(); ++it) ImGui::TextUnformatted(it->c_str());
}

static void cameraTab(GameSettings& g, SDL_Gamepad* pad, const LockOn* lock, const Session& session) {
    ImGui::SliderFloat("Field of view", &g.fovDegrees, 30, 120, "%.0f deg");
    ImGui::SliderFloat("Distance", &g.cameraDistance, 2, 80, "%.1f blocks");
    ImGui::SetItemTooltip("Also the mouse wheel. Pull far back for an overview.");
    ImGui::Checkbox("Terrain collision", &g.cameraCollision);
    ImGui::SliderFloat("Camera shake", &g.cameraShake, 0, 2, "x %.2f");
    ImGui::SetItemTooltip("Big hits shake the camera (ADR-045); 0 turns it off.");
    ImGui::Checkbox("HUD", &g.hud);
    ImGui::SetItemTooltip("Nameplates, speech balloons and the health bar.");
    ImGui::SliderFloat("Mouse sensitivity", &g.mouseSensitivity, 0.001f, 0.02f, "%.4f");
    ImGui::SeparatorText("Lock-on (R3 / Tab)");
    if (const Entity* t = lock ? lockedEntity(*lock, session.entities) : nullptr)
        ImGui::Text("On %s #%u, %.1f blocks away, unseen %.1f s", t->displayName().c_str(), t->id,
                    double(glm::distance(t->body.pos, session.player.pos)), double(lock->unseen));
    else
        ImGui::TextDisabled("Not locked on");
    ImGui::SliderFloat("Lock range", &g.lock.acquireRange, 5, 60, "%.0f blocks");
    ImGui::SetItemTooltip("How far a new lock reaches. Picks the enemy nearest the middle of the view, in sight.");
    ImGui::SliderFloat("Break range", &g.lock.breakRange, 5, 80, "%.0f blocks");
    ImGui::SetItemTooltip("A lock lets go past this.");
    ImGui::SliderFloat("Lost sight after", &g.lock.unseenSeconds, 0.1f, 10, "%.1f s");
    ImGui::Checkbox("Next target on a kill", &g.lock.autoSwitch);
    ImGui::SliderFloat("Mouse flick", &g.lockFlickPixels, 10, 300, "%.0f px");
    ImGui::SetItemTooltip("Mouse travel (quickly, while locked) that switches target. The D-pad, arrow keys and a flick of the right stick switch too.");
    ImGui::SeparatorText("Controller");
    if (!pad) ImGui::TextDisabled("None connected");
    else {
        auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0f; };
        ImGui::Text("%s", SDL_GetGamepadName(pad));
        ImGui::Text("Left stick %+.2f %+.2f, right stick %+.2f %+.2f, triggers %.2f %.2f", axis(SDL_GAMEPAD_AXIS_LEFTX),
                    axis(SDL_GAMEPAD_AXIS_LEFTY), axis(SDL_GAMEPAD_AXIS_RIGHTX), axis(SDL_GAMEPAD_AXIS_RIGHTY),
                    axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    }
    ImGui::SliderFloat("Stick look speed", &g.stickLookSpeed, 0.5f, 8, "%.1f rad/s");
    ImGui::SliderFloat("Stick deadzone", &g.stickDeadzone, 0, 0.5f, "%.2f");
}

static void streamSection(GameSettings& g, const Stream& stream, const Grid& world, glm::vec3 player) {
    ImGui::SeparatorText("Streaming (the endless world)");
    Stream::Stats st = stream.stats();
    ImGui::Text("Window %d..%d x %d..%d (%d x %d columns of %d)", world.lo.x, world.hi().x, world.lo.z, world.hi().z,
                world.size.x / kChunk, world.size.z / kChunk, kChunk);
    ImGui::Text("Columns: %d queued, %d generated, %d lit, %d meshed", st.queued, st.generated, st.lit, st.meshed);
    ImGui::Text("Last frame: %d in (%.2f ms), %d lit (%.2f ms); %d dropped by the last move", st.installed, st.installMs,
                st.litNow, st.lightMs, st.dropped);
    ImGui::Text("Generating a column: %.1f ms (a worker's average)", st.generateMs);
    ImGui::SliderFloat("Light ms per frame", &g.streamLightMs, 0.5f, 16, "%.1f");
    ImGui::SetItemTooltip("Time a frame spends lighting chunk columns (at least one; they're meshed next): more fills in faster, but costs frame time.");
    const char* stages[] = {"queued", "generated", "lit", "meshed"};
    ImGui::Text("Player's column: %s", stages[int(stream.stage(glm::ivec3(glm::floor(player))))]);
}

static void worldTab(GameSettings& g, Session& sim, const RenderStats& s, DebugActions& actions, const Stream* stream) {
    if (stream) streamSection(g, *stream, sim.world, sim.player.pos);
    int minutes = int(sim.timeOfDay * 24 * 60);
    ImGui::SliderFloat("Time of day", &sim.timeOfDay, 0, 0.9999f, "%.3f");
    ImGui::SameLine();
    ImGui::Text("%02d:%02d", minutes / 60, minutes % 60);
    ImGui::SliderFloat("Time speed", &sim.timeScale, 0, 200, sim.timeScale == 0 ? "paused" : "%.1fx",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::Text("Simulated %.1f s, seed %u (spawns, damage rolls)", sim.elapsed, sim.seed);

    ImGui::SeparatorText("Timed spawns");  // spawn markers with an active window (Marker::activeFrom)
    bool anyTimed = false;
    for (const Marker& m : sim.markers) {
        if (m.activeFrom == m.activeTo) continue;
        anyTimed = true;
        bool active = activeAt(m, sim.timeOfDay);
        int from = int(m.activeFrom * 24 * 60), to = int(m.activeTo * 24 * 60);
        auto out = std::ranges::count(sim.entities, m.id, [](const Entity& e) { return e.spawn.id; });
        ImGui::TextColored(active ? ImVec4(0.4f, 1, 0.5f, 1) : ImVec4(0.6f, 0.6f, 0.65f, 1), "#%u %s: %s %02d:%02d-%02d:%02d, %d/%d out",
                           m.id, m.name.empty() ? defs().creatureIds[size_t(m.entity)].c_str() : m.name.c_str(), active ? "active" : "inactive",
                           from / 60, from % 60, to / 60, to % 60, int(out), m.count);
    }
    if (!anyTimed) ImGui::TextDisabled("None: every spawn marker is always active (the editor's Marker tool sets windows).");

    ImGui::SeparatorText("Generation");
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &g.seed);
    if (ImGui::Button("Regenerate world")) actions.regenerate = true;
    ImGui::SetItemTooltip("Takes about a second.");
    ImGui::Text("Size %d x %d x %d", kWorldSize.x, kWorldSize.y, kWorldSize.z);
    ImGui::Text("Generated in %.3f s, meshed + uploaded in %.3f s", s.genSeconds, s.meshSeconds);
    ImGui::Text("%zu opaque + %zu water quads (%.1f MB) in %d chunks", s.opaqueQuads, s.waterQuads,
                double(s.opaqueQuads + s.waterQuads) * 8 / (1 << 20), s.chunksTotal);
}

static void saveTab(const std::string& path, const std::string& status, const std::vector<std::string>& notes, const char* autosave,
                    bool locked, DebugActions& actions) {
    ImGui::TextWrapped("Player save: %s", path.c_str());
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        char when[32];
        std::tm tm;
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime_r(&st.st_mtime, &tm));
        ImGui::Text("Last saved %s, %lld bytes", when, (long long)st.st_size);
    } else ImGui::TextUnformatted("No save yet");
    if (ImGui::Button(locked ? "Save over the unreadable save" : "Save (F5)")) actions.saveGame = true;
    if (locked) ImGui::SetItemTooltip("F5 and quitting won't write over a save that didn't load; this does (it becomes the .bak).");
    ImGui::SameLine();
    if (ImGui::Button("Load (F9)")) actions.loadGame = true;
    ImGui::SameLine();
    if (ImGui::Button("Delete")) actions.deleteSave = true;
    ImGui::SetItemTooltip("Removes the save and its .bak. Quitting doesn't save again until the next F5, so the next start is a new game.");
    ImGui::TextWrapped("Autosave: %s", autosave);
    ImGui::TextUnformatted("--new-game skips loading on start. The editor's saves write only the world file.");
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    if (notes.empty()) return;
    ImGui::SeparatorText("Last load");
    for (const std::string& note : notes) ImGui::BulletText("%s", note.c_str());
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

void DebugUI::window(GameSettings& game, RenderSettings& render, Session& session, const RenderStats& stats,
                     DebugActions& actions) {
    Player& player = session.player;
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({440, 470}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Debug   ( . to toggle )", &open) && ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Render")) { renderTab(render, stats); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Player")) { playerTab(session, session.armorK, actions, region); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Progression")) { progressionTab(player); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Items")) { itemsTab(player); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Entities")) { entitiesTab(session, actions, spawnKind); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Quests")) { questsTab(session); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Data")) { dataTab(defsStatus_, player); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Camera")) { cameraTab(game, gamepad, lock, session); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("World")) { worldTab(game, session, stats, actions, stream); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Save")) { saveTab(savePath, saveStatus, saveNotes, autosave, saveLocked, actions); ImGui::EndTabItem(); }
        if (audio && ImGui::BeginTabItem("Audio")) { audioTab(*audio, ambience); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Stats")) {
            statsTab(frameMs_, frameIndex_);
            ImGui::Text("Sim ticks this frame %d (60 Hz), drawn %.2f into the next", ticks, tickAlpha);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

NoteAction noteWindow(char* text, size_t size, bool focus) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({520, 0}, ImGuiCond_Always);
    NoteAction action = NoteAction::None;
    if (ImGui::Begin("Debug note (game paused)", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("What happened? The game's state and a screenshot from when this opened go with the note "
                           "(debug_notes/notes.jsonl). Enter saves, Shift+Enter starts a new line, Esc cancels.");
        if (focus) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputTextMultiline("##note", text, size, {-1, 6 * ImGui::GetTextLineHeightWithSpacing()},
                                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        bool empty = !*text;
        ImGui::BeginDisabled(empty);
        if ((ImGui::Button("Save") || enter) && !empty) action = NoteAction::Save;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) action = NoteAction::Cancel;
    }
    ImGui::End();
    return action;
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

ImDrawData* DebugUI::end(GameSettings& game, RenderSettings& render, Session& session, const RenderStats& stats,
                         DebugActions& actions) {
    if (open) window(game, render, session, stats, actions);
    ImGui::Render();
    return ImGui::GetDrawData();
}
