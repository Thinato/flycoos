#pragma once
#include "player.h"

struct Session;

// In-game windows: Character (C: equipment and attributes), Bags (B: backpack, bags, bag bar),
// Journal (J: quests) and Progress (K: level, skills, talents; ADR-027).
// Drag items between slots; right-click equips or unequips; hover for details.
// Call between DebugUI::begin and DebugUI::end; `open` is cleared by the window's close button.
void characterWindow(bool& open, Player& player, float armorK);  // armorK: see armorReduction
void bagsWindow(bool& open, Player& player);
// Active quests (the current stage's journal text and the shown objectives, checked when done),
// then completed and failed ones.
void journalWindow(bool& open, const Session& s);
// Level and XP, the skills with their progress, and a talent tree per skill: click a learnable
// talent (highlighted) to spend a point on it.
void progressWindow(bool& open, Player& player);
