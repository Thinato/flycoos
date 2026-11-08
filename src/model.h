#pragma once
#include <array>

#include <glm/glm.hpp>

#include "item.h"
#include "world.h"

struct Player;

// Character models: 6 floating voxel parts, each its own tiny grid meshed by meshRegion.
// The training dummy reuses the slots: FootL is its post, FootR its base, hands its arms.
enum Part { Head, Torso, HandL, HandR, FootL, FootR, PartCount };
// The boar and the wolf reuse them as quadrupeds: hands are front legs, feet hind legs.
// Weapons and shields are held items: only their Head slot has voxels, posed by poseModel's `held`
// and `shield` outputs.
// Armor is a humanoid whose parts are plated: a worn piece swaps in the part it covers.
// The guard, the bandit chief and the ghoul are humanoids (the chief scaled up, the ghoul thinner).
enum Model { ModelPlayer, ModelVillager, ModelDummy, ModelBoar, ModelBandit, ModelSword, ModelKnife, ModelIronArmor,
             ModelWolf, ModelGuard, ModelChief, ModelGhoul, ModelBuckler, ModelHeater, ModelTower, ModelCount };
Grid buildPart(Model model, Part part);
Model heldModel(ItemId weapon);
Model shieldModel(ItemId shield);  // an off-hand shield's, by its size (ADR-036)
// Which model each part of `model` is drawn from: its own, or the armor worn over it. Greaves have
// no part (the rig has no legs), so they don't show.
std::array<Model, PartCount> partModels(Model model, const Inventory& inv);
// Part-grid space -> world space for the body's current animation pose (the dummy only wobbles
// when hit and topples when out of health; everyone else falls over). With a main-hand weapon
// equipped, a humanoid's `held` gets its pose (gripped in the right hand) and attacks are slashes
// instead of punches; the boar and the wolf lunge with their heads.
// `alpha`: where between its last two ticks the body is drawn (see drawPos). With a shield in the off
// hand, `shield` gets its pose (strapped to the left forearm: at the side, raised by the guard, swept
// out by a parry).
std::array<glm::mat4, PartCount> poseModel(const Player& body, Model model, glm::mat4* held = nullptr, float alpha = 1,
                                           glm::mat4* shield = nullptr);
