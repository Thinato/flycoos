#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "stats.h"

// Where an item is worn. Bags go in the bag bar instead.
// Player saves store the equipment by slot index (ADR-025): only append slots (SlotBag and SlotNone
// aren't saved and may move).
enum EquipSlot : uint8_t { SlotHead, SlotChest, SlotHands, SlotLegs, SlotFeet, SlotMainHand, SlotOffHand, SlotTrinket,
                           EquipCount, SlotBag = EquipCount, SlotNone };
extern const char* const kSlotNames[EquipCount];

// Item save ids (the `save_id` of their record in data/items.json): append-only, never renumbered.
// These are the ones code names; data may add more after ItemCount.
enum ItemId : uint8_t { NoItem, Shortsword, Satchel, Knife, BoarSkin,
                        IronHelm, IronCuirass, IronGauntlets, IronGreaves, IronSabatons, WolfPelt, BoneCharm, ItemCount };

// What a physical hit does (ADR-040): a blade's swings slash and its thrusts pierce, fists and blunt
// weapons strike. Creatures resist (or are weak to) each apart, and shields stop each apart.
enum class DamageType : uint8_t { Slash, Strike, Pierce, Count };
inline constexpr const char* kDamageTypeIds[] = {"slash", "strike", "pierce"};  // in data files
inline constexpr const char* kDamageTypeNames[] = {"Slash", "Strike", "Pierce"};
static_assert(std::size(kDamageTypeIds) == size_t(DamageType::Count) && std::size(kDamageTypeNames) == size_t(DamageType::Count));
using PerType = std::array<float, size_t(DamageType::Count)>;  // a number for each damage type

// What kind of weapon it is (ADR-044): it picks the moveset (data/weapon_classes.json), the swing
// poses and the held model. Code asks a weapon's class, never its item id. Bare hands are Fists.
// Greatswords and greataxes (ADR-049) are held in both hands. Only append classes.
enum class WeaponClass : uint8_t { Fists, Dagger, Sword, Axe, Mace, Greatsword, Greataxe, Count };
inline constexpr const char* kWeaponClassIds[] = {"fists", "dagger", "sword", "axe", "mace", "greatsword", "greataxe"};  // in data files
static_assert(std::size(kWeaponClassIds) == size_t(WeaponClass::Count));

// What an off-hand shield does when guarding (ADR-036): the share of a blocked hit's damage it stops
// (per damage type, ADR-040) and of its stamina damage it saves, and whether it can parry. Its size
// picks its model.
enum class ShieldSize : uint8_t { None, Small, Medium, Great };
struct Shield {
    ShieldSize size = ShieldSize::None;  // None: not a shield
    PerType negation{};
    float boost = 0;
    float parry = 0;                     // the parry's length (x its base timing); 0: can't parry
};

struct ItemType {
    const char* name;
    EquipSlot slot;
    Attack attack;  // weapons
    int bagSlots;   // bags
    int armor;      // armor pieces; chest and legs always give the most (a test enforces it)
    std::vector<Modifier> mods;  // what wearing it adds to the stat pipeline, besides its armor (source unset)
    Shield shield;               // off-hand shields
    float weight = 0;            // worn, it counts toward the equip load (ADR-039)
    DamageType damage = DamageType::Slash;  // weapons: what their swings do (a blade's thrusts pierce)
    float parry = 0;             // main-hand weapons that parry (a knife: ADR-041): the parry's length; 0: can't
    WeaponClass weaponClass = WeaponClass::Fists;  // weapons: their class (ADR-044)
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
// giveItem, or with the bags full into its empty equip slot (a bag: an empty bag bar slot). Loot and quest rewards.
bool stowItem(Inventory& inv, ItemId id);
bool canStow(const Inventory& inv, ItemId id);
// Removes one `id`: from the item slots first, then worn. False if there's none.
bool takeItem(Inventory& inv, ItemId id);
// Destroys what's in `ref` (Ctrl+right-click): an item slot's or a worn item, or an empty bag from the bar.
bool discardItem(Inventory& inv, SlotRef ref);
int itemSlots(const Inventory& inv);
// What the worn items weigh (the equip load, ADR-039; bags and what's in them don't count).
float equipWeight(const Inventory& inv);
int freeSlots(const Inventory& inv);

// A weapon's class; Fists for NoItem.
inline WeaponClass classOf(ItemId weapon) { return itemType(weapon).weaponClass; }
// A weapon's attack; a punch for NoItem.
const Attack& weaponAttack(ItemId weapon);
// What a left click does: the main-hand weapon's attack, or a punch.
inline const Attack& mainAttack(const Inventory& inv) { return weaponAttack(inv.equipped[SlotMainHand]); }
