#pragma once
#include "player.h"

// In-game windows: Character (C: equipment and attributes) and Bags (B: backpack, bags, bag bar).
// Drag items between slots; right-click equips or unequips; hover for details.
// Call between DebugUI::begin and DebugUI::end; `open` is cleared by the window's close button.
void characterWindow(bool& open, Player& player, float armorK);  // armorK: see armorReduction
void bagsWindow(bool& open, Player& player);
