#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "entity.h"

// In-game overlay on ImGui's background draw list: NPC nameplates (level, name, health bar),
// speech balloons, the "[E] Talk" hint and the player's health bar.
// Call between DebugUI::begin and DebugUI::end.
void drawHud(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const Player& player,
             const std::vector<Entity>& entities, const Entity* talkTarget);
