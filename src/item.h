#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "stats.h"

// Where an item is worn. Bags go in the bag bar instead.
// Not saved anywhere yet (data files name slots), so appending one may shift SlotBag and SlotNone.
enum EquipSlot : uint8_t { SlotHead, SlotChest, SlotHands, SlotLegs, SlotFeet, SlotMainHand, SlotOffHand, SlotTrinket,
                           EquipCount, SlotBag = EquipCount, SlotNone };
extern const char* const kSlotNames[EquipCount];

// Item save ids (the `save_id` of their record in data/items.json): append-only, never renumbered.
// These are the ones code names; data may add more after ItemCount.
enum ItemId : uint8_t { NoItem, Shortsword, Satchel, Knife, BoarSkin,
                        IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, WolfPelt, BoneCharm, ItemCount };

struct ItemType {
    const char* name;
    EquipSlot slot;
    Attack attack;  // weapons
    int bagSlots;   // bags
    int armor;      // armor pieces; chest and legs always give the most (a test enforces it)
    std::vector<Modifier> mods;  // what wearing it adds to the stat pipeline, besides its armor (source unset)
};
const ItemType& itemType(ItemId id);  // from the loaded defs (defs.h); NoItem's row when out of range

constexpr int kBackpackSlots = 12;  // always there, before any bag
constexpr int kBagBarSlots = 4;

// Equipment, the bag bar, and the item slots: the backpack plus one container per equipped bag.
struct Inventory {
    std::array<ItemId, EquipCount> equipped{};
    std::array<ItemId, kBagBarSlots> bags{};
    std::array<std::vector<ItemId>, 1 + kBagBarSlots> containers{std::vector<ItemId>(kBackpackSlots)};
};

// One slot of an inventory. Bag `index` of the bar holds container `index + 1`.
struct SlotRef {
    enum Kind : uint8_t { Equip, Bag, Item } kind;
    int index = 0;      // equip slot, bag bar slot, or slot within the container
    int container = 0;  // Item only: 0 = backpack
    bool operator==(const SlotRef&) const = default;
};

ItemId* slotAt(Inventory& inv, SlotRef ref);  // null if out of range
// Moves (or swaps) an item between two slots if both ends accept what they'd get. A bag leaves
// the bar only when empty, and never into its own slots.
bool moveItem(Inventory& inv, SlotRef from, SlotRef to);
// Right-click: equips gear and bags from the item slots, or puts equipped things back into them.
bool quickMove(Inventory& inv, SlotRef from);
bool giveItem(Inventory& inv, ItemId id);  // into the first free item slot
int itemSlots(const Inventory& inv);
int freeSlots(const Inventory& inv);

// A weapon's attack; a punch for NoItem.
const Attack& weaponAttack(ItemId weapon);
// What a left click does: the main-hand weapon's attack, or a punch.
inline const Attack& mainAttack(const Inventory& inv) { return weaponAttack(inv.equipped[SlotMainHand]); }
