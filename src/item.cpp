#include "item.h"

const char* const kSlotNames[EquipCount] = {"Head", "Chest", "Hands", "Legs", "Feet", "Main Hand", "Off Hand"};

namespace {

constexpr ItemType kItems[ItemCount] = {
    // name              slot          attack         bag slots  armor
    {"",                 SlotNone,     {0, 0, 0},     0,         0},
    {"Iron Shortsword",  SlotMainHand, {6, 10, 1.0f}, 0,         0},
    {"Canvas Satchel",   SlotBag,      {0, 0, 0},     6,         0},
    {"Rusty Knife",      SlotMainHand, {3, 6, 0.75f}, 0,         0},
    {"Boar Skin",        SlotNone,     {0, 0, 0},     0,         0},
    {"Iron Helm",        SlotHead,     {0, 0, 0},     0,         6},
    {"Iron Cuirass",     SlotChest,    {0, 0, 0},     0,         12},
    {"Iron Gauntlets",   SlotHands,    {0, 0, 0},     0,         4},
    {"Iron Greaves",     SlotLegs,     {0, 0, 0},     0,         10},
    {"Iron Sabatons",    SlotFeet,     {0, 0, 0},     0,         5},
    {"Wolf Pelt",        SlotNone,     {0, 0, 0},     0,         0},
    {"Bone Charm",       SlotNone,     {0, 0, 0},     0,         0},  // a trinket; no slot for it yet
};

bool bagEmpty(const Inventory& inv, int bar) {
    for (ItemId id : inv.containers[size_t(bar) + 1])
        if (id != NoItem) return false;
    return true;
}

bool accepts(SlotRef s, ItemId id) {
    if (id == NoItem || s.kind == SlotRef::Item) return true;
    return itemType(id).slot == (s.kind == SlotRef::Bag ? SlotBag : EquipSlot(s.index));
}

}  // namespace

const ItemType& itemType(ItemId id) { return kItems[id < ItemCount ? id : NoItem]; }

ItemId* slotAt(Inventory& inv, SlotRef r) {
    switch (r.kind) {
    case SlotRef::Equip: return r.index >= 0 && r.index < EquipCount ? &inv.equipped[size_t(r.index)] : nullptr;
    case SlotRef::Bag: return r.index >= 0 && r.index < kBagBarSlots ? &inv.bags[size_t(r.index)] : nullptr;
    case SlotRef::Item:
        if (r.container < 0 || r.container > kBagBarSlots) return nullptr;
        auto& c = inv.containers[size_t(r.container)];
        return r.index >= 0 && size_t(r.index) < c.size() ? &c[size_t(r.index)] : nullptr;
    }
    return nullptr;
}

bool moveItem(Inventory& inv, SlotRef from, SlotRef to) {
    ItemId *a = slotAt(inv, from), *b = slotAt(inv, to);
    if (!a || !b || a == b || *a == NoItem) return false;
    if (!accepts(to, *a) || !accepts(from, *b)) return false;
    for (SlotRef s : {from, to})
        if (s.kind == SlotRef::Bag && !bagEmpty(inv, s.index)) return false;
    for (auto [bag, slot] : {std::pair{from, to}, std::pair{to, from}})
        if (bag.kind == SlotRef::Bag && slot.kind == SlotRef::Item && slot.container == bag.index + 1) return false;
    std::swap(*a, *b);
    for (SlotRef s : {from, to})  // a swapped bag brings its own (empty) slots
        if (s.kind == SlotRef::Bag)
            inv.containers[size_t(s.index) + 1].assign(size_t(itemType(inv.bags[size_t(s.index)]).bagSlots), NoItem);
    return true;
}

bool quickMove(Inventory& inv, SlotRef from) {
    ItemId* item = slotAt(inv, from);
    if (!item || *item == NoItem) return false;
    if (from.kind == SlotRef::Item) {
        EquipSlot slot = itemType(*item).slot;
        if (slot < EquipCount) return moveItem(inv, from, {SlotRef::Equip, slot});
        if (slot == SlotBag)
            for (int i = 0; i < kBagBarSlots; ++i)  // the first free bar slot
                if (inv.bags[size_t(i)] == NoItem) return moveItem(inv, from, {SlotRef::Bag, i});
        return false;
    }
    for (int c = 0; c <= kBagBarSlots; ++c)  // the first free item slot that will take it
        for (int i = 0; i < int(inv.containers[size_t(c)].size()); ++i)
            if (inv.containers[size_t(c)][size_t(i)] == NoItem && moveItem(inv, from, {SlotRef::Item, i, c}))
                return true;
    return false;
}

bool giveItem(Inventory& inv, ItemId id) {
    if (id == NoItem || id >= ItemCount) return false;
    for (auto& c : inv.containers)
        for (ItemId& slot : c)
            if (slot == NoItem) {
                slot = id;
                return true;
            }
    return false;
}

int itemSlots(const Inventory& inv) {
    int n = 0;
    for (const auto& c : inv.containers) n += int(c.size());
    return n;
}

int freeSlots(const Inventory& inv) {
    int n = 0;
    for (const auto& c : inv.containers)
        for (ItemId id : c) n += id == NoItem;
    return n;
}

int armor(const Inventory& inv) {
    int n = 0;
    for (ItemId id : inv.equipped) n += itemType(id).armor;
    return n;
}

const Attack& weaponAttack(ItemId weapon) { return weapon != NoItem ? itemType(weapon).attack : kPunch; }
