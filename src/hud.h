#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "entity.h"

struct ImFont;
// Region titles' font (Alagard, data/fonts/), set by DebugUI; null (the default font) if it didn't load.
inline ImFont* titleFont = nullptr;

// An sRGB color as ImGui's packed ImU32, for the linear (sRGB-format) swapchain.
unsigned uiColor(float r, float g, float b, float a = 1);

// In-game overlay on ImGui's background draw list: NPC nameplates (level, name, health bar),
// speech balloons, damage numbers, the "[E] Talk"/"[E] Loot" hint over `target`, the player's
// health bar with its flask charges beside it (ADR-047), the readied throwable and its count (ADR-053) and, with a bow, the quiver's arrows and the
// aim mode's crosshair (ADR-050), the knocked-out message, the lock-on dot on
// `locked`'s chest, and "[E] Rest" when `rest` (canRest) and there's no `target`.
// Bodies are where poseModel draws them `tickAlpha` into the tick. Call between DebugUI::begin and DebugUI::end.
void drawHud(const glm::mat4& view, float fovY, glm::vec3 eye, const Grid& world, const Player& player,
             const std::vector<Entity>& entities, const Entity* target, float tickAlpha, const Entity* locked = nullptr,
             bool rest = false);
// The boss bar (ADR-059), at the bottom over the player's bars: the name and health of the boss fighting the player
// (bossBar), a chip of recent damage that drains after a moment, and its stance filling toward a break. A killed boss's
// bar lingers empty and fades; one that leaves the fight takes its bar with it.
void drawBossBar(const std::vector<Entity>& entities, const Player& player);
// Place names: the label of every poi whose radius the player is within (fading in over its outer
// quarter), and `region`'s name for a few seconds after the player walked into it `regionSeconds` ago.
void drawPlaces(const glm::mat4& view, float fovY, const Player& player, const std::vector<Marker>& markers,
                const Marker* region, float regionSeconds);
// A short message at the top of the screen (e.g. "Game saved"), shown for a few seconds after it
// came `age` seconds ago; nothing for an empty one.
void drawNotice(const char* text, float age);
