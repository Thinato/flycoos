#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "entity.h"

// An sRGB color as ImGui's packed ImU32, for the linear (sRGB-format) swapchain.
unsigned uiColor(float r, float g, float b, float a = 1);

// In-game overlay on ImGui's background draw list: NPC nameplates (level, name, health bar),
// speech balloons, damage numbers, the "[E] Talk"/"[E] Loot" hint over `target`, the player's
// health bar and the knocked-out message.
// Call between DebugUI::begin and DebugUI::end.
void drawHud(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const Player& player,
             const std::vector<Entity>& entities, const Entity* target);
// Place names: the label of every poi whose radius the player is within (fading in over its outer
// quarter), and `region`'s name for a few seconds after the player walked into it `regionSeconds` ago.
void drawPlaces(const glm::mat4& view, float fovY, const Player& player, const std::vector<Marker>& markers,
                const Marker* region, float regionSeconds);
