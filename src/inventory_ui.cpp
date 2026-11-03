#include "inventory_ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>

#include <imgui.h>

#include "defs.h"
#include "hud.h"
#include "session.h"

namespace {

constexpr float kSlot = 46;  // pixels
constexpr int kColumns = 6;  // item slots per row in the bags window

// Item icons are drawn with lines and shapes, colored like their voxels.
void icon(ImDrawList* dl, ItemId id, ImVec2 lo, float s) {
    auto at = [&](float x, float y) { return ImVec2(lo.x + x * s, lo.y + y * s); };
    ImU32 steel = uiColor(0.78f, 0.80f, 0.84f), iron = uiColor(0.30f, 0.30f, 0.33f), leather = uiColor(0.35f, 0.22f, 0.12f);
    ImU32 plate = uiColor(0.50f, 0.52f, 0.56f);
    switch (id) {
    case Shortsword:
        dl->AddLine(at(0.32f, 0.68f), at(0.86f, 0.14f), steel, s * 0.12f);   // blade
        dl->AddLine(at(0.18f, 0.54f), at(0.46f, 0.82f), iron, s * 0.08f);    // crossguard
        dl->AddLine(at(0.32f, 0.68f), at(0.16f, 0.84f), leather, s * 0.08f); // grip
        dl->AddCircleFilled(at(0.14f, 0.86f), s * 0.06f, iron);               // pommel
        break;
    case Satchel:
        dl->AddBezierCubic(at(0.32f, 0.38f), at(0.32f, 0.08f), at(0.68f, 0.08f), at(0.68f, 0.38f), leather, s * 0.06f);
        dl->AddRectFilled(at(0.16f, 0.36f), at(0.84f, 0.86f), uiColor(0.76f, 0.66f, 0.48f), s * 0.1f);
        dl->AddRectFilled(at(0.16f, 0.36f), at(0.84f, 0.58f), uiColor(0.62f, 0.52f, 0.36f), s * 0.1f,
                          ImDrawFlags_RoundCornersTop);  // flap
        dl->AddRectFilled(at(0.45f, 0.52f), at(0.55f, 0.64f), iron);  // buckle
        break;
    case Knife:
        dl->AddLine(at(0.40f, 0.60f), at(0.74f, 0.26f), steel, s * 0.09f);   // blade
        dl->AddLine(at(0.30f, 0.52f), at(0.48f, 0.70f), iron, s * 0.07f);    // guard
        dl->AddLine(at(0.40f, 0.60f), at(0.24f, 0.76f), leather, s * 0.08f); // grip
        dl->AddCircleFilled(at(0.22f, 0.78f), s * 0.05f, iron);
        break;
    case BoarSkin: {  // a stretched hide: body with four leg flaps, darker ridge down the middle
        ImU32 hide = uiColor(0.36f, 0.25f, 0.18f), ridge = uiColor(0.25f, 0.17f, 0.12f);
        for (auto [x, y] : {std::pair{0.22f, 0.24f}, std::pair{0.78f, 0.24f}, std::pair{0.22f, 0.76f}, std::pair{0.78f, 0.76f}})
            dl->AddCircleFilled(at(x, y), s * 0.1f, hide);
        dl->AddRectFilled(at(0.26f, 0.18f), at(0.74f, 0.82f), hide, s * 0.14f);
        dl->AddLine(at(0.5f, 0.2f), at(0.5f, 0.8f), ridge, s * 0.07f);
        break;
    }
    // Iron armor: plates (the Plate block's color), steel trim.
    case IronHelm:
        dl->AddRectFilled(at(0.2f, 0.2f), at(0.8f, 0.84f), plate, s * 0.26f, ImDrawFlags_RoundCornersTop);
        dl->AddRectFilled(at(0.34f, 0.48f), at(0.66f, 0.74f), uiColor(0.93f, 0.74f, 0.58f));  // the face
        dl->AddLine(at(0.22f, 0.44f), at(0.78f, 0.44f), steel, s * 0.06f);                   // brow
        dl->AddLine(at(0.5f, 0.14f), at(0.5f, 0.42f), steel, s * 0.08f);                     // crest
        break;
    case IronCuirass:
        dl->AddRectFilled(at(0.26f, 0.22f), at(0.74f, 0.86f), plate, s * 0.08f);
        dl->AddRectFilled(at(0.12f, 0.2f), at(0.4f, 0.36f), steel, s * 0.06f);  // shoulders
        dl->AddRectFilled(at(0.6f, 0.2f), at(0.88f, 0.36f), steel, s * 0.06f);
        dl->AddLine(at(0.5f, 0.36f), at(0.5f, 0.8f), steel, s * 0.06f);         // ridge
        break;
    case IronGauntlets:
        for (float x : {0.12f, 0.54f}) {
            dl->AddRectFilled(at(x, 0.5f), at(x + 0.34f, 0.84f), plate, s * 0.04f);     // cuff
            dl->AddRectFilled(at(x, 0.18f), at(x + 0.34f, 0.48f), plate, s * 0.08f);    // fist
            dl->AddLine(at(x + 0.03f, 0.24f), at(x + 0.31f, 0.24f), steel, s * 0.07f); // knuckles
        }
        break;
    case IronGreaves:
        for (float x : {0.2f, 0.56f}) {
            dl->AddRectFilled(at(x, 0.14f), at(x + 0.24f, 0.88f), plate, s * 0.06f);
            dl->AddCircleFilled(at(x + 0.12f, 0.42f), s * 0.08f, steel);  // knee
        }
        break;
    case IronSabatons:
        for (float x : {0.1f, 0.54f}) {
            dl->AddRectFilled(at(x, 0.3f), at(x + 0.18f, 0.78f), plate, s * 0.04f);         // shin
            dl->AddRectFilled(at(x, 0.62f), at(x + 0.34f, 0.8f), plate, s * 0.04f);         // foot
            dl->AddRectFilled(at(x + 0.26f, 0.62f), at(x + 0.36f, 0.8f), steel, s * 0.04f); // toe cap
        }
        break;
    case WolfPelt: {  // a grey hide with a brown back stripe and a tail
        ImU32 fur = uiColor(0.52f, 0.50f, 0.47f), back = uiColor(0.36f, 0.25f, 0.18f);
        for (auto [x, y] : {std::pair{0.24f, 0.22f}, std::pair{0.76f, 0.22f}, std::pair{0.24f, 0.7f}, std::pair{0.76f, 0.7f}})
            dl->AddCircleFilled(at(x, y), s * 0.09f, fur);
        dl->AddRectFilled(at(0.28f, 0.14f), at(0.72f, 0.76f), fur, s * 0.16f);
        dl->AddLine(at(0.5f, 0.76f), at(0.62f, 0.92f), fur, s * 0.1f);  // tail
        dl->AddLine(at(0.5f, 0.18f), at(0.5f, 0.72f), back, s * 0.1f);
        break;
    }
    case BoneCharm: {  // a bone on a cord
        ImU32 bone = uiColor(0.92f, 0.88f, 0.76f);
        dl->AddBezierCubic(at(0.2f, 0.16f), at(0.2f, 0.5f), at(0.8f, 0.5f), at(0.8f, 0.16f), leather, s * 0.05f);
        dl->AddLine(at(0.5f, 0.42f), at(0.5f, 0.76f), bone, s * 0.1f);
        for (float x : {0.43f, 0.57f}) {
            dl->AddCircleFilled(at(x, 0.42f), s * 0.07f, bone);
            dl->AddCircleFilled(at(x, 0.8f), s * 0.07f, bone);
        }
        break;
    }
    default: break;
    }
}

void tooltip(ItemId id, SlotRef where) {
    const ItemType& t = itemType(id);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(t.name);
    if (t.slot == SlotBag) ImGui::Text("%d slot bag", t.bagSlots);
    else if (t.slot < EquipCount) ImGui::TextDisabled("%s", kSlotNames[t.slot]);
    if (t.armor) ImGui::Text("%d Armor", t.armor);
    ImGui::PushStyleColor(ImGuiCol_Text, uiColor(0.4f, 0.9f, 0.4f));
    for (const Modifier& m : t.mods) {
        const char* stat = kStatNames[size_t(m.stat)];
        if (m.kind == ModKind::Flat) ImGui::Text("%+g %s", double(m.value), stat);
        else ImGui::Text("%+.0f%% %s%s", double(m.value) * 100, m.kind == ModKind::More ? "more " : "", stat);
    }
    ImGui::PopStyleColor();
    const Attack& a = t.attack;
    if (a.speed > 0) {
        ImGui::Text("%d - %d damage    Speed %.2f", a.minDamage, a.maxDamage, a.speed);
        ImGui::Text("(%.1f damage per second)", float(a.minDamage + a.maxDamage) / 2 / a.speed);
    }
    if (t.slot != SlotNone) ImGui::TextDisabled(where.kind == SlotRef::Item ? "Right-click to equip" : "Right-click to unequip");
    ImGui::TextDisabled("Ctrl+right-click to destroy");
    ImGui::EndTooltip();
}

// One square slot: drag its item onto another slot to move or swap it, right-click to equip or
// unequip, Ctrl+right-click (or Cmd) to destroy it. Empty slots show `label` (if any) in faint text.
void slot(Inventory& inv, SlotRef ref, const char* label) {
    ImGui::PushID(int(ref.kind) * 1000 + ref.container * 100 + ref.index);
    ImGui::InvisibleButton("slot", {kSlot, kSlot});  // left button only: right-click never starts a drag
    ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, uiColor(0.08f, 0.08f, 0.09f, 0.9f), 4);
    dl->AddRect(lo, hi, hovered ? uiColor(0.95f, 0.8f, 0.45f) : uiColor(0.32f, 0.30f, 0.28f), 4, 0, hovered ? 2.0f : 1.0f);
    ItemId item = *slotAt(inv, ref);
    if (item != NoItem) {
        icon(dl, item, lo, kSlot);
    } else if (label) {
        ImFont* font = ImGui::GetFont();
        float size = ImGui::GetFontSize() * 0.7f;
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, kSlot - 6, label);
        dl->AddText(font, size, {lo.x + (kSlot - ts.x) / 2, lo.y + (kSlot - ts.y) / 2}, uiColor(0.45f, 0.43f, 0.40f),
                    label, nullptr, kSlot - 6);
    }

    if (item != NoItem && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("item", &ref, sizeof ref);
        ImGui::Dummy({kSlot, kSlot});
        icon(ImGui::GetWindowDrawList(), item, ImGui::GetItemRectMin(), kSlot);
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("item"))
            moveItem(inv, *static_cast<const SlotRef*>(p->Data), ref);
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl || io.KeySuper) discardItem(inv, ref);  // either, whichever way the backend maps them on macOS
        else quickMove(inv, ref);
    }
    else if (item != NoItem && hovered && !ImGui::GetDragDropPayload()) tooltip(item, ref);
    ImGui::PopID();
}

void statRow(const char* name, const char* tip, const char* fmt, auto... args) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    ImGui::TableNextColumn();
    ImGui::Text(fmt, args...);
    if (tip) ImGui::SetItemTooltip("%s", tip);
}

// A stat's final value, then in green (or red) what gear, skills and talents add to (or take from) its base.
void gearRow(const char* name, const char* tip, const char* fmt, const StatSheet& s, const StatSheet& base, Stat stat) {
    statRow(name, tip, fmt, double(s[stat]));
    if (float gear = s[stat] - base[stat]; std::abs(gear) > 0.01f) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, gear > 0 ? uiColor(0.4f, 0.9f, 0.4f) : uiColor(0.95f, 0.4f, 0.35f));
        ImGui::Text("(%+g)", double(gear));
        ImGui::PopStyleColor();
    }
}

void attributes(const Player& p, float armorK) {
    StatSheet s = sheet(p), base = compute(p.stats);
    const Attack& a = mainAttack(p.inv);
    StatSheet w = sheet(p, skillOf(p.inv.equipped[SlotMainHand]));  // with the weapon skill's conditional modifiers
    float swing = swingTime(w, a);
    int lo = hitBeforeArmor(w, a, a.minDamage), hi = hitBeforeArmor(w, a, a.maxDamage);
    char tip[96];
    if (!ImGui::BeginTable("attributes", 2, ImGuiTableFlags_SizingFixedFit)) return;
    statRow("Health", nullptr, "%d / %d", int(std::ceil(p.health)), maxHealth(s));
    std::snprintf(tip, sizeof tip, "+%d max health per point", kHealthPerStamina);
    gearRow("Stamina", tip, "%.0f", s, base, Stat::Stamina);
    gearRow("Strength", "Adds strength x weapon speed to every hit", "%.0f", s, base, Stat::Strength);
    std::snprintf(tip, sizeof tip, "+%.0f%% attack speed per point", kHastePerAgility * 100);
    gearRow("Agility", tip, "%.0f", s, base, Stat::Agility);
    gearRow("Intellect", "No use yet", "%.0f", s, base, Stat::Intellect);
    std::snprintf(tip, sizeof tip, "+%.1f health per second out of combat per point", kRegenPerSpirit);
    gearRow("Spirit", tip, "%.0f", s, base, Stat::Spirit);
    ImGui::TableNextRow();
    ImGui::TableNextRow();
    ItemId weapon = p.inv.equipped[SlotMainHand];
    statRow("Damage", weapon ? itemType(weapon).name : "Unarmed", "%d - %d", lo, hi);
    statRow("Attack speed", "Seconds per swing", "%.2f s", swing);
    statRow("Damage/sec", nullptr, "%.1f", float(lo + hi) / 2 / swing);
    gearRow("Regen", "Health per second, out of combat", "%.1f /s", s, base, Stat::Regen);
    statRow("Armor", "From gear, skills and talents", "%.0f", double(s[Stat::Armor]));
    float taken = (1 - armorReduction(s[Stat::Armor], 1, armorK)) * s[Stat::DamageTaken];
    statRow("Damage taken", "Against a level 1 enemy, after armor and damage taken modifiers. Higher levels get through more.",
            "%+.1f%%", double(taken - 1) * 100);
    ImGui::EndTable();
}

}  // namespace

void characterWindow(bool& open, Player& p, float armorK) {
    if (!open) return;
    ImGui::SetNextWindowPos({40, 140}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Character   ( C )", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 4});
        ImGui::BeginGroup();
        for (int s = SlotHead; s <= SlotFeet; ++s) slot(p.inv, {SlotRef::Equip, s}, kSlotNames[s]);
        ImGui::EndGroup();
        ImGui::SameLine(0, 18);
        ImGui::BeginGroup();
        attributes(p, armorK);
        ImGui::EndGroup();
        slot(p.inv, {SlotRef::Equip, SlotMainHand}, kSlotNames[SlotMainHand]);
        ImGui::SameLine();
        slot(p.inv, {SlotRef::Equip, SlotOffHand}, kSlotNames[SlotOffHand]);
        ImGui::SameLine();
        slot(p.inv, {SlotRef::Equip, SlotTrinket}, kSlotNames[SlotTrinket]);
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

void bagsWindow(bool& open, Player& p) {
    if (!open) return;
    Inventory& inv = p.inv;
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({screen.x - 30, screen.y - 90}, ImGuiCond_FirstUseEver, {1, 1});
    if (ImGui::Begin("Bags   ( B )", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 4});
        for (int c = 0; c <= kBagBarSlots; ++c) {
            if (inv.containers[size_t(c)].empty()) continue;
            ImGui::SeparatorText(c ? itemType(inv.bags[size_t(c) - 1]).name : "Backpack");
            for (int i = 0; i < int(inv.containers[size_t(c)].size()); ++i) {  // a drop can resize containers
                if (i % kColumns) ImGui::SameLine();
                slot(inv, {SlotRef::Item, i, c}, nullptr);
            }
        }
        ImGui::SeparatorText("Bag slots");
        for (int b = 0; b < kBagBarSlots; ++b) {
            if (b) ImGui::SameLine();
            slot(inv, {SlotRef::Bag, b}, "Bag");
        }
        ImGui::TextDisabled("%d / %d slots free", freeSlots(inv), itemSlots(inv));
        ImGui::PopStyleVar();
    }
    ImGui::End();
}

void journalWindow(bool& open, const Session& s) {
    if (!open) return;
    const Defs& d = defs();
    ImGui::SetNextWindowPos({40, 140}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({460, 420}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Journal   ( J )", &open, ImGuiWindowFlags_NoCollapse)) {
        auto quests = [&](const char* heading, auto shows) {
            ImGui::SeparatorText(heading);
            bool any = false;
            for (size_t q = 0; q < std::min(s.quests.size(), d.quests.size()); ++q) {
                const QuestState& st = s.quests[q];
                if (st.stage == 0 || !shows(st)) continue;
                any = true;
                const QuestDef& def = d.quests[q];
                ImGui::PushID(int(q));
                if (!st.active()) {
                    ImGui::TextDisabled("%s", def.title.c_str());
                } else if (ImGui::TreeNodeEx(def.title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    if (const Stage* stage = findStage(def, st.stage); stage && !stage->journal.empty())
                        ImGui::TextWrapped("%s", stage->journal.c_str());
                    for (size_t o = 0; o < std::min(st.objectives.size(), def.objectives.size()); ++o) {
                        ObjectiveState os = st.objectives[o];
                        if (os == ObjectiveState::Hidden) continue;
                        bool done = os == ObjectiveState::Completed;
                        ImGui::PushID(int(o));
                        ImGui::BeginDisabled(os != ObjectiveState::Shown);  // done and failed ones are dimmed
                        ImGui::Checkbox("##done", &done);  // a copy: clicking changes nothing
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s%s", def.objectives[o].text.c_str(), os == ObjectiveState::Failed ? "  (failed)" : "");
                        ImGui::EndDisabled();
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("None");
        };
        quests("Active", [](const QuestState& st) { return st.active(); });
        quests("Completed", [](const QuestState& st) { return st.done; });
        quests("Failed", [](const QuestState& st) { return st.failed; });
    }
    ImGui::End();
}

namespace {

// "+3% damage (one-handed)": what a modifier does, for tooltips.
std::string describe(const Modifier& m, int times) {
    float v = m.value * float(times);
    const char* how = m.kind == ModKind::More ? (v < 0 ? "less" : "more") : (v < 0 ? "reduced" : "increased");
    std::string out = m.kind == ModKind::Flat ? std::format("{:+g} {}", double(m.stat == Stat::Haste ? v * 100 : v),
                                                           m.stat == Stat::Haste ? "% attack speed" : kStatNames[size_t(m.stat)])
                                              : std::format("{:g}% {} {}", double(std::abs(v) * 100), how, kStatNames[size_t(m.stat)]);
    if (m.with != Skill::Count) out += std::format(" ({} hits)", skillDef(m.with).name);
    return out;
}

void bar(float fraction, const std::string& overlay) {
    ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), {-FLT_MIN, 0}, overlay.c_str());
}

}  // namespace

void progressWindow(bool& open, Player& p) {
    if (!open) return;
    const Defs& d = defs();
    Progress& g = p.progress;
    ImGui::SetNextWindowPos({120, 100}, ImGuiCond_FirstUseEver);  // not on top of the other windows' spot
    ImGui::SetNextWindowSize({720, 520}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Progress   ( K )", &open, ImGuiWindowFlags_NoCollapse)) {
        int points = talentPoints(g, p.level);
        ImGui::Text("Level %d", p.level);
        ImGui::SameLine(0, 24);
        if (points > 0) ImGui::TextColored({0.45f, 0.9f, 0.45f, 1}, "%d talent point%s to spend", points, points == 1 ? "" : "s");
        else if (points < 0) ImGui::TextColored({0.95f, 0.5f, 0.35f, 1}, "%d point%s over budget", -points, points == -1 ? "" : "s");
        else ImGui::TextDisabled("No talent points (one per level)");
        bar(p.level >= kMaxLevel ? 1 : float(g.xp) / float(levelXpToNext(p.level)),
            p.level >= kMaxLevel ? std::string("Highest level") : std::format("{} / {} XP", g.xp, levelXpToNext(p.level)));
        ImGui::SetItemTooltip("Character XP: kills, and every skill level gained (its new level number)");
        ImGui::SeparatorText("Skills");
        if (ImGui::BeginTable("skills", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 150);
            for (size_t k = 0; k < size_t(Skill::Count); ++k) {
                const SkillDef& sd = d.skills[k];
                const SkillLevel& sk = g.skills[k];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(), ImGui::Text("%s %d", sd.name.c_str(), sk.level);
                ImGui::TableNextColumn();
                float cost = skillXpToNext(sd.improveMult, sd.improveOffset, sk.level);
                bar(sk.level >= kMaxSkill ? 1 : sk.xp / cost, sk.level >= kMaxSkill ? std::string("Mastered") : std::format("{:.0f} / {:.0f}", sk.xp, cost));
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted(k == size_t(Skill::Armor) ? "Improves by taking hits while wearing armor"
                                           : k == size_t(Skill::Unarmed) ? "Improves by punching foes" : "Improves by hitting foes with a one-handed weapon");
                    for (const Modifier& m : sd.perLevel) ImGui::BulletText("%s per level (now %s)", describe(m, 1).c_str(), describe(m, sk.level - kMinSkill).c_str());
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Talents");
        if (ImGui::BeginTable("trees", int(Skill::Count), ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            for (size_t k = 0; k < size_t(Skill::Count); ++k) {
                ImGui::TableNextColumn();
                ImGui::Text("%s  (%d)", d.skills[k].name.c_str(), pointsSpent(g, Skill(k)));
                for (int tier = 0; tier <= kMaxTier; ++tier) {
                    bool any = false;
                    for (size_t t = 0; t < d.talents.size(); ++t) {
                        const TalentDef& td = d.talents[t];
                        if (size_t(td.tree) != k || td.tier != tier) continue;
                        if (!any) ImGui::TextDisabled("Tier %d%s", tier + 1, tier ? std::format(": {} points", kPointsPerTier * tier).c_str() : "");
                        any = true;
                        const char* why = nullptr;
                        bool learnable = canLearn(g, p.level, t, &why);
                        int r = rank(g, t);
                        // Maxed gold, learnable green, started blue, locked dark.
                        ImU32 color = r >= td.maxRank ? uiColor(0.55f, 0.42f, 0.12f) : learnable ? uiColor(0.18f, 0.48f, 0.2f)
                                      : r > 0 ? uiColor(0.25f, 0.32f, 0.45f) : uiColor(0.16f, 0.16f, 0.19f);
                        ImGui::PushStyleColor(ImGuiCol_Button, color);
                        ImGui::PushID(int(t));
                        if (ImGui::Button(std::format("{}  {}/{}", td.name, r, td.maxRank).c_str(), {-FLT_MIN, 0})) learn(g, p.level, t);
                        ImGui::PopID();
                        ImGui::PopStyleColor();
                        if (ImGui::BeginItemTooltip()) {
                            ImGui::Text("%s, rank %d of %d", td.name.c_str(), r, td.maxRank);
                            for (const Modifier& m : td.perRank) ImGui::BulletText("%s per rank", describe(m, 1).c_str());
                            if (td.prerequisite >= 0) ImGui::TextDisabled("Needs %s at its highest rank", d.talents[size_t(td.prerequisite)].name.c_str());
                            if (why) ImGui::TextColored({0.95f, 0.6f, 0.4f, 1}, "Can't learn: %s", why);
                            else ImGui::TextColored({0.45f, 0.9f, 0.45f, 1}, "Click to learn");
                            ImGui::EndTooltip();
                        }
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
