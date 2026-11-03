#include "debug_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include <sys/stat.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include "defs.h"

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

    ImGui::SeparatorText("This frame");
    ImGui::Text("Chunks drawn   %d / %d", s.chunksDrawn, s.chunksTotal);
    ImGui::Text("Quads drawn    %zu / %zu", s.quadsDrawn, s.opaqueQuads + s.waterQuads);
    ImGui::Text("Draw calls     %d", s.drawCalls);
}

// Health, the five attributes and what derives from them; shared by the Player and Entities tabs.
static void attributes(Player& b, bool fights) {
    Stats& s = b.stats;
    ImGui::SliderFloat("Health", &b.health, 0, float(maxHealth(sheet(b))), "%.0f");
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
    StatSheet total = sheet(b);
    ImGui::Text("Max health %d, regen %.1f/s out of combat", maxHealth(total), healthRegen(total));
    if (fights) {
        const Attack& a = mainAttack(b.inv);
        float swing = swingTime(total, a);
        int lo = hitBeforeArmor(total, a, a.minDamage), hi = hitBeforeArmor(total, a, a.maxDamage);
        ItemId weapon = b.inv.equipped[SlotMainHand];
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
        ImGui::BulletText("%s: %s %s %+g", m.source, kStatIds[size_t(m.stat)], kKinds[size_t(m.kind)], double(m.value));
    ImGui::TextDisabled("final = (base + flat) x (1 + increased) x (1 + more)...");
}

static void playerTab(Player& p, float& armorK, DebugActions& actions, const char* region) {
    attributes(p, true);
    ImGui::SliderInt("Level", &p.level, 1, 60);
    ImGui::SetItemTooltip("The attacker level of the player's hits in armor reduction (1 until progression).");
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
    if (ImGui::Button("Respawn NPCs at their markers")) actions.respawnEntities = true;
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
        if (t.temper != Temper::Harmless) {
            ImGui::Text("aggro %.0f   leash %.0f   re-aggro %.0f (after %.0f s)   chase %.1f b/s", t.aggroRange, t.leashRadius,
                        t.reaggroRadius, t.reaggroCooldown, t.chaseSpeed);
            ImGui::Text("windup %.2f s   recover %.2f s   cooldown %.2f s", t.windup, t.recover, t.cooldown);
            ImGui::Text("gives up after %.0f s stuck   flees below %.0f%% health", t.giveUpSeconds, t.fleeHealth * 100);
        }
        if (e.transitions && ImGui::TreeNode("transitions", "Last %u transitions", std::min(e.transitions, uint32_t(e.history.size())))) {
            for (uint32_t k = 0; k < std::min(e.transitions, uint32_t(e.history.size())); ++k) {  // newest first
                const AiTransition& h = e.history[(e.transitions - 1 - k) % e.history.size()];
                ImGui::Text("%s -> %s: %s (after %.1f s)", kAiStateNames[int(h.from)], kAiStateNames[int(h.to)], h.reason, h.after);
            }
            ImGui::TreePop();
        }
        if (t.lootChance > 0)
            ImGui::Text("Drops %s %.0f%%%s%s", itemType(t.loot).name, t.lootChance * 100, e.loot ? ", corpse holds " : "",
                        e.loot ? itemType(e.loot).name : "");
        if (t.weapon) ImGui::Text("Wields %s", itemType(t.weapon).name);
        ImGui::Text("At %.1f %.1f %.1f, %.1f from you", e.body.pos.x, e.body.pos.y, e.body.pos.z,
                    glm::distance(e.body.pos, player.pos));
        if (t.wanders)
            ImGui::Text("%s %.1f s more, %.1f from home (wanders %.0f)", e.wanderDir == glm::vec2(0) ? "Standing" : "Walking",
                        e.wanderTimer, glm::distance(e.body.pos, e.home), e.wanderRadius);
        if (e.spawn.id) ImGui::Text("From spawn marker #%u %s (%zu lines)", e.spawn.id, e.spawn.name.c_str(), e.spawn.lines.size());
        if (t.attackable) {
            if (ImGui::Button("Hit for 10")) damage(e, 10);
            ImGui::SameLine();
            if (ImGui::Button("Kill")) damage(e, int(std::ceil(e.body.health)));
        }
        if ((t.talks || !e.spawn.lines.empty()) && ImGui::Button("Talk")) talk(e);
        if (const char* s = e.speech()) ImGui::TextWrapped("Says: \"%s\"", s);
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

// The UI runs between ticks, so the registry can be swapped right here.
static void dataTab(std::string& status) {
    if (ImGui::Button("Reload defs")) {
        std::string error;
        status = reloadDefs(dataDir(), error) ? "Reloaded " + dataDir().string() : "Kept the old defs: " + error;
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
            if (t.attack.speed > 0) ImGui::Text("%d-%d / %.2f", t.attack.minDamage, t.attack.maxDamage, t.attack.speed);
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
    if (ImGui::BeginTable("creatures", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (const char* h : {"save", "id", "lvl / hp", "temper", "weapon", "drops"}) ImGui::TableSetupColumn(h);
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
        }
        ImGui::EndTable();
    }
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

static void worldTab(GameSettings& g, Session& sim, const RenderStats& s, DebugActions& actions) {
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
        if (ImGui::BeginTabItem("Player")) { playerTab(player, session.armorK, actions, region); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Items")) { itemsTab(player); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Entities")) { entitiesTab(session.entities, player, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Data")) { dataTab(defsStatus_); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Camera")) { cameraTab(game); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("World")) { worldTab(game, session, stats, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Save")) { saveTab(savePath, saveStatus, saveNotes, autosave, saveLocked, actions); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Stats")) {
            statsTab(frameMs_, frameIndex_);
            ImGui::Text("Sim ticks this frame %d (60 Hz), drawn %.2f into the next", ticks, tickAlpha);
            ImGui::EndTabItem();
        }
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

ImDrawData* DebugUI::end(GameSettings& game, RenderSettings& render, Session& session, const RenderStats& stats,
                         DebugActions& actions) {
    if (open) window(game, render, session, stats, actions);
    ImGui::Render();
    return ImGui::GetDrawData();
}
