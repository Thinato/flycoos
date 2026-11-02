#include "inventory_ui.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "hud.h"

namespace {

constexpr float kSlot = 46;  // pixels
constexpr int kColumns = 6;  // item slots per row in the bags window

// Item icons are drawn with lines and shapes, colored like their voxels.
void icon(ImDrawList* dl, ItemId id, ImVec2 lo, float s) {
    auto at = [&](float x, float y) { return ImVec2(lo.x + x * s, lo.y + y * s); };
    ImU32 steel = uiColor(0.78f, 0.80f, 0.84f), iron = uiColor(0.30f, 0.30f, 0.33f), leather = uiColor(0.35f, 0.22f, 0.12f);
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
    default: break;
    }
}

void tooltip(ItemId id, SlotRef where) {
    const ItemType& t = itemType(id);
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(t.name);
    if (t.slot == SlotBag) ImGui::Text("%d slot bag", t.bagSlots);
    else if (t.slot < EquipCount) ImGui::TextDisabled("%s", kSlotNames[t.slot]);
    const Attack& a = t.attack;
    if (a.speed > 0) {
        ImGui::Text("%d - %d damage    Speed %.2f", a.minDamage, a.maxDamage, a.speed);
        ImGui::Text("(%.1f damage per second)", float(a.minDamage + a.maxDamage) / 2 / a.speed);
    }
    if (t.slot != SlotNone) ImGui::TextDisabled(where.kind == SlotRef::Item ? "Right-click to equip" : "Right-click to unequip");
    ImGui::EndTooltip();
}

// One square slot: drag its item onto another slot to move or swap it, right-click to equip or
// unequip. Empty slots show `label` (if any) in faint text.
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
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) quickMove(inv, ref);
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

void attributes(const Player& p) {
    const Stats& s = p.stats;
    const Attack& a = mainAttack(p.inv);
    float swing = swingTime(s, a);
    int lo = a.minDamage + strengthBonus(s, a), hi = a.maxDamage + strengthBonus(s, a);
    char tip[96];
    if (!ImGui::BeginTable("attributes", 2, ImGuiTableFlags_SizingFixedFit)) return;
    statRow("Health", nullptr, "%d / %d", int(std::ceil(p.health)), maxHealth(s));
    std::snprintf(tip, sizeof tip, "+%d max health per point", kHealthPerStamina);
    statRow("Stamina", tip, "%d", s.stamina);
    statRow("Strength", "Adds strength x weapon speed to every hit", "%d", s.strength);
    std::snprintf(tip, sizeof tip, "+%.0f%% attack speed per point", kHastePerAgility * 100);
    statRow("Agility", tip, "%d", s.agility);
    statRow("Intellect", "No use yet", "%d", s.intellect);
    std::snprintf(tip, sizeof tip, "+%.1f health per second out of combat per point", kRegenPerSpirit);
    statRow("Spirit", tip, "%d", s.spirit);
    ImGui::TableNextRow();
    ImGui::TableNextRow();
    ItemId weapon = p.inv.equipped[SlotMainHand];
    statRow("Damage", weapon ? itemType(weapon).name : "Unarmed", "%d - %d", lo, hi);
    statRow("Attack speed", "Seconds per swing", "%.2f s", swing);
    statRow("Damage/sec", nullptr, "%.1f", float(lo + hi) / 2 / swing);
    statRow("Regen", "Out of combat", "%.1f /s", healthRegen(s));
    ImGui::EndTable();
}

}  // namespace

void characterWindow(bool& open, Player& p) {
    if (!open) return;
    ImGui::SetNextWindowPos({40, 140}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Character   ( C )", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 4});
        ImGui::BeginGroup();
        for (int s = SlotHead; s <= SlotFeet; ++s) slot(p.inv, {SlotRef::Equip, s}, kSlotNames[s]);
        ImGui::EndGroup();
        ImGui::SameLine(0, 18);
        ImGui::BeginGroup();
        attributes(p);
        ImGui::EndGroup();
        slot(p.inv, {SlotRef::Equip, SlotMainHand}, kSlotNames[SlotMainHand]);
        ImGui::SameLine();
        slot(p.inv, {SlotRef::Equip, SlotOffHand}, kSlotNames[SlotOffHand]);
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
